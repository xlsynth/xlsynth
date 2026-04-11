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

#include "xls/fuzzer/semantic_sum_source_seed_replay.h"

#include <filesystem>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/file/temp_directory.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"

namespace xls {
namespace {

using ::testing::HasSubstr;

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

TEST(SemanticSumSourceSeedReplayTest, ReplaysPassingAndFailingSourceSeeds) {
  XLS_ASSERT_OK_AND_ASSIGN(auto temp_dir, TempDirectory::Create());

  SampleOptions sample_options;
  sample_options.set_input_is_dslx(true);
  sample_options.set_sample_type(fuzzer::SAMPLE_TYPE_FUNCTION);
  sample_options.set_ir_converter_args({"--top=main"});
  sample_options.set_convert_to_ir(true);
  sample_options.set_optimize_ir(true);
  sample_options.set_calls_per_sample(3);

  XLS_ASSERT_OK_AND_ASSIGN(auto stats,
                           ReplaySemanticSumSourceSeeds(
                               GetManifestPath(), sample_options,
                               /*seed=*/0, temp_dir.path()));

  EXPECT_EQ(stats.passing_seed_count, 9);
  EXPECT_EQ(stats.failing_seed_count, 4);

  XLS_ASSERT_OK_AND_ASSIGN(
      auto failing_diagnostic,
      GetFileContents(temp_dir.path() /
                      "seed-source_or_pattern_binding_nonfinal" /
                      "diagnostic.txt"));
  EXPECT_THAT(failing_diagnostic,
              HasSubstr("Cannot bind names in a match arm with multiple patterns"));

  std::filesystem::path passing_seed_dir =
      temp_dir.path() /
      "seed-source_exhaustive_constructor_match_without_wildcard";
  EXPECT_TRUE(std::filesystem::exists(passing_seed_dir / "sample.x"));
  EXPECT_TRUE(std::filesystem::exists(passing_seed_dir / "sample.ir"));
  EXPECT_TRUE(std::filesystem::exists(passing_seed_dir / "sample.opt.ir"));
}

}  // namespace
}  // namespace xls
