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

#include "xls/fuzzer/semantic_sum_typeinfo_layout_fuzz.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/stopwatch.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.pb.h"
#include "xls/dslx/type_system/type_info_to_proto.h"
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

absl::StatusOr<const dslx::SumType*> GetConcreteSumType(
    const dslx::TypecheckedModule& tm, const dslx::SumDef& sum_def) {
  for (const auto& [_, function] : tm.module->GetFunctionByName()) {
    XLS_ASSIGN_OR_RETURN(dslx::FunctionType * function_type,
                         tm.type_info->GetItemAs<dslx::FunctionType>(function));
    if (auto* return_sum_type =
            dynamic_cast<const dslx::SumType*>(&function_type->return_type());
        return_sum_type != nullptr &&
        return_sum_type->nominal_type().identifier() == sum_def.identifier()) {
      return return_sum_type;
    }
    for (const std::unique_ptr<dslx::Type>& param_type : function_type->params()) {
      auto* param_sum_type =
          dynamic_cast<const dslx::SumType*>(param_type.get());
      if (param_sum_type != nullptr &&
          param_sum_type->nominal_type().identifier() == sum_def.identifier()) {
        return param_sum_type;
      }
    }
  }
  return absl::NotFoundError(absl::StrCat(
      "Could not find a concrete sum type for `", sum_def.identifier(), "`."));
}

absl::Status VerifySumMetadata(std::string_view case_name,
                               std::string_view program_text) {
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(program_text, absl::StrCat(case_name, ".x"),
                              case_name, &import_data));
  XLS_ASSIGN_OR_RETURN(dslx::TypeInfoProto proto,
                       dslx::TypeInfoToProto(*tm.type_info));

  std::vector<dslx::SumDef*> sum_defs = tm.module->GetSumDefs();
  if (sum_defs.empty()) {
    return absl::InvalidArgumentError("Program did not contain a sum definition.");
  }

  for (dslx::SumDef* sum_def : sum_defs) {
    XLS_ASSIGN_OR_RETURN(const dslx::SumType * sum_type,
                         GetConcreteSumType(tm, *sum_def));
    const dslx::SumTypeProto* sum_proto = nullptr;
    for (const dslx::AstNodeTypeInfoProto& node : proto.nodes()) {
      if (!node.has_type() || !node.type().has_sum_type()) {
        continue;
      }
      const dslx::SumTypeProto& candidate = node.type().sum_type();
      if (candidate.has_sum_def() &&
          candidate.sum_def().identifier() == sum_def->identifier()) {
        sum_proto = &candidate;
        break;
      }
    }
    if (sum_proto == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("Missing SumTypeProto for `", sum_def->identifier(), "`."));
    }
    if (!sum_proto->has_sum_def()) {
      return absl::FailedPreconditionError("SumTypeProto missing sum_def.");
    }
    if (sum_proto->sum_def().variants_size() != sum_def->variants().size()) {
      return absl::FailedPreconditionError(
          "SumTypeProto variant count did not match AST sum definition.");
    }
    if (sum_proto->variants_size() != sum_type->variants().size()) {
      return absl::FailedPreconditionError(
          "SumTypeProto concrete variant count did not match checked sum type.");
    }

    int64_t payload_member_count = 0;
    for (int64_t i = 0; i < sum_def->variants().size(); ++i) {
      const dslx::SumVariant* ast_variant = sum_def->variants().at(i);
      const dslx::SumVariantDefProto& proto_variant =
          sum_proto->sum_def().variants(i);
      if (proto_variant.identifier() != ast_variant->identifier()) {
        return absl::FailedPreconditionError(
            "SumTypeProto variant order did not match declaration order.");
      }
      dslx::SumVariantKindProto expected_kind = dslx::SUM_VARIANT_KIND_UNIT;
      if (ast_variant->is_tuple()) {
        expected_kind = dslx::SUM_VARIANT_KIND_TUPLE;
      } else if (ast_variant->is_struct()) {
        expected_kind = dslx::SUM_VARIANT_KIND_STRUCT;
      }
      if (proto_variant.kind() != expected_kind) {
        return absl::FailedPreconditionError(
            "SumTypeProto variant kind did not match AST declaration kind.");
      }
      payload_member_count += sum_type->variants().at(i).size();
    }

    const dslx::Phase1SumTypeEncoding encoding(*sum_type);
    if (encoding.payload_slot_count() != payload_member_count) {
      return absl::FailedPreconditionError(
          "Phase1SumTypeEncoding payload slot count did not match payload members.");
    }

    int64_t next_payload_start = 0;
    XLS_RETURN_IF_ERROR(encoding.ForEachVariant(
        [&](const dslx::Phase1SumTypeEncoding::VariantInfo& variant)
            -> absl::Status {
          if (variant.variant_index < 0 ||
              variant.variant_index >= sum_type->variant_count()) {
            return absl::FailedPreconditionError("Variant index out of bounds.");
          }
          if (variant.payload_start != next_payload_start) {
            return absl::FailedPreconditionError(
                "Variant payload offsets were not monotonic.");
          }
          next_payload_start += variant.payload_size();
          return absl::OkStatus();
        }));
  }

  return absl::OkStatus();
}

std::string GenerateVariantDecl(int64_t index, int64_t kind_choice) {
  switch (kind_choice) {
    case 0:
      return absl::StrFormat("  V%d,\n", index);
    case 1:
      return absl::StrFormat("  V%d(u%d),\n", index, 8 * (index + 1));
    case 2:
      return absl::StrFormat("  V%d(),\n", index);
    case 3:
      return absl::StrFormat("  V%d { x: u%d },\n", index, 8 * (index + 1));
    default:
      return absl::StrFormat("  V%d { },\n", index);
  }
}

std::string GenerateConstructorExpr(int64_t index, int64_t kind_choice) {
  switch (kind_choice) {
    case 0:
      return absl::StrFormat("Generated::V%d", index);
    case 1:
      return absl::StrFormat("Generated::V%d(u%d:0)", index, 8 * (index + 1));
    case 2:
      return absl::StrFormat("Generated::V%d()", index);
    case 3:
      return absl::StrFormat("Generated::V%d { x: u%d:0 }", index,
                             8 * (index + 1));
    default:
      return absl::StrFormat("Generated::V%d { }", index);
  }
}

std::string GenerateRandomProgram(absl::BitGenRef bit_gen) {
  int64_t variant_count = absl::Uniform(bit_gen, 2, 5);
  std::string program = "sum Generated {\n";
  std::vector<int64_t> kind_choices;
  kind_choices.reserve(variant_count);
  for (int64_t i = 0; i < variant_count; ++i) {
    int64_t kind_choice = absl::Uniform(bit_gen, 0, 5);
    kind_choices.push_back(kind_choice);
    absl::StrAppend(&program, GenerateVariantDecl(i, kind_choice));
  }
  absl::StrAppend(&program, "}\n\nfn main(x: bool) -> Generated {\n");
  absl::StrAppend(&program, "  if x { ",
                  GenerateConstructorExpr(0, kind_choices.front()),
                  " } else { ",
                  GenerateConstructorExpr(variant_count - 1, kind_choices.back()),
                  " }\n}\n");
  return program;
}

}  // namespace

absl::StatusOr<SemanticSumTypeinfoLayoutFuzzStats>
RunSemanticSumTypeinfoLayoutFuzz(
    const SemanticSumTypeinfoLayoutFuzzOptions& options) {
  if (!options.duration.has_value() && !options.iteration_count.has_value()) {
    return absl::InvalidArgumentError(
        "Typeinfo/layout fuzz options require duration or iteration_count.");
  }
  XLS_RETURN_IF_ERROR(CheckOrCreateWritableDirectory(options.artifact_dir));

  SemanticSumTypeinfoLayoutFuzzStats stats;
  XLS_RETURN_IF_ERROR(ReplaySemanticSumSeeds(
      options.manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_TYPEINFO_LAYOUT,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        XLS_RETURN_IF_ERROR(VerifySumMetadata(seed.seed_id(), seed_text));
        ++stats.seed_cases_verified;
        return absl::OkStatus();
      }));

  absl::BitGen nondeterministic_gen;
  std::mt19937_64 deterministic_gen(options.seed.value_or(0));
  absl::BitGenRef bit_gen = options.seed.has_value()
                                ? absl::BitGenRef(deterministic_gen)
                                : absl::BitGenRef(nondeterministic_gen);
  Stopwatch stopwatch;
  while (true) {
    if (options.iteration_count.has_value() &&
        stats.generated_cases_verified >= *options.iteration_count) {
      break;
    }
    if (options.duration.has_value() &&
        stopwatch.GetElapsedTime() >= *options.duration) {
      break;
    }
    XLS_RETURN_IF_ERROR(VerifySumMetadata("generated_typeinfo_case",
                                          GenerateRandomProgram(bit_gen)));
    ++stats.generated_cases_verified;
  }

  XLS_RETURN_IF_ERROR(SetFileContents(
      options.artifact_dir / "summary.txt",
      absl::StrCat("seed_cases_verified=", stats.seed_cases_verified,
                   "\ngenerated_cases_verified=",
                   stats.generated_cases_verified, "\n")));
  return stats;
}

}  // namespace xls
