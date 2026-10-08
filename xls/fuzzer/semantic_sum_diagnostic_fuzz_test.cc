// Copyright 2026 The XLS Authors
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

// Invalid pattern mutations stay out of the valid-source differential runner.
// Each mutation has a typechecked control with the same declaration and types.

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "fuzztest/fuzztest.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls {
namespace {

enum class InvalidPattern {
  kArmAfterWildcard,
  kArmAfterInvalid,
  kExtraPayloadPattern,
  kBindingInAlternative,
};

absl::Status Typecheck(std::string_view source) {
  dslx::ImportData imports = dslx::CreateImportDataForTest();
  return dslx::ParseAndTypecheck(source, "diagnostic.x", "diagnostic", &imports)
      .status();
}

void RejectsInvalidPatternMutation(int payload_width, int tag_width,
                                   bool signed_tags, InvalidPattern mutation) {
  const std::string payload_type = absl::StrCat("uN[", payload_width, "]");
  const std::string header = absl::StrCat(
      "enum S: ", signed_tags ? "sN[" : "uN[", tag_width, "] { None = 0, Some(",
      payload_type, ") = ", signed_tags ? -1 : 3, " }\nfn main(x: S) -> ",
      payload_type, " { match x {\n");
  const std::string zero = absl::StrCat(payload_type, ":0");
  const std::string control =
      absl::StrCat(header, "S::Some(value) => value, _ => ", zero,
                   ", invalid!(raw) => raw as ", payload_type, ", } }\n");
  XLS_ASSERT_OK(Typecheck(control)) << control;

  std::string arms;
  std::string_view diagnostic;
  if (mutation == InvalidPattern::kArmAfterWildcard) {
    arms = absl::StrCat("_ => ", zero, ", S::Some(value) => value,");
    diagnostic =
        "A wildcard arm may only be followed by a final `invalid!` arm.";
  } else if (mutation == InvalidPattern::kArmAfterInvalid) {
    arms = absl::StrCat("invalid! => ", zero, ", _ => ", zero, ",");
    diagnostic = "`invalid!` must be the final arm in a match.";
  } else if (mutation == InvalidPattern::kExtraPayloadPattern) {
    arms = absl::StrCat("S::Some(_, _) => ", zero, ", _ => ", zero, ",");
    diagnostic = "expects 1 payload pattern(s), got 2";
  } else {
    arms = absl::StrCat("S::Some(value) | S::None => value, _ => ", zero, ",");
    diagnostic = "Cannot bind names in a match arm with multiple patterns";
  }
  const std::string invalid = absl::StrCat(header, arms, " } }\n");
  SCOPED_TRACE(invalid);
  EXPECT_THAT(Typecheck(invalid),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                                     testing::HasSubstr(diagnostic)));
}

TEST(SemanticSumDiagnosticFuzzTest, WitnessesEachMutation) {
  for (InvalidPattern mutation :
       {InvalidPattern::kArmAfterWildcard, InvalidPattern::kArmAfterInvalid,
        InvalidPattern::kExtraPayloadPattern,
        InvalidPattern::kBindingInAlternative}) {
    RejectsInvalidPatternMutation(1, 2, false, mutation);
    RejectsInvalidPatternMutation(8, 5, true, mutation);
  }
}

FUZZ_TEST(SemanticSumDiagnosticFuzzTest, RejectsInvalidPatternMutation)
    .WithDomains(fuzztest::InRange(1, 8), fuzztest::InRange(2, 5),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::ElementOf<InvalidPattern>(
                     {InvalidPattern::kArmAfterWildcard,
                      InvalidPattern::kArmAfterInvalid,
                      InvalidPattern::kExtraPayloadPattern,
                      InvalidPattern::kBindingInAlternative}));

}  // namespace
}  // namespace xls
