// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_IR_FORMAT_STRINGS_H_
#define XLS_IR_FORMAT_STRINGS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/ir/format_preference.h"

namespace xls {

// IR-only control steps conditionally include a balanced sequence of formatting
// steps. A begin step consumes one bits[1] data operand; an end step consumes
// none. DSLX source format strings never accept these internal directives.
enum class FormatControl {
  kBeginConditional,
  kEndConditional,
};

// Formatting prints literal fragments and operands, optionally guarded by
// balanced IR-only conditional sections.
using FormatStep = std::variant<std::string, FormatPreference, FormatControl>;

// Parse a format string into the steps required to build output using it.
// Example: "x is {} in the default format." would parse into the steps
// {"x is ", FormatPreference::kDefault, " in the default format."}
absl::StatusOr<std::vector<FormatStep>> ParseFormatString(
    std::string_view format_string);

// Parses persisted IR format strings, additionally recognizing `{?}` and
// `{/}` as balanced conditional-begin and conditional-end directives.
absl::StatusOr<std::vector<FormatStep>> ParseIrFormatString(
    std::string_view format_string);

// Rejects unmatched conditional directives in a programmatically built format.
absl::Status ValidateFormatSteps(absl::Span<const FormatStep> format);

// Count the number of data operands expected by parsed format.
// Example: As above, "x is {} in the default format." parses into
// {"x is ", FormatPreference::kDefault, " in the default format."}
// This expects one data operand.
int64_t OperandsExpectedByFormat(absl::Span<const FormatStep> format);

// Extracts all the format preferences from the given format steps (i.e. drops
// all the string literals) and returns them.
std::vector<FormatPreference> OperandPreferencesFromFormat(
    absl::Span<const FormatStep> format);

// Converts format steps into their printed XLS IR representation. Ordinary
// steps can also be parsed as DSLX source; conditional directives are IR-only
// and require ParseIrFormatString instead of ParseFormatString.
std::string StepsToXlsFormatString(absl::Span<const FormatStep> format);

// Renders one IR literal fragment, replacing doubled braces exactly once.
// FormatStep literals retain escapes so their persisted IR can be reparsed.
std::string UnescapeFormatStringLiteral(std::string_view literal);

// Convert a sequence of format steps into a format string that can be used
// in generated Verilog. IR-only conditional control steps must have already
// been removed or split into separate format strings.
std::string StepsToVerilogFormatString(absl::Span<const FormatStep> format);

}  // namespace xls

#endif  // XLS_IR_FORMAT_STRINGS_H_
