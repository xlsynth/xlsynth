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
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/file/temp_directory.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/run_fuzz.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/fuzzer/value_generator.h"
#include "xls/ir/format_preference.h"
#include "xls/tests/testvector.pb.h"

namespace xls {
namespace {

struct ParsedReplayModule {
  std::string executable_text;
  std::unique_ptr<dslx::ImportData> import_data;
  dslx::TypecheckedModule tm;
  dslx::Function* function;
};

std::vector<const dslx::Type*> UnwrapUniquePtrs(
    const std::vector<std::unique_ptr<dslx::Type>>& wrapped) {
  std::vector<const dslx::Type*> unwrapped;
  unwrapped.reserve(wrapped.size());
  for (const std::unique_ptr<dslx::Type>& item : wrapped) {
    unwrapped.push_back(item.get());
  }
  return unwrapped;
}

absl::StatusOr<dslx::Function*> GetEntryFunction(dslx::Module& module) {
  auto functions = module.GetFunctionByName();
  auto it = functions.find("main");
  if (it != functions.end()) {
    return it->second;
  }
  if (functions.size() == 1) {
    return functions.begin()->second;
  }
  return absl::NotFoundError(absl::StrCat(
      "Expected a `main` function or exactly one top-level function in module `",
      module.name(), "`"));
}

absl::StatusOr<std::string> WrapEntryFunctionAsMain(
    std::string_view seed_text, const dslx::Function& function,
    const dslx::FunctionType& function_type) {
  std::vector<std::string> params;
  std::vector<std::string> arg_names;
  params.reserve(function.params().size());
  arg_names.reserve(function.params().size());
  for (dslx::Param* param : function.params()) {
    params.push_back(param->ToString());
    arg_names.push_back(param->identifier());
  }
  return absl::StrCat(seed_text, "\n\nfn main(",
                      absl::StrJoin(params, ", "), ") -> ",
                      function_type.return_type().ToString(), " {\n  ",
                      function.identifier(), "(",
                      absl::StrJoin(arg_names, ", "), ")\n}\n");
}

absl::StatusOr<const dslx::FunctionType*> GetFunctionTypeOfFunction(
    dslx::Function* function, const dslx::TypecheckedModule& tm) {
  return tm.type_info->GetItemAs<dslx::FunctionType>(function);
}

absl::StatusOr<std::vector<std::unique_ptr<dslx::Type>>> GetParamTypesOfFunction(
    dslx::Function* function, const dslx::TypecheckedModule& tm) {
  XLS_ASSIGN_OR_RETURN(const dslx::FunctionType * function_type,
                       GetFunctionTypeOfFunction(function, tm));
  std::vector<std::unique_ptr<dslx::Type>> params;
  for (const std::unique_ptr<dslx::Type>& param : function_type->params()) {
    params.push_back(param->CloneToUnique());
  }
  return params;
}

std::string ToArgString(const dslx::InterpValue& value) {
  return value.ConvertToIr().value().ToString(FormatPreference::kHex);
}

std::string InterpValueListToString(
    const std::vector<dslx::InterpValue>& values) {
  return absl::StrJoin(values, "; ",
                       [](std::string* out, const dslx::InterpValue& value) {
                         absl::StrAppend(out, ToArgString(value));
                       });
}

absl::StatusOr<testvector::SampleInputsProto> BuildFunctionTestvector(
    const fuzzer::SemanticSumSeed& seed, dslx::Function* function,
    const dslx::TypecheckedModule& tm, const SampleOptions& sample_options,
    absl::BitGenRef bit_gen) {
  testvector::SampleInputsProto testvector;
  testvector::FunctionArgsProto* function_args =
      testvector.mutable_function_args();
  if (seed.sample_args_size() > 0) {
    for (const std::string& args : seed.sample_args()) {
      function_args->add_args(args);
    }
    return testvector;
  }
  XLS_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<dslx::Type>> params,
                       GetParamTypesOfFunction(function, tm));
  std::vector<const dslx::Type*> param_ptrs = UnwrapUniquePtrs(params);

  for (int64_t i = 0; i < sample_options.calls_per_sample(); ++i) {
    XLS_ASSIGN_OR_RETURN(std::vector<dslx::InterpValue> args,
                         GenerateInterpValues(bit_gen, param_ptrs));
    function_args->add_args(InterpValueListToString(args));
  }
  return testvector;
}

absl::StatusOr<ParsedReplayModule> ParseReplayModule(
    std::string executable_text, const fuzzer::SemanticSumSeed& seed) {
  std::unique_ptr<dslx::ImportData> import_data =
      dslx::CreateImportDataPtrForTest();
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(executable_text, seed.relative_path(),
                              seed.seed_id(), import_data.get()));
  XLS_ASSIGN_OR_RETURN(dslx::Function * function, GetEntryFunction(*tm.module));
  return ParsedReplayModule{std::move(executable_text), std::move(import_data),
                            std::move(tm), function};
}

absl::Status CheckExpectedDiagnostic(const fuzzer::SemanticSumSeed& seed,
                                     const absl::Status& status) {
  if (!seed.expected_diagnostic_substr().empty() &&
      !absl::StrContains(status.message(), seed.expected_diagnostic_substr())) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Source seed `", seed.seed_id(),
        "` failed, but diagnostic did not contain expected substring `",
        seed.expected_diagnostic_substr(), "`: ", status));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::filesystem::path> GetSeedRunDir(
    const std::optional<std::filesystem::path>& run_root,
    std::optional<TempDirectory>& temp_dir,
    const fuzzer::SemanticSumSeed& seed) {
  if (run_root.has_value()) {
    XLS_RETURN_IF_ERROR(RecursivelyCreateDir(*run_root));
    std::filesystem::path run_dir = *run_root / absl::StrCat("seed-", seed.seed_id());
    XLS_RETURN_IF_ERROR(RecursivelyCreateDir(run_dir));
    return run_dir;
  }
  XLS_ASSIGN_OR_RETURN(temp_dir, TempDirectory::Create());
  return temp_dir->path();
}

absl::Status ReplayParsedSourceSeed(
    const fuzzer::SemanticSumSeed& seed, const ParsedReplayModule& parsed,
    const SampleOptions& sample_options, absl::BitGenRef bit_gen,
    const std::optional<std::filesystem::path>& run_root,
    const std::optional<std::filesystem::path>& summary_file) {
  XLS_ASSIGN_OR_RETURN(testvector::SampleInputsProto testvector,
                       BuildFunctionTestvector(seed, parsed.function, parsed.tm,
                                              sample_options,
                                              bit_gen));

  std::optional<TempDirectory> temp_dir;
  XLS_ASSIGN_OR_RETURN(std::filesystem::path run_dir,
                       GetSeedRunDir(run_root, temp_dir, seed));
  Sample sample(parsed.executable_text, sample_options, std::move(testvector));
  XLS_RETURN_IF_ERROR(
      RunSample(sample, run_dir, summary_file).status());
  return absl::OkStatus();
}

absl::Status ReplayPassingSourceSeed(
    const fuzzer::SemanticSumSeed& seed, const std::string& seed_text,
    const SampleOptions& sample_options, absl::BitGenRef bit_gen,
    const std::optional<std::filesystem::path>& run_root,
    const std::optional<std::filesystem::path>& summary_file) {
  XLS_ASSIGN_OR_RETURN(ParsedReplayModule parsed,
                       ParseReplayModule(seed_text, seed));
  if (parsed.function->identifier() == "main") {
    return ReplayParsedSourceSeed(seed, parsed, sample_options, bit_gen,
                                  run_root, summary_file);
  }
  XLS_ASSIGN_OR_RETURN(const dslx::FunctionType * function_type,
                       GetFunctionTypeOfFunction(parsed.function, parsed.tm));
  XLS_ASSIGN_OR_RETURN(std::string wrapped_text,
                       WrapEntryFunctionAsMain(seed_text, *parsed.function,
                                               *function_type));
  XLS_ASSIGN_OR_RETURN(ParsedReplayModule wrapped,
                       ParseReplayModule(std::move(wrapped_text), seed));
  return ReplayParsedSourceSeed(seed, wrapped, sample_options, bit_gen,
                                run_root, summary_file);
}

absl::Status ReplayFailingSourceSeed(
    const fuzzer::SemanticSumSeed& seed, const std::string& seed_text,
    const std::optional<std::filesystem::path>& run_root) {
  std::optional<TempDirectory> temp_dir;
  XLS_ASSIGN_OR_RETURN(std::filesystem::path run_dir,
                       GetSeedRunDir(run_root, temp_dir, seed));
  XLS_RETURN_IF_ERROR(SetFileContents(run_dir / "sample.x", seed_text));

  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  absl::Status status =
      dslx::ParseAndTypecheck(seed_text, seed.relative_path(), seed.seed_id(),
                              &import_data)
          .status();
  if (status.ok()) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Source seed `", seed.seed_id(),
        "` unexpectedly parsed and typechecked successfully."));
  }
  XLS_RETURN_IF_ERROR(CheckExpectedDiagnostic(seed, status));
  XLS_RETURN_IF_ERROR(SetFileContents(run_dir / "diagnostic.txt", status.ToString()));
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<SemanticSumSourceReplayStats> ReplaySemanticSumSourceSeeds(
    const std::filesystem::path& manifest_path,
    const SampleOptions& sample_options, std::optional<uint64_t> seed,
    const std::optional<std::filesystem::path>& run_root,
    const std::optional<std::filesystem::path>& summary_file) {
  CHECK(sample_options.IsFunctionSample());

  SemanticSumSourceReplayStats stats;
  absl::BitGen bit_gen;
  std::mt19937_64 deterministic_engine(seed.value_or(0));
  absl::BitGenRef replay_gen = seed.has_value() ? absl::BitGenRef(deterministic_engine)
                                                : absl::BitGenRef(bit_gen);

  XLS_RETURN_IF_ERROR(ReplaySemanticSumSeeds(
      manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_SOURCE,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        if (seed.outcome() == fuzzer::SEMANTIC_SUM_SEED_OUTCOME_SHOULD_PASS) {
          ++stats.passing_seed_count;
          return ReplayPassingSourceSeed(seed, seed_text, sample_options,
                                         replay_gen, run_root,
                                         summary_file);
        }
        ++stats.failing_seed_count;
        return ReplayFailingSourceSeed(seed, seed_text, run_root);
      }));
  return stats;
}

}  // namespace xls
