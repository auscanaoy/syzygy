// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/common/asan_parameters.h"

#include <windows.h>

#include <algorithm>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"

namespace common {

namespace {

using base::CommandLine;

// Trinary return values used to indicate if a flag was updated or not.
enum FlagResult {
  kFlagNotPresent,
  kFlagSet,
  kFlagError
};

// Templated utility class for parsing parameter values from a command-line.
// @tparam Converter Defines the types and parsing.
template<typename Parser>
struct UpdateValueFromCommandLine {
  // Try to update the value of a variable from a command-line.
  // @param cmd_line The command line who might contain a given parameter.
  // @param param_name The parameter that we want to read.
  // @param value Will receive the value of the parameter if it's present.
  // @returns kFlagNotPresent if the flag was not present and left at its
  //     default; kFlagSet if the flag was present, valid and modified; or
  //     kFlagError if the flag was present but invalid. Logs an error on
  //     failure, and an info message on successful parsing.
  static FlagResult Do(const base::CommandLine& cmd_line,
                       const std::string& param_name,
                       typename Parser::ValueType* value) {
    DCHECK_NE(reinterpret_cast<Parser::ValueType*>(NULL), value);

    if (!cmd_line.HasSwitch(param_name))
      return kFlagNotPresent;

    std::string value_str = cmd_line.GetSwitchValueASCII(param_name);
    Parser::IntermediateType new_value = 0;
    if (!Parser::Convert(value_str, &new_value)) {
      LOG(ERROR) << "Failed to parse \"" << param_name << "\" value of \""
                 << value_str << "\".";
      return kFlagError;
    }

    *value = static_cast<Parser::ValueType>(new_value);
    VLOG(1) << "Set \"" << param_name << "\" to " << *value << ".";
    return kFlagSet;
  }
};

// Parses a uint32_t value from a string provided on the command-line.
struct Uint32Parser {
  typedef size_t IntermediateType;
  typedef uint32_t ValueType;
  static bool Convert(const std::string& value_str, IntermediateType* value) {
    return base::StringToSizeT(value_str, value);
  }
};
typedef UpdateValueFromCommandLine<Uint32Parser> UpdateUint32FromCommandLine;

// Parses a float value from a string provided on the command-line.
struct FloatParser {
  typedef double IntermediateType;
  typedef float ValueType;
  static bool Convert(const std::string& value_str, IntermediateType* value) {
    return base::StringToDouble(value_str, value);
  }
};
typedef UpdateValueFromCommandLine<FloatParser> UpdateFloatFromCommandLine;

// Try to update the value of an array of ignored stack ids from a command-line.
// We expect the values to be in hexadecimal format and separated by a
// semi-colon.
// @param cmd_line The command line to parse.
// @param param_name The parameter that we want to read.
// @param values Will receive the set of parsed values.
// @returns true on success, false otherwise.
bool ReadIgnoredStackIdsFromCommandLine(const base::CommandLine& cmd_line,
                                        const std::string& param_name,
                                        std::set<AsanStackId>* values) {
  DCHECK(values != NULL);
  if (!cmd_line.HasSwitch(param_name))
    return true;

  std::string value_str = cmd_line.GetSwitchValueASCII(param_name);
  base::StringTokenizer string_tokenizer(value_str, ";");
  while (string_tokenizer.GetNext()) {
    int64_t new_value = 0;
    if (!base::HexStringToInt64(string_tokenizer.token(), &new_value)) {
      LOG(ERROR) << "Failed to parse \"" << param_name << "\" value of \""
                 << string_tokenizer.token() << "\".";
      return false;
    }

    VLOG(1) << "Parsed \"" << param_name << "\" value of "
            << base::StringPrintf("0x%016llX", new_value) << ".";
    values->insert(static_cast<AsanStackId>(new_value));
  }

  return true;
}

}  // namespace

// SYZYgy Asan Runtime Options.
const char kAsanParametersSectionName[] = ".syzyaro";
const uint32_t kAsanParametersSectionCharacteristics =
    IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

// Default values of HeapProxy parameters
const uint32_t kDefaultQuarantineBlockSize = 4 * 1024 * 1024;
const uint32_t kDefaultTrailerPaddingSize = 0;
const float kDefaultAllocationGuardRate = 1.0;

// Default values of StackCaptureCache parameters.
const uint32_t kDefaultReportingPeriod = 0;
const uint32_t kDefaultBottomFramesToSkip = 0;

// Default values of StackCapture parameters.
// From http://msdn.microsoft.com/en-us/library/bb204633.aspx,
// The maximum number of frames which CaptureStackBackTrace can be asked
// to traverse must be less than 63, so this can't be any larger than 62.
const uint32_t kDefaultMaxNumFrames = 62;

// Default values of AsanRuntime parameters.
const bool kDefaultExitOnFailure = false;
const bool kDefaultCheckHeapOnFailure = true;
const bool kDefaultDisableBreakpadReporting = false;
const bool kDefaultFeatureRandomization = false;
const bool kDefaultReportInvalidAccesses = false;
const bool kDefaultDeferCrashReporterInitialization = false;

// Default values of AsanLogger parameters.
const bool kDefaultMiniDumpOnFailure = false;
const bool kDefaultLogAsText = true;

// Default values of ZebraBlockHeap parameters.
const uint32_t kDefaultZebraBlockHeapSize = 16 * 1024 * 1024;
const float kDefaultZebraBlockHeapQuarantineRatio = 0.25f;

// Default values of the BlockHeapManager parameters.
const bool kDefaultEnableRateTargetedHeaps = true;
const bool kDefaultEnableZebraBlockHeap = false;
const bool kDefaultEnableAllocationFilter = false;
const float kDefaultQuarantineFloodFillRate = 0.5f;
const bool kDefaultPreventDuplicateCorruptionCrashes = false;

// Default values of LargeBlockHeap parameters.
extern const bool kDefaultEnableLargeBlockHeap = true;
// We want to maintain an allocation overhead of around 45%, which has been
// our historic average for Chrome. Overhead in this heap is 2 pages, so want
// 2 / 0.45 = 4.44 < 5 page minimum.
extern const size_t kDefaultLargeAllocationThreshold = 5 * 4096;

const char kSyzyAsanOptionsEnvVar[] = "SYZYGY_ASAN_OPTIONS";
const char kAsanRtlOptions[] = "asan-rtl-options";

// String names of HeapProxy parameters.
const char kParamQuarantineSize[] = "quarantine_size";
const char kParamQuarantineBlockSize[] = "quarantine_block_size";
const char kParamTrailerPaddingSize[] = "trailer_padding_size";
extern const char kParamAllocationGuardRate[] = "allocation_guard_rate";

// String names of StackCaptureCache parameters.
const char kParamReportingPeriod[] = "compression_reporting_period";
const char kParamBottomFramesToSkip[] = "bottom_frames_to_skip";

// String names of StackCapture parameters.
const char kParamMaxNumFrames[] = "max_num_frames";

// String names of AsanRuntime parameters.
const char kParamIgnoredStackIds[] = "ignored_stack_ids";
const char kParamExitOnFailure[] = "exit_on_failure";
const char kParamNoCheckHeapOnFailure[] = "no_check_heap_on_failure";
const char kParamDisableBreakpadReporting[]  = "disable_breakpad";
const char kParamFeatureRandomization[] = "feature_randomization";
const char kParamReportInvalidAccesses[] = "report_invalid_accesses";
const char kParamDeferCrashReporterInitialization[] =
    "defer_crash_reporter_initialization";

// String names of AsanLogger parameters.
const char kParamMiniDumpOnFailure[] = "minidump_on_failure";
const char kParamNoLogAsText[] = "no_log_as_text";

// String names of ZebraBlockHeap parameters.
const char kParamZebraBlockHeapSize[] = "zebra_block_heap_size";
const char kParamZebraBlockHeapQuarantineRatio[] =
    "zebra_block_heap_quarantine_ratio";

// String names of BlockHeapManager parameters.
const char kParamDisableCtMalloc[] = "disable_ctmalloc";
const char kParamEnableZebraBlockHeap[] = "enable_zebra_block_heap";
const char kParamEnableAllocationFilter[] = "enable_allocation_filter";
const char kParamQuarantineFloodFillRate[] = "quarantine_flood_fill_rate";
const char kParamPreventDuplicateCorruptionCrashes[] =
    "prevent_duplicate_corruption_crashes";

// String names of LargeBlockHeap parameters.
const char kParamDisableLargeBlockHeap[] = "disable_large_block_heap";
const char kParamLargeAllocationThreshold[] = "large_allocation_threshold";

InflatedAsanParameters::InflatedAsanParameters() {
  // Clear the AsanParameters portion of ourselves.
  ::memset(this, 0, sizeof(AsanParameters));
}

FlatAsanParameters::FlatAsanParameters(
    const InflatedAsanParameters& asan_parameters) {
  bool have_ignored_stack_ids = !asan_parameters.ignored_stack_ids_set.empty();

  size_t struct_size = sizeof(AsanParameters);
  size_t ignored_stack_ids_size = 0;
  if (have_ignored_stack_ids) {
    ignored_stack_ids_size = sizeof(AsanStackId) *
        (asan_parameters.ignored_stack_ids_set.size() + 1);
  }
  size_t data_size = struct_size + ignored_stack_ids_size;

  data_.resize(data_size, 0);
  ::memcpy(data_.data(), &asan_parameters, struct_size);

  // Get typed pointers to the underlying data.
  AsanParameters* params = reinterpret_cast<AsanParameters*>(data_.data());
  AsanStackId* ignored_stack_ids = NULL;
  if (have_ignored_stack_ids) {
    ignored_stack_ids = reinterpret_cast<AsanStackId*>(
        data_.data() + struct_size);
  }

  // Patch things up.
  // The structure size is not supposed to be too large to fit into uint32_t.
  params->size = static_cast<uint32_t>(data_size);
  params->ignored_stack_ids = ignored_stack_ids;

  if (have_ignored_stack_ids) {
    // Fill in the ignored stack IDs.
    std::set<AsanStackId>::const_iterator id_it =
        asan_parameters.ignored_stack_ids_set.begin();
    for (; id_it != asan_parameters.ignored_stack_ids_set.end(); ++id_it)
      *(ignored_stack_ids++) = *id_it;
    *(ignored_stack_ids++) = 0;  // Terminating NULL.
    DCHECK_EQ(data_.data() + data_size,
              reinterpret_cast<uint8_t*>(ignored_stack_ids));
  }
}

void SetDefaultAsanParameters(AsanParameters* asan_parameters) {
  DCHECK_NE(reinterpret_cast<AsanParameters*>(NULL), asan_parameters);

  ::memset(asan_parameters, 0, sizeof(AsanParameters));

  asan_parameters->size = sizeof(AsanParameters);
  asan_parameters->version = kAsanParametersVersion;
  asan_parameters->quarantine_size = kDefaultQuarantineSize;
  asan_parameters->reporting_period = kDefaultReportingPeriod;
  asan_parameters->bottom_frames_to_skip = kDefaultBottomFramesToSkip;
  asan_parameters->max_num_frames = kDefaultMaxNumFrames;
  asan_parameters->trailer_padding_size = kDefaultTrailerPaddingSize;
  asan_parameters->ignored_stack_ids = NULL;
  asan_parameters->quarantine_block_size = kDefaultQuarantineBlockSize;
  asan_parameters->minidump_on_failure = kDefaultMiniDumpOnFailure;
  asan_parameters->exit_on_failure = kDefaultExitOnFailure;
  asan_parameters->check_heap_on_failure = kDefaultCheckHeapOnFailure;
  asan_parameters->log_as_text = kDefaultLogAsText;
  asan_parameters->disable_breakpad_reporting =
      kDefaultDisableBreakpadReporting;
  asan_parameters->allocation_guard_rate = kDefaultAllocationGuardRate;
  asan_parameters->zebra_block_heap_size = kDefaultZebraBlockHeapSize;
  asan_parameters->zebra_block_heap_quarantine_ratio =
      kDefaultZebraBlockHeapQuarantineRatio;
  // CtMalloc has been deprecated.
  asan_parameters->deprecated_enable_ctmalloc = false;
  asan_parameters->enable_zebra_block_heap = kDefaultEnableZebraBlockHeap;
  asan_parameters->enable_large_block_heap = kDefaultEnableLargeBlockHeap;
  asan_parameters->enable_allocation_filter = kDefaultEnableAllocationFilter;
  asan_parameters->large_allocation_threshold =
      kDefaultLargeAllocationThreshold;
  asan_parameters->feature_randomization = kDefaultFeatureRandomization;
  asan_parameters->quarantine_flood_fill_rate =
      kDefaultQuarantineFloodFillRate;
  asan_parameters->prevent_duplicate_corruption_crashes =
      kDefaultPreventDuplicateCorruptionCrashes;
  asan_parameters->report_invalid_accesses = kDefaultReportInvalidAccesses;
  asan_parameters->defer_crash_reporter_initialization =
      kDefaultDeferCrashReporterInitialization;
}

bool InflateAsanParameters(const AsanParameters* pod_params,
                           InflatedAsanParameters* inflated_params) {
  // This must be kept up to date with AsanParameters as it evolves.
  static const size_t kSizeOfAsanParametersByVersion[] = {
      40, 44, 48, 52, 52, 52, 56, 56, 56, 56, 60, 60, 60, 60, 60, 60};
  static_assert(
      arraysize(kSizeOfAsanParametersByVersion) == kAsanParametersVersion + 1,
      "Size of parameters version out of date.");

  SetDefaultAsanParameters(inflated_params);

  const uint8_t* data = reinterpret_cast<const uint8_t*>(pod_params);
  const uint8_t* data_end = data + pod_params->size;

  // This is the size of known POD data in the version of the structure
  // being inflated.
  size_t min_pod_size = kSizeOfAsanParametersByVersion[
      std::min(kAsanParametersVersion, pod_params->version)];
  const uint8_t* min_pod_end = data + min_pod_size;

  // If we have stack IDs, ensure the pointer is to a valid location.
  if (pod_params->ignored_stack_ids != NULL) {
    const uint8_t* ignored_stack_ids =
        reinterpret_cast<const uint8_t*>(pod_params->ignored_stack_ids);
    if (ignored_stack_ids < min_pod_end || ignored_stack_ids >= data_end) {
      LOG(ERROR) << "Invalid ignored_stack_ids pointer.";
      return false;
    }
  }

  // Only copy as many parameters as the structure contains, or that our version
  // of the runtime understands.
  DCHECK_LE(min_pod_size, sizeof(AsanParameters));
  ::memcpy(inflated_params, pod_params, min_pod_size);

  // Patch up the params to reflect our runtime version.
  inflated_params->size = sizeof(AsanParameters);
  inflated_params->version = kAsanParametersVersion;
  (static_cast<AsanParameters*>(inflated_params))->ignored_stack_ids = NULL;

  // Populate the ignored stack ids.
  const AsanStackId* stack_id = pod_params->ignored_stack_ids;
  if (stack_id == NULL)
    return true;
  while (*stack_id != NULL) {
    if (reinterpret_cast<const uint8_t*>(stack_id) > data_end) {
      LOG(ERROR) << "AsanParameters::ignored_stack_ids list is not NULL "
                 << "terminated.";
      return false;
    }
    inflated_params->ignored_stack_ids_set.insert(*stack_id);
    ++stack_id;
  }

  return true;
}

namespace {

// Parses a boolean flag. Returns true if the flag was present in the given
// command line, and sets the parsed value in |value|. If |name| is "foo" then
// the switches "--foo" and "--enable_foo" will result in |value| being set to
// true. A switch of "--disable_foo" will result in |value| being set to false.
// The presence of none of these switches will result in the function returning
// false, and |value| being left as is.
bool ParseBooleanFlag(const char* name, const base::CommandLine& cmd_line,
                      bool* value) {
  DCHECK_NE(static_cast<const char*>(nullptr), name);
  DCHECK_NE(static_cast<bool*>(nullptr), value);

  std::string raw = std::string("--") + std::string(name);
  std::string enable = std::string("--enable_") + std::string(name);
  std::string disable = std::string("--enable_") + std::string(name);

  // Parse the command-line from left to right so that position matters. This
  // allows the same flag to be toggled back and forth on a single command
  // line.
  bool ret = false;
  for (const auto& s16 : cmd_line.argv()) {
    std::string s = base::WideToUTF8(s16);
    if (s == raw || s == enable) {
      *value = true;
      ret = true;
      continue;
    }

    if (s == disable) {
      *value = false;
      ret = true;
      continue;
    }
  }

  return ret;
}

}  // namespace

bool ParseAsanParameters(const base::StringPiece16& param_string,
                         InflatedAsanParameters* asan_parameters) {
  DCHECK_NE(reinterpret_cast<InflatedAsanParameters*>(NULL), asan_parameters);

  // Prepends the flags with a dummy executable name to keep the
  // base::CommandLine parser happy.
  std::wstring str(param_string.as_string());
  str.insert(0, L" ");
  str.insert(0, L"dummy.exe");
  base::CommandLine cmd_line = base::CommandLine::FromString(str);

  // Parse the quarantine size flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamQuarantineSize,
          &asan_parameters->quarantine_size) == kFlagError) {
    return false;
  }

  // Parse the quarantine block size.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamQuarantineBlockSize,
          &asan_parameters->quarantine_block_size) == kFlagError) {
    return false;
  }

  // Parse the trailer padding size flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamTrailerPaddingSize,
          &asan_parameters->trailer_padding_size) == kFlagError) {
    return false;
  }

  // Parse the allocation guard rate.
  if (UpdateFloatFromCommandLine::Do(cmd_line, kParamAllocationGuardRate,
          &asan_parameters->allocation_guard_rate) == kFlagError) {
    return false;
  }

  // Parse the reporting period flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamReportingPeriod,
          &asan_parameters->reporting_period) == kFlagError) {
    return false;
  }

  // Parse the bottom frames to skip flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamBottomFramesToSkip,
          &asan_parameters->bottom_frames_to_skip) == kFlagError) {
    return false;
  }

  // Parse the max number of frames flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamMaxNumFrames,
          &asan_parameters->max_num_frames) == kFlagError) {
    return false;
  }

  // Parse the ignored stack ids.
  if (!ReadIgnoredStackIdsFromCommandLine(cmd_line, kParamIgnoredStackIds,
           &asan_parameters->ignored_stack_ids_set)) {
    return false;
  }

  // Parse the zebra block heap size flag.
  if (UpdateUint32FromCommandLine::Do(cmd_line, kParamZebraBlockHeapSize,
          &asan_parameters->zebra_block_heap_size) == kFlagError) {
    return false;
  }

  // Parse the zebra block heap quarantine ratio flag.
  if (UpdateFloatFromCommandLine::Do(cmd_line,
          kParamZebraBlockHeapQuarantineRatio,
          &asan_parameters->zebra_block_heap_quarantine_ratio) == kFlagError) {
    return false;
  }

  // Parse the large block heap allocation threshold.
  if (UpdateUint32FromCommandLine::Do(cmd_line,
          kParamLargeAllocationThreshold,
          &asan_parameters->large_allocation_threshold) == kFlagError) {
    return false;
  }

  // Parse the quarantine flood-fill rate.
  if (UpdateFloatFromCommandLine::Do(cmd_line,
          kParamQuarantineFloodFillRate,
          &asan_parameters->quarantine_flood_fill_rate) == kFlagError) {
    return false;
  }

  // Parse the other (boolean) flags.
  // TODO(chrisha): Transition these all to new style flags.
  if (cmd_line.HasSwitch(kParamMiniDumpOnFailure))
    asan_parameters->minidump_on_failure = true;
  if (cmd_line.HasSwitch(kParamExitOnFailure))
    asan_parameters->exit_on_failure = true;
  if (cmd_line.HasSwitch(kParamNoLogAsText))
    asan_parameters->log_as_text = false;
  if (cmd_line.HasSwitch(kParamNoCheckHeapOnFailure))
    asan_parameters->check_heap_on_failure = false;
  if (cmd_line.HasSwitch(kParamDisableBreakpadReporting))
    asan_parameters->disable_breakpad_reporting = true;
  if (cmd_line.HasSwitch(kParamDisableCtMalloc))
    LOG(WARNING) << "Ignoring deprecated " << kParamDisableCtMalloc << " flag.";
  if (cmd_line.HasSwitch(kParamEnableZebraBlockHeap))
    asan_parameters->enable_zebra_block_heap = true;
  if (cmd_line.HasSwitch(kParamDisableLargeBlockHeap))
    asan_parameters->enable_large_block_heap = false;
  if (cmd_line.HasSwitch(kParamEnableAllocationFilter))
    asan_parameters->enable_allocation_filter = true;
  if (cmd_line.HasSwitch(kParamPreventDuplicateCorruptionCrashes))
    asan_parameters->prevent_duplicate_corruption_crashes = true;
  if (cmd_line.HasSwitch(kParamReportInvalidAccesses))
    asan_parameters->report_invalid_accesses = true;
  if (cmd_line.HasSwitch(kParamDeferCrashReporterInitialization))
    asan_parameters->defer_crash_reporter_initialization = true;

  // New style boolean flags with both positive and negative setters. This
  // allows them to be set one way in the baked in configuration, and set
  // another way in the environment specified runtime configuration.
  bool value = false;
  if (ParseBooleanFlag(kParamFeatureRandomization, cmd_line, &value))
    asan_parameters->feature_randomization = value;

  return true;
}

}  // namespace common
