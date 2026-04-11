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
#include <optional>
#include <random>
#include <string>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/stopwatch.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/fuzzer/semantic_sum_negative_mutator.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"

namespace xls {
namespace {

absl::Status CheckOrCreateWritableDirectory(const std::filesystem::path& path) {
  XLS_RETURN_IF_ERROR(RecursivelyCreateDir(path));
  if (!std::filesystem::is_directory(path)) {
    return absl::InvalidArgumentError(
        absl::StrCat(path.string(), " is not a directory"));
  }
  return absl::OkStatus();
}

absl::Status VerifyNegativeProgram(std::string_view case_name,
                                   std::string_view program_text,
                                   std::string_view expected_substr,
                                   const std::filesystem::path& artifact_dir,
                                   int64_t sequence_number) {
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  absl::Status status =
      dslx::ParseAndTypecheck(program_text, absl::StrCat(case_name, ".x"),
                              std::string(case_name), &import_data)
          .status();
  if (status.ok()) {
    std::filesystem::path failure_dir =
        artifact_dir / absl::StrCat("unexpected_pass_", sequence_number);
    XLS_RETURN_IF_ERROR(RecursivelyCreateDir(failure_dir));
    XLS_RETURN_IF_ERROR(SetFileContents(failure_dir / "sample.x",
                                        std::string(program_text)));
    XLS_RETURN_IF_ERROR(SetFileContents(
        failure_dir / "diagnostic.txt",
        "Program unexpectedly parsed and typechecked successfully."));
    return absl::FailedPreconditionError(
        absl::StrCat("Negative semantic-sum case `", case_name,
                     "` unexpectedly succeeded."));
  }
  if (!expected_substr.empty() &&
      !absl::StrContains(status.message(), expected_substr)) {
    std::filesystem::path failure_dir =
        artifact_dir / absl::StrCat("unexpected_diagnostic_", sequence_number);
    XLS_RETURN_IF_ERROR(RecursivelyCreateDir(failure_dir));
    XLS_RETURN_IF_ERROR(SetFileContents(failure_dir / "sample.x",
                                        std::string(program_text)));
    XLS_RETURN_IF_ERROR(
        SetFileContents(failure_dir / "diagnostic.txt", status.ToString()));
    return absl::FailedPreconditionError(
        absl::StrCat("Negative semantic-sum case `", case_name,
                     "` failed with unexpected diagnostic: ", status));
  }
  return absl::OkStatus();
}

absl::StatusOr<int64_t> VerifyManifestNegativeSeeds(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& artifact_dir) {
  int64_t verified = 0;
  XLS_RETURN_IF_ERROR(ReplaySemanticSumSeeds(
      manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_NEGATIVE_MUTATION,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        ++verified;
        return VerifyNegativeProgram(seed.seed_id(), seed_text,
                                     seed.expected_diagnostic_substr(),
                                     artifact_dir, verified);
      }));
  return verified;
}

}  // namespace

absl::StatusOr<SemanticSumNegativeMutationFuzzStats>
RunSemanticSumNegativeMutationFuzz(
    const SemanticSumNegativeMutationFuzzOptions& options) {
  if (!options.duration.has_value() && !options.iteration_count.has_value()) {
    return absl::InvalidArgumentError(
        "Negative mutation fuzz options require duration or iteration_count.");
  }
  XLS_RETURN_IF_ERROR(CheckOrCreateWritableDirectory(options.artifact_dir));
  XLS_ASSIGN_OR_RETURN(fuzzer::SemanticSumSeedManifest manifest,
                       LoadSemanticSumSeedManifest(options.manifest_path));

  SemanticSumNegativeMutationFuzzStats stats;
  XLS_ASSIGN_OR_RETURN(stats.manifest_seed_failures_verified,
                       VerifyManifestNegativeSeeds(options.manifest_path,
                                                  options.artifact_dir));

  absl::BitGen nondeterministic_gen;
  std::mt19937_64 deterministic_gen(options.seed.value_or(0));
  absl::BitGenRef bit_gen = options.seed.has_value()
                                ? absl::BitGenRef(deterministic_gen)
                                : absl::BitGenRef(nondeterministic_gen);

  Stopwatch stopwatch;
  while (true) {
    if (options.iteration_count.has_value() &&
        stats.mutations_verified >= *options.iteration_count) {
      break;
    }
    if (options.duration.has_value() &&
        stopwatch.GetElapsedTime() >= *options.duration) {
      break;
    }

    XLS_ASSIGN_OR_RETURN(
        SemanticSumNegativeMutation mutation,
        MakeRandomSemanticSumNegativeMutation(manifest, options.manifest_path,
                                              bit_gen));
    XLS_RETURN_IF_ERROR(VerifyNegativeProgram(
        mutation.mutation_name, mutation.mutated_text,
        mutation.expected_diagnostic_substr, options.artifact_dir,
        stats.manifest_seed_failures_verified + stats.mutations_verified + 1));
    ++stats.mutations_verified;
  }

  XLS_RETURN_IF_ERROR(SetFileContents(
      options.artifact_dir / "summary.txt",
      absl::StrCat("manifest_seed_failures_verified=",
                   stats.manifest_seed_failures_verified,
                   "\nmutations_verified=", stats.mutations_verified, "\n")));
  return stats;
}

}  // namespace xls
