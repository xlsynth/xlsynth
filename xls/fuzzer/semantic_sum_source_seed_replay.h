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

#ifndef XLS_FUZZER_SEMANTIC_SUM_SOURCE_SEED_REPLAY_H_
#define XLS_FUZZER_SEMANTIC_SUM_SOURCE_SEED_REPLAY_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include "absl/status/statusor.h"
#include "xls/fuzzer/sample.h"

namespace xls {

struct SemanticSumSourceReplayStats {
  int64_t passing_seed_count = 0;
  int64_t failing_seed_count = 0;
};

// Replays semantic-sum source seeds in manifest order.
//
// Passing seeds run through the normal function-sample execution path. Failing
// seeds are parsed and typechecked directly and must produce the manifest's
// expected diagnostic substring when one is specified.
absl::StatusOr<SemanticSumSourceReplayStats> ReplaySemanticSumSourceSeeds(
    const std::filesystem::path& manifest_path,
    const SampleOptions& sample_options, std::optional<uint64_t> seed,
    const std::optional<std::filesystem::path>& run_root = std::nullopt,
    const std::optional<std::filesystem::path>& summary_file = std::nullopt);

}  // namespace xls

#endif  // XLS_FUZZER_SEMANTIC_SUM_SOURCE_SEED_REPLAY_H_
