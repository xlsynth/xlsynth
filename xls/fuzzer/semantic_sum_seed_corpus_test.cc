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
using ::testing::ContainsRegex;
using ::testing::SizeIs;

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
      "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

TEST(SemanticSumSeedCorpusTest, LoadsManifestAndFiltersBySurface) {
  XLS_ASSERT_OK_AND_ASSIGN(fuzzer::SemanticSumSeedManifest manifest,
                           LoadSemanticSumSeedManifest(GetManifestPath()));

  EXPECT_THAT(manifest.seeds(), SizeIs(17));

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

  std::vector<std::string> raw_seed_ids;
  for (const fuzzer::SemanticSumSeed* seed :
       GetSemanticSumSeedsForSurface(
           manifest, fuzzer::SEMANTIC_SUM_SEED_SURFACE_RAW_BOUNDARY)) {
    raw_seed_ids.push_back(seed->seed_id());
  }
  EXPECT_THAT(raw_seed_ids, SizeIs(3));

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

TEST(SemanticSumSeedCorpusTest, ReplaysSeedTextsFromManifestRoot) {
  std::vector<std::string> seen_seed_ids;
  std::vector<std::string> seen_texts;
  XLS_ASSERT_OK(ReplaySemanticSumSeeds(
      GetManifestPath(), fuzzer::SEMANTIC_SUM_SEED_SURFACE_TYPEINFO_LAYOUT,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        seen_seed_ids.push_back(seed.seed_id());
        seen_texts.push_back(seed_text);
        return absl::OkStatus();
      }));

  EXPECT_THAT(seen_seed_ids,
              Contains("typeinfo_layout_basic_sum_shape"));
  EXPECT_THAT(seen_seed_ids,
              Contains("typeinfo_layout_empty_payload_kinds"));
  EXPECT_THAT(seen_texts,
              Contains(ContainsRegex(R"(sum Option \{)")));
  EXPECT_THAT(seen_texts,
              Contains(ContainsRegex(R"(EmptyStruct \{ \})")));
}

}  // namespace
}  // namespace xls
