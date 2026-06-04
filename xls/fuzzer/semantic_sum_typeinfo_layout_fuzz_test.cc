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

// Covers durable serialized metadata for bounded generated sum declarations.
// FUZZ_TEST generates a vector of two to four variant-kind choices; each choice
// becomes a bare unit, one-field tuple, empty tuple, one-field struct, or empty
// struct variant in a fixed enum plus main-function shell.
//
// The property validates serialized variant existence, count, declaration
// order, and unit/tuple/struct kind. It does not fuzz invalid programs,
// parametrics, nested sums, payload values, payload-slot widths, or offsets.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/fuzzing/fuzztest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
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

// Variant-kind codes consumed by the generated-program domain.
constexpr int64_t kBareUnitVariantKind = 0;
constexpr int64_t kEmptyTuplePayloadVariantKind = 2;

// Resolves the checked-in corpus manifest from Bazel runfiles.
std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

// Finds the checked sum type corresponding to one parsed sum declaration.
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
    for (const std::unique_ptr<dslx::Type>& param_type :
         function_type->params()) {
      auto* param_sum_type =
          dynamic_cast<const dslx::SumType*>(param_type.get());
      if (param_sum_type != nullptr &&
          param_sum_type->nominal_type().identifier() == sum_def.identifier()) {
        return param_sum_type;
      }
    }
  }
  return absl::NotFoundError(absl::StrCat(
      "Could not find a concrete sum type for '", sum_def.identifier(), "'."));
}

// Checks durable SumTypeProto declaration metadata for one DSLX program.
absl::Status VerifySumMetadata(std::string_view case_name,
                               std::string_view program_text) {
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(program_text, absl::StrCat(case_name, ".x"),
                              case_name, &import_data));
  XLS_ASSIGN_OR_RETURN(dslx::TypeInfoProto proto,
                       dslx::TypeInfoToProto(*tm.type_info, tm.module));

  std::vector<dslx::SumDef*> sum_defs = tm.module->GetSumDefs();
  if (sum_defs.empty()) {
    return absl::InvalidArgumentError(
        "Program did not contain a sum definition.");
  }

  for (dslx::SumDef* sum_def : sum_defs) {
    XLS_ASSIGN_OR_RETURN(const dslx::SumType* sum_type,
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
      return absl::FailedPreconditionError(absl::StrCat(
          "Missing SumTypeProto for '", sum_def->identifier(), "'."));
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
          "SumTypeProto concrete variant count did not match checked sum "
          "type.");
    }

    int64_t max_payload_bit_count = 0;
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
      XLS_ASSIGN_OR_RETURN(TypeDim payload_bit_count,
                           sum_type->variants().at(i).GetTotalBitCount());
      XLS_ASSIGN_OR_RETURN(int64_t payload_bit_count_value,
                           payload_bit_count.GetAsInt64());
      max_payload_bit_count =
          std::max(max_payload_bit_count, payload_bit_count_value);
    }

    const dslx::Phase1SumTypeEncoding encoding(*sum_type);
    XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                         encoding.payload_slot_bit_count());
    if (payload_slot_bit_count != max_payload_bit_count) {
      return absl::FailedPreconditionError(
          "Phase1SumTypeEncoding payload slot width did not match widest "
          "payload.");
    }

    XLS_RETURN_IF_ERROR(encoding.ForEachVariant(
        [&](const dslx::Phase1SumTypeEncoding::VariantInfo& variant)
            -> absl::Status {
          if (variant.variant_index < 0 ||
              variant.variant_index >= sum_type->variant_count()) {
            return absl::FailedPreconditionError(
                "Variant index out of bounds.");
          }
          XLS_ASSIGN_OR_RETURN(int64_t encoded_payload_bit_count,
                               variant.payload_bit_count());
          XLS_ASSIGN_OR_RETURN(TypeDim concrete_payload_bit_count,
                               variant.variant->GetTotalBitCount());
          XLS_ASSIGN_OR_RETURN(int64_t expected_payload_bit_count,
                               concrete_payload_bit_count.GetAsInt64());
          if (encoded_payload_bit_count != expected_payload_bit_count) {
            return absl::FailedPreconditionError(
                "Variant payload bit count did not match concrete payload.");
          }
          return absl::OkStatus();
        }));
  }

  return absl::OkStatus();
}

// Emits one bounded variant declaration from a generated kind code.
std::string GenerateVariantDecl(int64_t index, int64_t kind_choice) {
  switch (kind_choice) {
    case kBareUnitVariantKind:
      return absl::StrFormat("  V%d,\n", index);
    case 1:
      return absl::StrFormat("  V%d(u%d),\n", index, 8 * (index + 1));
    case kEmptyTuplePayloadVariantKind:
      return absl::StrFormat("  V%d(),\n", index);
    case 3:
      return absl::StrFormat("  V%d { x: u%d },\n", index, 8 * (index + 1));
    default:
      return absl::StrFormat("  V%d { },\n", index);
  }
}

// Emits a constructor matching the generated variant declaration.
std::string GenerateConstructorExpr(int64_t index, int64_t kind_choice) {
  switch (kind_choice) {
    case kBareUnitVariantKind:
      return absl::StrFormat("Generated::V%d", index);
    case 1:
      return absl::StrFormat("Generated::V%d(u%d:0)", index, 8 * (index + 1));
    case kEmptyTuplePayloadVariantKind:
      return absl::StrFormat("Generated::V%d()", index);
    case 3:
      return absl::StrFormat("Generated::V%d { x: u%d:0 }", index,
                             8 * (index + 1));
    default:
      return absl::StrFormat("Generated::V%d { }", index);
  }
}

// Builds one deterministic DSLX program from two to four kind choices.
std::string GenerateProgram(std::vector<int64_t> kind_choices) {
  if (std::all_of(kind_choices.begin(), kind_choices.end(),
                  [](int64_t kind_choice) {
                    return kind_choice == kBareUnitVariantKind;
                  })) {
    // Bare variants alone parse as a numeric enum. Preserve unit-only semantic
    // sum coverage by adding payload syntax only for otherwise-invalid draws.
    kind_choices.front() = kEmptyTuplePayloadVariantKind;
  }

  std::string program = "enum Generated {\n";
  for (int64_t i = 0; i < kind_choices.size(); ++i) {
    absl::StrAppend(&program, GenerateVariantDecl(i, kind_choices.at(i)));
  }
  absl::StrAppend(&program, "}\n\nfn main(x: bool) -> Generated {\n");
  absl::StrAppend(
      &program, "  if x { ", GenerateConstructorExpr(0, kind_choices.front()),
      " } else { ",
      GenerateConstructorExpr(kind_choices.size() - 1, kind_choices.back()),
      " }\n}\n");
  return program;
}

// Verifies: reviewed type-info seeds preserve durable sum metadata.
// Catches: missing, reordered, or misclassified serialized variants.
TEST(SemanticSumTypeinfoLayoutFuzzTest, ReplaysManifestCases) {
  int64_t verified = 0;
  XLS_ASSERT_OK(ReplaySemanticSumSeeds(
      GetManifestPath(), fuzzer::SEMANTIC_SUM_SEED_SURFACE_TYPEINFO_LAYOUT,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        XLS_RETURN_IF_ERROR(VerifySumMetadata(seed.seed_id(), seed_text));
        ++verified;
        return absl::OkStatus();
      }));
  EXPECT_EQ(verified, 2);
}

// Generates one bounded sum declaration and validates its metadata contract.
// It intentionally excludes layout-width assertions owned by later semantics.
void GeneratedProgramHasConsistentMetadata(std::vector<int64_t> kind_choices) {
  std::string program = GenerateProgram(std::move(kind_choices));
  SCOPED_TRACE(program);
  XLS_ASSERT_OK(VerifySumMetadata("generated_typeinfo_case", program));
}

FUZZ_TEST(SemanticSumTypeinfoLayoutFuzzTest,
          GeneratedProgramHasConsistentMetadata)
    .WithDomains(fuzztest::VectorOf(fuzztest::InRange<int64_t>(0, 4))
                     .WithMinSize(2)
                     .WithMaxSize(4));

}  // namespace
}  // namespace xls
