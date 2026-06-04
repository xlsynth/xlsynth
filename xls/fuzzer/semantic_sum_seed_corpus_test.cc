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
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"

namespace xls {
namespace {

using ::testing::Contains;
using ::testing::SizeIs;

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
      "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

TEST(SemanticSumSeedCorpusTest, LoadsManifestAndFiltersBySurface) {
  XLS_ASSERT_OK_AND_ASSIGN(fuzzer::SemanticSumSeedManifest manifest,
                           LoadSemanticSumSeedManifest(GetManifestPath()));

  EXPECT_THAT(manifest.seeds(), SizeIs(12));

  std::vector<std::string> source_seed_ids;
  for (const fuzzer::SemanticSumSeed* seed :
       GetSemanticSumSeedsForSurface(
           manifest, fuzzer::SEMANTIC_SUM_SEED_SURFACE_SOURCE)) {
    source_seed_ids.push_back(seed->seed_id());
  }
  EXPECT_THAT(source_seed_ids,
              Contains("source_exhaustive_constructor_match_without_wildcard"));
  EXPECT_THAT(source_seed_ids,
              Contains("source_sample_runner_semantic_sum_argument"));

  const fuzzer::SemanticSumSeed* assert_eq_seed = nullptr;
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (seed.seed_id() == "source_semantic_sum_assert_eq_observer") {
      assert_eq_seed = &seed;
      break;
    }
  }
  ASSERT_NE(assert_eq_seed, nullptr);
  EXPECT_THAT(assert_eq_seed->sample_args(), SizeIs(3));
  EXPECT_THAT(assert_eq_seed->sample_args(),
              Contains("(bits[1]:0x1, (bits[32]:0x7fffffff)); "
                       "(bits[1]:0x1, (bits[32]:0x7fffffff))"));
}

}  // namespace
}  // namespace xls
