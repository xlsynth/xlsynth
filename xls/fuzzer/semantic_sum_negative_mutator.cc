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

#include "xls/fuzzer/semantic_sum_negative_mutator.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/status_macros.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"

namespace xls {
namespace {

struct MutationRecipe {
  SemanticSumNegativeMutationKind kind;
  std::string_view mutation_name;
  std::string_view base_seed_id;
  std::string_view needle;
  std::string_view replacement;
  std::string_view expected_diagnostic_substr;
};

constexpr std::array<MutationRecipe, 6> kMutationRecipes = {{
    {
        .kind = SemanticSumNegativeMutationKind::kAsymmetricOrPattern,
        .mutation_name = "asymmetric_or_pattern",
        .base_seed_id = "source_exhaustive_constructor_match_without_wildcard",
        .needle =
            "    Option::Some(v) => v,\n"
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
        .needle =
            "    Option::Some(v) => v,\n"
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
    {
        .kind = SemanticSumNegativeMutationKind::kAggregateZeroMaterialization,
        .mutation_name = "aggregate_zero_materialization",
        .base_seed_id = "source_exhaustive_constructor_match_without_wildcard",
        .needle =
            "fn f(x: Option) -> u32 {\n"
            "  match x {\n"
            "    Option::Some(v) => v,\n"
            "    Option::None => u32:0,\n"
            "  }\n"
            "}\n",
        .replacement =
            "fn f(x: Option) -> u32 {\n"
            "  let y = zero!<(Option,)>();\n"
            "  match y {\n"
            "    (Option::Some(v),) => v,\n"
            "    _ => u32:0,\n"
            "  }\n"
            "}\n",
        .expected_diagnostic_substr = "",
    },
}};

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

}  // namespace

absl::StatusOr<SemanticSumNegativeMutation> MakeSemanticSumNegativeMutation(
    const fuzzer::SemanticSumSeedManifest& manifest,
    const std::filesystem::path& manifest_path,
    SemanticSumNegativeMutationKind kind) {
  XLS_ASSIGN_OR_RETURN(const MutationRecipe * recipe, GetRecipe(kind));
  XLS_ASSIGN_OR_RETURN(const fuzzer::SemanticSumSeed * seed,
                       FindSeed(manifest, recipe->base_seed_id));
  XLS_ASSIGN_OR_RETURN(std::string seed_text,
                       ReadSemanticSumSeedText(manifest_path, *seed));
  XLS_ASSIGN_OR_RETURN(std::string mutated_text,
                       ReplaceOnce(seed_text, recipe->needle,
                                   recipe->replacement));
  return SemanticSumNegativeMutation{
      .kind = recipe->kind,
      .mutation_name = std::string(recipe->mutation_name),
      .base_seed_id = seed->seed_id(),
      .mutated_text = std::move(mutated_text),
      .expected_diagnostic_substr =
          std::string(recipe->expected_diagnostic_substr),
  };
}

absl::StatusOr<SemanticSumNegativeMutation> MakeRandomSemanticSumNegativeMutation(
    const fuzzer::SemanticSumSeedManifest& manifest,
    const std::filesystem::path& manifest_path, absl::BitGenRef bit_gen) {
  size_t index = absl::Uniform<size_t>(bit_gen, 0, kMutationRecipes.size());
  return MakeSemanticSumNegativeMutation(manifest, manifest_path,
                                         kMutationRecipes.at(index).kind);
}

}  // namespace xls
