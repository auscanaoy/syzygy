# Copyright 2015 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os.path
import string


_HEADER = """\
{c} Copyright 2015 Google Inc. All Rights Reserved.
{c}
{c} Licensed under the Apache License, Version 2.0 (the "License");
{c} you may not use this file except in compliance with the License.
{c} You may obtain a copy of the License at
{c}
{c}     http://www.apache.org/licenses/LICENSE-2.0
{c}
{c} Unless required by applicable law or agreed to in writing, software
{c} distributed under the License is distributed on an "AS IS" BASIS,
{c} WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
{c} See the License for the specific language governing permissions and
{c} limitations under the License.

{c} This file is generated by generate_memory_interceptors.py, DO NOT MODIFY.
"""

_ASM_HEADER = """\
.386
.MODEL FLAT, C

.CODE

; Declare the tail function all the stubs direct to.
EXTERN C asan_redirect_tail:PROC
"""


_PROC_HEADER = """\

; Declare a single top-level function to prevent identical code folding from
; folding the redirectors into one. Each redirector simply calls through to
; the tail function. This allows the tail function to trivially compute the
; redirector's address, which is used to identify the invoked redirector.
asan_redirectors PROC
"""

_PROC_TRAILER = """\
asan_redirectors ENDP

END
"""


# Generates the single-instance assembly stubs.
AsanGlobalFunctions = """\
// On entry, the address to check is in EDX and the previous contents of
// EDX are on stack. On exit the previous contents of EDX have been restored
// and popped off the stack. This function modifies no other registers,
// in particular it saves and restores EFLAGS.
extern "C" __declspec(naked)
void asan_no_check() {{
  __asm {{
    // Restore EDX.
    mov edx, DWORD PTR[esp + 4]
    // And return.
    ret 4
  }}
}}

// No state is saved for string instructions.
extern "C" __declspec(naked)
void asan_string_no_check() {{
  __asm {{
    // Just return.
    ret
  }}
}}

// On entry, the address to check is in EDX and the stack has:
// - previous contents of EDX.
// - return address to original caller.
// - return address to redirection stub.
extern "C" __declspec(naked)
void asan_redirect_tail() {{
  __asm {{
    // Prologue, save context.
    pushfd
    pushad
    // Compute the address of the calling function and push it.
    mov eax, DWORD PTR[esp + 9 * 4]
    sub eax, 5  // Length of call instruction.
    push eax
    // Push the original caller's address.
    push DWORD PTR[esp + 11 * 4]
    call agent::asan::RedirectStubEntry
    // Clean arguments off the stack.
    add esp, 8

    // Overwrite access_size with the stub to return to.
    mov DWORD PTR[esp + 9 * 4], eax

    // Restore context.
    popad
    popfd

    // return to the stashed stub.
    ret
  }}
}}
"""


# Starts by saving EAX onto the stack and then loads the value of
# the flags into it.
#
# This is a trick for efficient saving/restoring part of the flags register.
# See http://blog.freearrow.com/archives/396.
# Flags (bits 16-31) probably need a pipeline flush on update (POPFD). Thus,
# using LAHF/SAHF instead gives better performance.
#   PUSHFD/POPFD: 23.314684 ticks
#   LAHF/SAHF:     8.838665 ticks
_ASAN_SAVE_EFLAGS = """\
    // Save the EFLAGS.
    push eax
    lahf
    seto al"""


# Restores the flags.
#
# The previous flags value is assumed to be in EAX and we expect to have the
# previous value of EAX on the top of the stack.
# AL is set to 1 if the overflow flag was set before the call to our hook, 0
# otherwise. We add 0x7f to it so it'll restore the flag. Then we restore the
# low bytes of the flags and EAX.
_ASAN_RESTORE_EFLAGS = """\
    // Restore the EFLAGS.
    add al, 0x7f
    sahf
    pop eax"""


# The common part of the fast path shared between the different
# implementations of the hooks.
#
# This does the following:
#   - Saves the memory location in EDX for the slow path.
#   - Checks if the address we're trying to access is signed, if so this mean
#       that this is an access to the upper region of the memory (over the
#       2GB limit) and we should report this as a wild access.
#   - Checks for zero shadow for this memory location. We use the cmp
#       instruction so it'll set the sign flag if the upper bit of the shadow
#       value of this memory location is set to 1.
#   - If the shadow byte is not equal to zero then it jumps to the slow path.
#   - Otherwise it removes the memory location from the top of the stack.
_ASAN_FAST_PATH = """\
    push edx
    sar edx, 3
    js report_failure
    movzx edx, BYTE PTR[edx + {shadow}]
    cmp dl, 0
    jnz check_access_slow
    add esp, 4"""


# This is the common part of the slow path shared between the different
# implementations of the hooks.
#
# The memory location is expected to be on top of the stack and the shadow
# value for it is assumed to be in DL at this point.
# This also relies on the fact that the shadow non accessible byte mask has
# its upper bit set to 1 and that we jump to this macro after doing a
# "cmp shadow_byte, 0", so the sign flag would be set to 1 if the value isn't
# accessible.
# We inline the Shadow::IsAccessible function for performance reasons.
# This function does the following:
#     - Checks if this byte is accessible and jump to the error path if it's
#       not.
#     - Removes the memory location from the top of the stack.
_ASAN_SLOW_PATH = """\
    js report_failure
    mov dh, BYTE PTR[esp]
    and dh, 7
    cmp dh, dl
    jae report_failure
    add esp, 4"""


# The error path.
#
# It expects to have the previous value of EDX at [ESP + 4] and the address
# of the faulty instruction at [ESP].
# This macro takes care of saving and restoring the flags.
_ASAN_ERROR_PATH ="""\
    // Restore original value of EDX, and put memory location on stack.
    xchg edx, DWORD PTR[esp + 4]
    // Create an Asan registers context on the stack.
    pushfd
    pushad
    // Fix the original value of ESP in the Asan registers context.
    // Removing 12 bytes (e.g. EFLAGS / EIP / Original EDX).
    add DWORD PTR[esp + 12], 12
    // Push ARG4: the address of Asan context on stack.
    push esp
    // Push ARG3: the access size.
    push {access_size}
    // Push ARG2: the access type.
    push {access_mode_value}
    // Push ARG1: the memory location.
    push DWORD PTR[esp + 52]
    call agent::asan::ReportBadMemoryAccess
    // Remove 4 x ARG on stack.
    add esp, 16
    // Restore original registers.
    popad
    popfd
    // Return and remove memory location on stack.
    ret 4"""


_MACROS = {
  "AsanSaveEflags": _ASAN_SAVE_EFLAGS,
  "AsanRestoreEflags": _ASAN_RESTORE_EFLAGS,
  "AsanFastPath": _ASAN_FAST_PATH,
  "AsanSlowPath": _ASAN_SLOW_PATH,
  "AsanErrorPath": _ASAN_ERROR_PATH,
}


# Generates the Asan check access functions.
#
# The name of the generated method will be
# asan_check_(@p access_size)_byte_(@p access_mode_str)().
#
# Args:
#   access_size: The size of the access (in byte).
#   access_mode_str: The string representing the access mode (read_access
#       or write_access).
#   access_mode_value: The internal value representing this kind of
#       access.
AsanCheckFunction = """\
// On entry, the address to check is in EDX and the previous contents of
// EDX are on stack. On exit the previous contents of EDX have been restored
// and popped off the stack. This function modifies no other registers,
// in particular it saves and restores EFLAGS.
extern "C" __declspec(naked)
void asan_check_{access_size}_byte_{access_mode_str}() {{
  __asm {{
    {AsanSaveEflags}
    {AsanFastPath}
    // Restore original EDX.
    mov edx, DWORD PTR[esp + 8]
    {AsanRestoreEflags}
    ret 4
  check_access_slow:
    {AsanSlowPath}
    // Restore original EDX.
    mov edx, DWORD PTR[esp + 8]
    {AsanRestoreEflags}
    ret 4
  report_failure:
    // Restore memory location in EDX.
    pop edx
    {AsanRestoreEflags}
    {AsanErrorPath}
  }}
}}
"""


# Generates a variant of the Asan check access functions that don't save
# the flags.
#
# The name of the generated method will be
# asan_check_(@p access_size)_byte_(@p access_mode_str)_no_flags().
#
# Args:
#     access_size: The size of the access (in byte).
#     access_mode_str: The string representing the access mode (read_access
#         or write_access).
#     access_mode_value: The internal value representing this kind of access.
# Note: Calling this function may alter the EFLAGS register only.
AsanCheckFunctionNoFlags = """\
// On entry, the address to check is in EDX and the previous contents of
// EDX are on stack. On exit the previous contents of EDX have been restored
// and popped off the stack. This function may modify EFLAGS, but preserves
// all other registers.
extern "C" __declspec(naked)
void asan_check_{access_size}_byte_{access_mode_str}_no_flags() {{
  __asm {{
    {AsanFastPath}
    // Restore original EDX.
    mov edx, DWORD PTR[esp + 4]
    ret 4
  check_access_slow:
    {AsanSlowPath}
    // Restore original EDX.
    mov edx, DWORD PTR[esp + 4]
    ret 4
  report_failure:
    // Restore memory location in EDX.
    pop edx
    {AsanErrorPath}
  }}
}}
"""


# Generates the Asan memory accessor redirector stubs.
#
# The name of the generated method will be
# asan_redirect_(@p access_size)_byte_(@p access_mode_str)(@p suffix)().
#
# Args:
#   access_size: The size of the access (in byte).
#   access_mode_str: The string representing the access mode (read_access
#       or write_access).
#   access_mode_value: The internal value representing this kind of
#       access.
#   suffix: The suffix - if any - for this function name
AsanRedirectFunction = """\
asan_redirect_{access_size}_byte_{access_mode_str}{suffix} LABEL PROC
  call asan_redirect_tail"""


# Declare the public label.
AsanRedirectFunctionDecl = """\
PUBLIC asan_redirect_{access_size}_byte_{access_mode_str}{suffix}"""


# Generates the Asan check access functions for a string instruction.
#
# The name of the generated method will be
# asan_check_(@p prefix)(@p access_size)_byte_(@p inst)_access().
#
# Args:
#     inst: The instruction mnemonic.
#     prefix: The prefix of the instruction (repz or nothing).
#     counter: The number of times the instruction must be executed (ECX).
#         It may be a register or a constant.
#     dst:_mode The memory access mode for destination (EDI).
#     src:_mode The memory access mode for destination (ESI).
#     access:_size The size of the access (in byte).
#     compare: A flag to enable shortcut execution by comparing memory
#         contents.
AsanCheckStrings = """\
extern "C" __declspec(naked)
void asan_check{prefix}{access_size}_byte_{func}_access() {{
  __asm {{
    // Prologue, save context.
    pushfd
    pushad
    // Fix the original value of ESP in the Asan registers context.
    // Removing 8 bytes (e.g.EFLAGS / EIP was on stack).
    add DWORD PTR[esp + 12], 8
    // Setup increment in EBX (depends on direction flag in EFLAGS).
    mov ebx, {access_size}
    pushfd
    pop eax
    test eax, 0x400
    jz skip_neg_direction
    neg ebx
  skip_neg_direction:
    // By standard calling convention, direction flag must be forward.
    cld
    // Push ARG(context), the Asan registers context.
    push esp
    // Push ARG(compare), shortcut when memory contents differ.
    push {compare}
    // Push ARG(increment), increment for EDI/EDI.
    push ebx
    // Push ARG(access_size), the access size.
    push {access_size}
    // Push ARG(length), the number of memory accesses.
    push {counter}
    // Push ARG(src_access_mode), source access type.
    push {src_mode}
    // Push ARG(src), the source pointer.
    push esi
    // Push ARG(dst_access_mode), destination access type.
    push {dst_mode}
    // Push ARG(dst), the destination pointer.
    push edi
    // Call the generic check strings function.
    call agent::asan::CheckStringsMemoryAccesses
    add esp, 36
    // Epilogue, restore context.
    popad
    popfd
    ret
  }}
}}
"""


# Generates the Asan string memory accessor redirector stubs.
#
# The name of the generated method will be
# asan_redirect_(@p prefix)(@p access_size)_byte_(@p inst)_access().
#
# Args:
#     inst: The instruction mnemonic.
#     prefix: The prefix of the instruction (repz or nothing).
#     counter: The number of times the instruction must be executed (ECX).
#         It may be a register or a constant.
#     dst:_mode The memory access mode for destination (EDI).
#     src:_mode The memory access mode for destination (ESI).
#     access:_size The size of the access (in byte).
#     compare: A flag to enable shortcut execution by comparing memory
#         contents.
AsanStringRedirectFunction = """\
asan_redirect{prefix}{access_size}_byte_{func}_access LABEL PROC
  call asan_redirect_tail"""

# Declare the public label.
AsanStringRedirectFunctionDecl = """\
PUBLIC asan_redirect{prefix}{access_size}_byte_{func}_access"""


class MacroAssembler(string.Formatter):
  """A formatter specialization to inject the AsanXXX macros and make
  them easier to use."""

  def parse(self, str):
    """Override to trim whitespace on empty trailing line."""
    for (lit, fld, fmt, conv) in super(MacroAssembler, self).parse(str):
      # Strip trailing whitespace from the previous literal to allow natural
      # use of AsanXXX macros.
      if lit[-5:] == '\n    ':
        lit = lit[:-4]
      yield((lit, fld, fmt, conv))

  def get_value(self, key, args, kwargs):
    """Override to inject macro definitions."""
    if key in _MACROS:
      return _MACROS[key].format(*args, **kwargs)
    return super(MacroAssembler, self).get_value(key, args, kwargs)


# Access sizes for the memory accessors generated.
_ACCESS_SIZES = (1, 2, 4, 8, 10, 16, 32)


# Access modes for the memory accessors generated.
_ACCESS_MODES = [
    ('read_access', 'AsanReadAccess'),
    ('write_access', 'AsanWriteAccess'),
]


# The string accessors generated.
_STRING_ACCESSORS = [
    ("cmps", "_repz_", "ecx", "AsanReadAccess", "AsanReadAccess", 4, 1),
    ("cmps", "_repz_", "ecx", "AsanReadAccess", "AsanReadAccess", 2, 1),
    ("cmps", "_repz_", "ecx", "AsanReadAccess", "AsanReadAccess", 1, 1),
    ("cmps", "_", 1, "AsanReadAccess", "AsanReadAccess", 4, 1),
    ("cmps", "_", 1, "AsanReadAccess", "AsanReadAccess", 2, 1),
    ("cmps", "_", 1, "AsanReadAccess", "AsanReadAccess", 1, 1),
    ("movs", "_repz_", "ecx", "AsanWriteAccess", "AsanReadAccess", 4, 0),
    ("movs", "_repz_", "ecx", "AsanWriteAccess", "AsanReadAccess", 2, 0),
    ("movs", "_repz_", "ecx", "AsanWriteAccess", "AsanReadAccess", 1, 0),
    ("movs", "_", 1, "AsanWriteAccess", "AsanReadAccess", 4, 0),
    ("movs", "_", 1, "AsanWriteAccess", "AsanReadAccess", 2, 0),
    ("movs", "_", 1, "AsanWriteAccess", "AsanReadAccess", 1, 0),
    ("stos", "_repz_", "ecx", "AsanWriteAccess", "AsanUnknownAccess", 4, 0),
    ("stos", "_repz_", "ecx", "AsanWriteAccess", "AsanUnknownAccess", 2, 0),
    ("stos", "_repz_", "ecx", "AsanWriteAccess", "AsanUnknownAccess", 1, 0),
    ("stos", "_", 1, "AsanWriteAccess", "AsanUnknownAccess", 4, 0),
    ("stos", "_", 1, "AsanWriteAccess", "AsanUnknownAccess", 2, 0),
    ("stos", "_", 1, "AsanWriteAccess", "AsanUnknownAccess", 1, 0),
]


def _GenerateCppFile():
  f = MacroAssembler()
  parts = [f.format(_HEADER, c='//')]

  # Generate the single-instance functions.
  parts.append(f.format(AsanGlobalFunctions))

  # TODO(siggi): Think about the best way to allow the stubs to communicate
  #     their own and their alternative identities to the bottleneck function.
  #     A particularly nice way is to generate an array of N-tuples that can
  #     be used when patching up IATs, where the redirector and the
  #     alternatives consume a row each. Passing in the array entry to the
  #     bottleneck is then the nicest, but the easiest is probably to pass in
  #     the redirector function itself...

  # Generate the memory accessor checkers.
  for access_size in _ACCESS_SIZES:
    for access, access_name in _ACCESS_MODES:
      parts.append(f.format(AsanCheckFunction,
                            access_size=access_size,
                            access_mode_str=access,
                            access_mode_value=access_name,
                            shadow="Shadow::shadow_"))

  # Generate the no flag saving memory accessor checkers.
  for access_size in _ACCESS_SIZES:
    for access, access_name in _ACCESS_MODES:
      parts.append(f.format(AsanCheckFunctionNoFlags,
                            access_size=access_size,
                            access_mode_str=access,
                            access_mode_value=access_name,
                            shadow="Shadow::shadow_"))

  # Generate string operation accessors.
  for (fn, p, c, dst_mode, src_mode, size, compare) in _STRING_ACCESSORS:
    parts.append(f.format(AsanCheckStrings,
                          func=fn,
                          prefix=p,
                          counter=c,
                          dst_mode=dst_mode,
                          src_mode=src_mode,
                          access_size=size,
                          compare=compare))
  return parts


def _GenerateAsmFile():
  f = MacroAssembler()
  parts = [f.format(_HEADER, c=';')]

  parts.append(f.format(_ASM_HEADER))

  # Declare the memory accessor redirectors.
  for suffix in ("", "_no_flags"):
    for access_size in _ACCESS_SIZES:
      for access, access_name in _ACCESS_MODES:
        parts.append(f.format(AsanRedirectFunctionDecl,
                              access_size=access_size,
                              access_mode_str=access,
                              access_mode_value=access_name,
                              suffix=suffix))

  # Declare string operation redirectors.
  for (fn, p, c, dst_mode, src_mode, size, compare) in _STRING_ACCESSORS:
    parts.append(f.format(AsanStringRedirectFunctionDecl,
                          func=fn,
                          prefix=p,
                          counter=c,
                          dst_mode=dst_mode,
                          src_mode=src_mode,
                          access_size=size,
                          compare=compare))

  parts.append(f.format(_PROC_HEADER))

  # Generate the memory accessor redirectors.
  for suffix in ("", "_no_flags"):
    for access_size in _ACCESS_SIZES:
      for access, access_name in _ACCESS_MODES:
        parts.append(f.format(AsanRedirectFunction,
                              access_size=access_size,
                              access_mode_str=access,
                              access_mode_value=access_name,
                              suffix=suffix))

  # Generate string operation redirectors.
  for (fn, p, c, dst_mode, src_mode, size, compare) in _STRING_ACCESSORS:
    parts.append(f.format(AsanStringRedirectFunction,
                          func=fn,
                          prefix=p,
                          counter=c,
                          dst_mode=dst_mode,
                          src_mode=src_mode,
                          access_size=size,
                          compare=compare))

  parts.append(f.format(_PROC_TRAILER))

  return parts


def _WriteFile(file_name, parts):
  contents = '\n'.join(parts)
  dir = os.path.dirname(__file__)
  with open(os.path.join(dir, file_name), "wb") as f:
    f.write(contents)


def main():
  cpp_file = _GenerateCppFile()
  asm_file = _GenerateAsmFile()

  _WriteFile('memory_interceptors_gen.cc', cpp_file)
  _WriteFile('memory_interceptors.asm', asm_file)


if __name__ == '__main__':
  main()
