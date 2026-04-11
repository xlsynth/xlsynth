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

#ifndef XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATOR_H_
#define XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATOR_H_

#include <filesystem>
#include <string>

#include "absl/random/bit_gen_ref.h"
#include "absl/status/statusor.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.pb.h"

namespace xls {

enum class SemanticSumNegativeMutationKind : unsigned char {
  kAsymmetricOrPattern,
  kDuplicatePayloadField,
  kWildcardConstructorMix,
  kTooManyPayloadPatterns,
  kMissingPayloadPattern,
  kAggregateZeroMaterialization,
};

struct SemanticSumNegativeMutation {
  SemanticSumNegativeMutationKind kind;
  std::string mutation_name;
  std::string base_seed_id;
  std::string mutated_text;
  std::string expected_diagnostic_substr;
};

absl::StatusOr<SemanticSumNegativeMutation> MakeSemanticSumNegativeMutation(
    const fuzzer::SemanticSumSeedManifest& manifest,
    const std::filesystem::path& manifest_path,
    SemanticSumNegativeMutationKind kind);

absl::StatusOr<SemanticSumNegativeMutation> MakeRandomSemanticSumNegativeMutation(
    const fuzzer::SemanticSumSeedManifest& manifest,
    const std::filesystem::path& manifest_path, absl::BitGenRef bit_gen);

}  // namespace xls

#endif  // XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATOR_H_
