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

#include "xls/fuzzer/semantic_sum_negative_mutation_fuzz.h"

#include <filesystem>

#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/file/temp_directory.h"
#include "xls/common/status/matchers.h"

namespace xls {
namespace {

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

TEST(SemanticSumNegativeMutationFuzzTest, ReplaysManifestAndRandomMutations) {
  XLS_ASSERT_OK_AND_ASSIGN(auto temp_dir, TempDirectory::Create());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto stats,
      RunSemanticSumNegativeMutationFuzz({
          .manifest_path = GetManifestPath(),
          .artifact_dir = temp_dir.path(),
          .duration = std::nullopt,
          .iteration_count = 24,
          .seed = 0,
      }));

  EXPECT_EQ(stats.manifest_seed_failures_verified, 4);
  EXPECT_EQ(stats.mutations_verified, 24);
  EXPECT_TRUE(std::filesystem::exists(temp_dir.path() / "summary.txt"));
}

}  // namespace
}  // namespace xls
