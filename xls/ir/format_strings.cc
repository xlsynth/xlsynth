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

#include "xls/ir/format_strings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "xls/ir/format_preference.h"

namespace xls {

namespace {

absl::StatusOr<std::vector<FormatStep>> ParseFormatStringImpl(
    std::string_view format_string, bool allow_control_steps) {
  std::vector<FormatStep> steps;
  int64_t conditional_depth = 0;

  int64_t i = 0;
  auto consume_substr = [&i, format_string](std::string_view m) -> bool {
    if (format_string.substr(i, m.length()) == m) {
      i = i + m.length();
      return true;
    }
    return false;
  };

  std::string fragment;
  fragment.reserve(format_string.length());

  auto push_fragment = [&fragment, &steps]() {
    if (!fragment.empty()) {
      steps.push_back(fragment);
      fragment.clear();
    }
  };

  while (i < format_string.length()) {
    if (consume_substr("{{")) {
      absl::StrAppend(&fragment, "{{");
      continue;
    }
    if (consume_substr("}}")) {
      absl::StrAppend(&fragment, "}}");
      continue;
    }
    if (allow_control_steps && consume_substr("{?}")) {
      push_fragment();
      steps.push_back(FormatControl::kBeginConditional);
      ++conditional_depth;
      continue;
    }
    if (allow_control_steps && consume_substr("{/}")) {
      if (conditional_depth == 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Conditional format end without matching begin in format string "
            "\"%s\"",
            format_string));
      }
      push_fragment();
      steps.push_back(FormatControl::kEndConditional);
      --conditional_depth;
      continue;
    }
    if (consume_substr("{}")) {
      push_fragment();
      steps.push_back(FormatPreference::kDefault);
      continue;
    }
    if (consume_substr("{:u}")) {
      push_fragment();
      steps.push_back(FormatPreference::kUnsignedDecimal);
      continue;
    }
    if (consume_substr("{:d}")) {
      push_fragment();
      steps.push_back(FormatPreference::kSignedDecimal);
      continue;
    }
    if (consume_substr("{:x}")) {
      push_fragment();
      steps.push_back(FormatPreference::kPlainHex);
      continue;
    }
    if (consume_substr("{:0x}")) {
      push_fragment();
      steps.push_back(FormatPreference::kZeroPaddedHex);
      continue;
    }
    if (consume_substr("{:#x}")) {
      push_fragment();
      steps.push_back(FormatPreference::kHex);
      continue;
    }
    if (consume_substr("{:b}")) {
      push_fragment();
      steps.push_back(FormatPreference::kPlainBinary);
      continue;
    }
    if (consume_substr("{:0b}")) {
      push_fragment();
      steps.push_back(FormatPreference::kZeroPaddedBinary);
      continue;
    }
    if (consume_substr("{:#b}")) {
      push_fragment();
      steps.push_back(FormatPreference::kBinary);
      continue;
    }
    if (format_string[i] == '{') {
      size_t close_pos = format_string.find('}', i);
      if (close_pos != std::string_view::npos) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Invalid or unsupported format specifier \"%s\" in format string "
            "\"%s\"",
            format_string.substr(i, close_pos - i + 1), format_string));
      }
      return absl::InvalidArgumentError(absl::StrFormat(
          "{ without matching } at position %d in format string \"%s\"", i,
          format_string));
    }
    if (format_string[i] == '}') {
      return absl::InvalidArgumentError(absl::StrFormat(
          "} with no preceding { at position %d in format string \"%s\"", i,
          format_string));
    }

    fragment += format_string[i];
    i = i + 1;
  }

  push_fragment();
  if (conditional_depth != 0) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Conditional format begin without matching end in format string "
        "\"%s\"",
        format_string));
  }
  return steps;
}

}  // namespace

absl::StatusOr<std::vector<FormatStep>> ParseFormatString(
    std::string_view format_string) {
  return ParseFormatStringImpl(format_string, /*allow_control_steps=*/false);
}

absl::StatusOr<std::vector<FormatStep>> ParseIrFormatString(
    std::string_view format_string) {
  return ParseFormatStringImpl(format_string, /*allow_control_steps=*/true);
}

absl::Status ValidateFormatSteps(absl::Span<const FormatStep> format) {
  int64_t conditional_depth = 0;
  for (const FormatStep& step : format) {
    if (auto* control = std::get_if<FormatControl>(&step)) {
      if (*control == FormatControl::kBeginConditional) {
        ++conditional_depth;
      } else if (conditional_depth == 0) {
        return absl::InvalidArgumentError(
            "Conditional format end without matching begin.");
      } else {
        --conditional_depth;
      }
    }
  }
  if (conditional_depth != 0) {
    return absl::InvalidArgumentError(
        "Conditional format begin without matching end.");
  } else {
    return absl::OkStatus();
  }
}

std::vector<FormatPreference> OperandPreferencesFromFormat(
    absl::Span<const FormatStep> format) {
  std::vector<FormatPreference> preferences;
  for (const FormatStep& step : format) {
    if (std::holds_alternative<FormatPreference>(step)) {
      preferences.push_back(std::get<FormatPreference>(step));
    }
  }
  return preferences;
}

int64_t OperandsExpectedByFormat(absl::Span<const FormatStep> format) {
  return std::count_if(
      format.begin(), format.end(), [](const FormatStep& step) {
        const auto* control = std::get_if<FormatControl>(&step);
        return std::holds_alternative<FormatPreference>(step) ||
               (control != nullptr &&
                *control == FormatControl::kBeginConditional);
      });
}

std::string StepsToXlsFormatString(absl::Span<const FormatStep> format) {
  return absl::StrJoin(
      format, "", [](std::string* out, const FormatStep& step) {
        if (std::holds_alternative<FormatPreference>(step)) {
          absl::StrAppend(out, FormatPreferenceToXlsSpecifier(
                                   std::get<FormatPreference>(step)));
        } else if (std::holds_alternative<std::string>(step)) {
          absl::StrAppend(out, std::get<std::string>(step));
        } else {
          absl::StrAppend(out, std::get<FormatControl>(step) ==
                                       FormatControl::kBeginConditional
                                   ? "{?}"
                                   : "{/}");
        }
      });
}

std::string UnescapeFormatStringLiteral(std::string_view literal) {
  return absl::StrReplaceAll(literal, {{"{{", "{"}, {"}}", "}"}});
}

std::string StepsToVerilogFormatString(absl::Span<const FormatStep> format) {
  return absl::StrJoin(
      format, "", [](std::string* out, const FormatStep& step) {
        if (std::holds_alternative<FormatPreference>(step)) {
          absl::StrAppend(out, FormatPreferenceToVerilogSpecifier(
                                   std::get<FormatPreference>(step)));
        } else if (std::holds_alternative<std::string>(step)) {
          absl::StrAppend(
              out, UnescapeFormatStringLiteral(std::get<std::string>(step)));
        } else {
          absl::StrAppend(out, std::get<FormatControl>(step) ==
                                       FormatControl::kBeginConditional
                                   ? "{?}"
                                   : "{/}");
        }
      });
}

}  // namespace xls
