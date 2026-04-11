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

#ifndef XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATION_FUZZ_H_
#define XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATION_FUZZ_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include "absl/status/statusor.h"
#include "absl/time/time.h"

namespace xls {

struct SemanticSumNegativeMutationFuzzOptions {
  std::filesystem::path manifest_path;
  std::filesystem::path artifact_dir;
  std::optional<absl::Duration> duration;
  std::optional<int64_t> iteration_count;
  std::optional<uint64_t> seed;
};

struct SemanticSumNegativeMutationFuzzStats {
  int64_t manifest_seed_failures_verified = 0;
  int64_t mutations_verified = 0;
};

absl::StatusOr<SemanticSumNegativeMutationFuzzStats>
RunSemanticSumNegativeMutationFuzz(
    const SemanticSumNegativeMutationFuzzOptions& options);

}  // namespace xls

#endif  // XLS_FUZZER_SEMANTIC_SUM_NEGATIVE_MUTATION_FUZZ_H_
