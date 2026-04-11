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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "xls/common/exit_status.h"
#include "xls/common/init_xls.h"
#include "xls/fuzzer/semantic_sum_raw_boundary_fuzz.h"

ABSL_FLAG(absl::Duration, duration, absl::InfiniteDuration(),
          "Duration to run the semantic-sum raw-boundary fuzzer.");
ABSL_FLAG(std::optional<int64_t>, seed, std::nullopt,
          "Optional deterministic seed.");
ABSL_FLAG(std::string, seed_manifest, "",
          "Path to the semantic-sum seed manifest.");
ABSL_FLAG(std::string, artifact_dir, "",
          "Directory for failure artifacts and run summaries.");

int main(int argc, char** argv) {
  std::vector<std::string_view> positional_arguments = xls::InitXls(
      absl::StrCat("Runs the semantic-sum raw-boundary fuzzer: ", argv[0]),
      argc, argv);
  if (!positional_arguments.empty()) {
    LOG(QFATAL) << "Unexpected positional arguments.";
  }
  return xls::ExitStatus(
      xls::RunSemanticSumRawBoundaryFuzz({
          .manifest_path = absl::GetFlag(FLAGS_seed_manifest),
          .artifact_dir = absl::GetFlag(FLAGS_artifact_dir),
          .duration = absl::GetFlag(FLAGS_duration),
          .iteration_count = std::nullopt,
          .seed = absl::GetFlag(FLAGS_seed),
      }).status());
}
