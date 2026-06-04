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

#include "xls/fuzzer/semantic_sum_seed_corpus.h"

#include <filesystem>
#include <set>
#include <string>

#include "google/protobuf/text_format.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"

namespace xls {
namespace {

absl::Status ValidateSemanticSumSeedManifest(
    const fuzzer::SemanticSumSeedManifest& manifest) {
  std::set<std::string> seed_ids;
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (seed.seed_id().empty()) {
      return absl::InvalidArgumentError("Semantic-sum seed is missing seed_id.");
    }
    if (!seed_ids.insert(seed.seed_id()).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("Duplicate semantic-sum seed_id: ", seed.seed_id()));
    }
    if (seed.surfaces_size() == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("Semantic-sum seed `", seed.seed_id(),
                       "` is missing surfaces."));
    }
    if (seed.relative_path().empty()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Semantic-sum seed `", seed.seed_id(),
                       "` is missing relative_path."));
    }
    if (std::filesystem::path(seed.relative_path()).is_absolute()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Semantic-sum seed `", seed.seed_id(),
                       "` must use a relative_path."));
    }
    if (seed.outcome() ==
        fuzzer::SEMANTIC_SUM_SEED_OUTCOME_UNSPECIFIED) {
      return absl::InvalidArgumentError(
          absl::StrCat("Semantic-sum seed `", seed.seed_id(),
                       "` is missing an outcome."));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<fuzzer::SemanticSumSeedManifest> LoadSemanticSumSeedManifest(
    const std::filesystem::path& manifest_path) {
  XLS_ASSIGN_OR_RETURN(std::string text, GetFileContents(manifest_path));
  fuzzer::SemanticSumSeedManifest manifest;
  if (!google::protobuf::TextFormat::ParseFromString(text, &manifest)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Could not parse semantic-sum seed manifest: ",
                     manifest_path.string()));
  }
  XLS_RETURN_IF_ERROR(ValidateSemanticSumSeedManifest(manifest));
  return manifest;
}

bool SemanticSumSeedHasSurface(
    const fuzzer::SemanticSumSeed& seed,
    fuzzer::SemanticSumSeedSurface surface) {
  for (int candidate : seed.surfaces()) {
    if (candidate == surface) {
      return true;
    }
  }
  return false;
}

std::vector<const fuzzer::SemanticSumSeed*> GetSemanticSumSeedsForSurface(
    const fuzzer::SemanticSumSeedManifest& manifest,
    fuzzer::SemanticSumSeedSurface surface) {
  std::vector<const fuzzer::SemanticSumSeed*> seeds;
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (SemanticSumSeedHasSurface(seed, surface)) {
      seeds.push_back(&seed);
    }
  }
  return seeds;
}

absl::StatusOr<std::string> ReadSemanticSumSeedText(
    const std::filesystem::path& manifest_path,
    const fuzzer::SemanticSumSeed& seed) {
  const std::filesystem::path seed_path =
      manifest_path.parent_path() / seed.relative_path();
  XLS_ASSIGN_OR_RETURN(std::string seed_text, GetFileContents(seed_path));
  return seed_text;
}

absl::Status ReplaySemanticSumSeeds(
    const std::filesystem::path& manifest_path,
    fuzzer::SemanticSumSeedSurface surface,
    absl::FunctionRef<absl::Status(const fuzzer::SemanticSumSeed&,
                                   const std::string&)> replay_seed) {
  XLS_ASSIGN_OR_RETURN(fuzzer::SemanticSumSeedManifest manifest,
                       LoadSemanticSumSeedManifest(manifest_path));
  for (const fuzzer::SemanticSumSeed* seed :
       GetSemanticSumSeedsForSurface(manifest, surface)) {
    XLS_ASSIGN_OR_RETURN(std::string seed_text,
                         ReadSemanticSumSeedText(manifest_path, *seed));
    XLS_RETURN_IF_ERROR(replay_seed(*seed, seed_text));
  }
  return absl::OkStatus();
}

}  // namespace xls
