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

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/fuzzing/fuzztest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"

namespace xls {
namespace {

enum class SemanticSumNegativeMutationKind {
  kAsymmetricOrPattern,
  kDuplicatePayloadField,
  kWildcardConstructorMix,
  kTooManyPayloadPatterns,
  kMissingPayloadPattern,
};

struct MutationRecipe {
  SemanticSumNegativeMutationKind kind;
  std::string_view mutation_name;
  std::string_view base_seed_id;
  std::string_view needle;
  std::string_view replacement;
  std::string_view expected_diagnostic_substr;
};

struct SemanticSumNegativeMutation {
  std::string mutation_name;
  std::string mutated_text;
  std::string expected_diagnostic_substr;
};

constexpr std::array<MutationRecipe, 5> kMutationRecipes = {{
    {
        .kind = SemanticSumNegativeMutationKind::kAsymmetricOrPattern,
        .mutation_name = "asymmetric_or_pattern",
        .base_seed_id = "source_exhaustive_constructor_match_without_wildcard",
        .needle = "    Option::Some(v) => v,\n"
                  "    Option::None => u32:0,\n",
        .replacement = "    Option::None | Option::Some(v) => v,\n",
        .expected_diagnostic_substr =
            "Cannot bind names in a match arm with multiple patterns",
    },
    {
        .kind = SemanticSumNegativeMutationKind::kDuplicatePayloadField,
        .mutation_name = "duplicate_payload_field",
        .base_seed_id = "typeinfo_layout_empty_payload_kinds",
        .needle = "  Point { x: u32 },\n",
        .replacement = "  Point { x: u32, x: u32 },\n",
        .expected_diagnostic_substr = "defined more than once",
    },
    {
        .kind = SemanticSumNegativeMutationKind::kWildcardConstructorMix,
        .mutation_name = "wildcard_constructor_mix",
        .base_seed_id = "source_exhaustive_constructor_match_without_wildcard",
        .needle = "    Option::Some(v) => v,\n"
                  "    Option::None => u32:0,\n",
        .replacement = "    Option::Some(v) | _ => v,\n",
        .expected_diagnostic_substr =
            "Cannot bind names in a match arm with multiple patterns",
    },
    {
        .kind = SemanticSumNegativeMutationKind::kTooManyPayloadPatterns,
        .mutation_name = "too_many_payload_patterns",
        .base_seed_id = "source_sample_runner_semantic_sum_argument",
        .needle = "    Choice::Byte(value) => value as u16 + u16:1,\n",
        .replacement =
            "    Choice::Byte(value, other) => value as u16 + u16:1,\n",
        .expected_diagnostic_substr = "",
    },
    {
        .kind = SemanticSumNegativeMutationKind::kMissingPayloadPattern,
        .mutation_name = "missing_payload_pattern",
        .base_seed_id = "source_sample_runner_semantic_sum_argument",
        .needle = "    Choice::Wide(value) => value + u16:2,\n",
        .replacement = "    Choice::Wide() => u16:2,\n",
        .expected_diagnostic_substr = "",
    },
}};

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

absl::StatusOr<const MutationRecipe*> GetRecipe(
    SemanticSumNegativeMutationKind kind) {
  for (const MutationRecipe& recipe : kMutationRecipes) {
    if (recipe.kind == kind) {
      return &recipe;
    }
  }
  return absl::NotFoundError("Unknown semantic-sum mutation kind.");
}

absl::StatusOr<const fuzzer::SemanticSumSeed*> FindSeed(
    const fuzzer::SemanticSumSeedManifest& manifest, std::string_view seed_id) {
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (seed.seed_id() == seed_id) {
      return &seed;
    }
  }
  return absl::NotFoundError(
      absl::StrCat("Could not find semantic-sum seed `", seed_id, "`."));
}

absl::StatusOr<std::string> ReplaceOnce(std::string_view text,
                                        std::string_view needle,
                                        std::string_view replacement) {
  size_t pos = text.find(needle);
  if (pos == std::string_view::npos) {
    return absl::FailedPreconditionError(
        absl::StrCat("Mutation needle not found: `", needle, "`."));
  }
  std::string mutated(text);
  mutated.replace(pos, needle.size(), replacement);
  return mutated;
}

absl::StatusOr<SemanticSumNegativeMutation> MakeSemanticSumNegativeMutation(
    const fuzzer::SemanticSumSeedManifest& manifest,
    const std::filesystem::path& manifest_path,
    SemanticSumNegativeMutationKind kind) {
  XLS_ASSIGN_OR_RETURN(const MutationRecipe* recipe, GetRecipe(kind));
  XLS_ASSIGN_OR_RETURN(const fuzzer::SemanticSumSeed* seed,
                       FindSeed(manifest, recipe->base_seed_id));
  XLS_ASSIGN_OR_RETURN(std::string seed_text,
                       ReadSemanticSumSeedText(manifest_path, *seed));
  XLS_ASSIGN_OR_RETURN(
      std::string mutated_text,
      ReplaceOnce(seed_text, recipe->needle, recipe->replacement));
  return SemanticSumNegativeMutation{
      .mutation_name = std::string(recipe->mutation_name),
      .mutated_text = std::move(mutated_text),
      .expected_diagnostic_substr =
          std::string(recipe->expected_diagnostic_substr),
  };
}

absl::Status VerifyNegativeProgram(std::string_view case_name,
                                   std::string_view program_text,
                                   std::string_view expected_substr) {
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  absl::Status status =
      dslx::ParseAndTypecheck(program_text, absl::StrCat(case_name, ".x"),
                              std::string(case_name), &import_data)
          .status();
  if (status.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("Negative semantic-sum case `", case_name,
                     "` unexpectedly succeeded."));
  }
  if (!expected_substr.empty() &&
      !absl::StrContains(status.message(), expected_substr)) {
    return absl::FailedPreconditionError(
        absl::StrCat("Negative semantic-sum case `", case_name,
                     "` failed with unexpected diagnostic: ", status));
  }
  return absl::OkStatus();
}

TEST(SemanticSumNegativeMutationFuzzTest, ReplaysManifestSeeds) {
  int64_t verified = 0;
  XLS_ASSERT_OK(ReplaySemanticSumSeeds(
      GetManifestPath(), fuzzer::SEMANTIC_SUM_SEED_SURFACE_NEGATIVE_MUTATION,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        ++verified;
        return VerifyNegativeProgram(seed.seed_id(), seed_text,
                                     seed.expected_diagnostic_substr());
      }));
  EXPECT_EQ(verified, 3);
}

void GeneratedMutationIsRejected(SemanticSumNegativeMutationKind kind) {
  std::filesystem::path manifest_path = GetManifestPath();
  XLS_ASSERT_OK_AND_ASSIGN(fuzzer::SemanticSumSeedManifest manifest,
                           LoadSemanticSumSeedManifest(manifest_path));
  XLS_ASSERT_OK_AND_ASSIGN(
      SemanticSumNegativeMutation mutation,
      MakeSemanticSumNegativeMutation(manifest, manifest_path, kind));
  XLS_ASSERT_OK(VerifyNegativeProgram(mutation.mutation_name,
                                      mutation.mutated_text,
                                      mutation.expected_diagnostic_substr));
}

FUZZ_TEST(SemanticSumNegativeMutationFuzzTest, GeneratedMutationIsRejected)
    .WithDomains(fuzztest::ElementOf<SemanticSumNegativeMutationKind>(
        {SemanticSumNegativeMutationKind::kAsymmetricOrPattern,
         SemanticSumNegativeMutationKind::kDuplicatePayloadField,
         SemanticSumNegativeMutationKind::kWildcardConstructorMix,
         SemanticSumNegativeMutationKind::kTooManyPayloadPatterns,
         SemanticSumNegativeMutationKind::kMissingPayloadPattern}));

}  // namespace
}  // namespace xls
