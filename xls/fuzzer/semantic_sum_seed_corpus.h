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

#ifndef XLS_FUZZER_SEMANTIC_SUM_SEED_CORPUS_H_
#define XLS_FUZZER_SEMANTIC_SUM_SEED_CORPUS_H_

#include <filesystem>
#include <string>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.pb.h"

namespace xls {

// Loads and validates the semantic-sum seed manifest rooted at `manifest_path`.
absl::StatusOr<fuzzer::SemanticSumSeedManifest> LoadSemanticSumSeedManifest(
    const std::filesystem::path& manifest_path);

// Returns whether `seed` participates in the requested fuzzing surface.
bool SemanticSumSeedHasSurface(
    const fuzzer::SemanticSumSeed& seed,
    fuzzer::SemanticSumSeedSurface surface);

// Returns manifest seeds that are tagged for `surface`, preserving manifest
// order.
std::vector<const fuzzer::SemanticSumSeed*> GetSemanticSumSeedsForSurface(
    const fuzzer::SemanticSumSeedManifest& manifest,
    fuzzer::SemanticSumSeedSurface surface);

// Reads the seed text file referenced by `seed` relative to `manifest_path`.
absl::StatusOr<std::string> ReadSemanticSumSeedText(
    const std::filesystem::path& manifest_path,
    const fuzzer::SemanticSumSeed& seed);

// Replays all seeds for `surface` through `replay_seed`, preserving manifest
// order and surfacing the first callback error.
absl::Status ReplaySemanticSumSeeds(
    const std::filesystem::path& manifest_path,
    fuzzer::SemanticSumSeedSurface surface,
    absl::FunctionRef<absl::Status(const fuzzer::SemanticSumSeed&,
                                   const std::string&)> replay_seed);

}  // namespace xls

#endif  // XLS_FUZZER_SEMANTIC_SUM_SEED_CORPUS_H_
