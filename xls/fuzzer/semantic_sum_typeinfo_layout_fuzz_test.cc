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

// Covers serialized metadata for finite, acyclic sum declarations. The generated
// property owns its expected discriminants, concrete member types, parametric
// arguments, and nominal references independently of production type encoding.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/proto_test_utils.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.pb.h"
#include "xls/dslx/type_system/type_info_to_proto.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"

namespace xls {
namespace {

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

// Checks source-backed SumTypeProto payload metadata for one DSLX program.
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
      if (!candidate.has_sum_def_span()) {
        continue;
      }
      XLS_ASSIGN_OR_RETURN(
          const dslx::SumDef* candidate_sum_def,
          import_data.FindSumDef(dslx::FromProto(candidate.sum_def_span(),
                                                 import_data.file_table())));
      if (candidate_sum_def == sum_def) {
        sum_proto = &candidate;
        break;
      }
    }
    if (sum_proto == nullptr) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Missing SumTypeProto for '", sum_def->identifier(), "'."));
    }
    if (sum_proto->variants_size() != sum_def->variants().size()) {
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
      const dslx::SumTypeVariantProto& proto_variant = sum_proto->variants(i);
      if (proto_variant.payload_members_size() !=
          ast_variant->payload_member_count()) {
        return absl::FailedPreconditionError(
            "SumTypeProto payload count did not match its AST variant.");
      }
      if (proto_variant.payload_members_size() !=
          sum_type->variants().at(i).size()) {
        return absl::FailedPreconditionError(
            "SumTypeProto payload count did not match its checked variant.");
      }
      XLS_ASSIGN_OR_RETURN(dslx::TypeDim payload_bit_count,
                           sum_type->variants().at(i).GetTotalBitCount());
      XLS_ASSIGN_OR_RETURN(int64_t payload_bit_count_value,
                           payload_bit_count.GetAsInt64());
      max_payload_bit_count =
          std::max(max_payload_bit_count, payload_bit_count_value);
    }

    const dslx::SumTypeEncoding encoding(*sum_type);
    XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                         encoding.payload_slot_bit_count());
    if (payload_slot_bit_count != max_payload_bit_count) {
      return absl::FailedPreconditionError(
          "SumTypeEncoding payload slot width did not match widest "
          "payload.");
    }

    XLS_RETURN_IF_ERROR(encoding.ForEachVariant(
        [&](const dslx::SumTypeEncoding::VariantInfo& variant) -> absl::Status {
          if (variant.variant_index < 0 ||
              variant.variant_index >= sum_type->variant_count()) {
            return absl::FailedPreconditionError(
                "Variant index out of bounds.");
          }
          return absl::OkStatus();
        }));
  }

  return dslx::ToHumanString(proto, import_data, import_data.file_table())
      .status();
}

// Verifies: reviewed type-info seeds preserve source-backed sum metadata.
// Catches: missing variants and inconsistent concrete payload-member counts.
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

enum class PayloadKind {
  kUnit,
  kEmptyTuple,
  kEmptyStruct,
  kBits,
  kMembers,
  kStruct,
  kTuple,
  kArray,
  kNested,
};

enum class TagLayout { kImplicit, kUnsignedSparse, kSignedSparse };

int64_t DeclaredTag(TagLayout layout, int64_t index) {
  constexpr int64_t kUnsignedTags[] = {0, 2, 5, 7};
  constexpr int64_t kSignedTags[] = {-3, -1, 0, 2};
  switch (layout) {
    case TagLayout::kImplicit:
      return index;
    case TagLayout::kUnsignedSparse:
      return kUnsignedTags[index];
    case TagLayout::kSignedSparse:
      return kSignedTags[index];
  }
}

std::string GenerateProgram(const std::vector<PayloadKind>& kinds,
                            TagLayout layout, int64_t tag_width,
                            int64_t value_argument, int64_t type_width) {
  std::string program =
      "#![feature(generics)]\n"
      "enum Child { Leaf(u2), Other(s3) }\n"
      "enum Generated<N: u32, T: type>";
  if (layout != TagLayout::kImplicit) {
    absl::StrAppend(&program,
                    layout == TagLayout::kSignedSparse ? ": s" : ": u",
                    tag_width);
  }
  absl::StrAppend(&program, " {\n");
  for (int64_t i = 0; i < kinds.size(); ++i) {
    absl::StrAppend(&program, "V", i);
    switch (kinds[i]) {
      case PayloadKind::kUnit:
        break;
      case PayloadKind::kEmptyTuple:
        absl::StrAppend(&program, "()");
        break;
      case PayloadKind::kEmptyStruct:
        absl::StrAppend(&program, " {}");
        break;
      case PayloadKind::kBits:
        absl::StrAppend(&program, "(uN[N])");
        break;
      case PayloadKind::kMembers:
        absl::StrAppend(&program, "(T, uN[N])");
        break;
      case PayloadKind::kStruct:
        absl::StrAppend(&program, " { first: T, second: uN[N] }");
        break;
      case PayloadKind::kTuple:
        absl::StrAppend(&program, "((T, uN[N]))");
        break;
      case PayloadKind::kArray:
        absl::StrAppend(&program, "(T[2])");
        break;
      case PayloadKind::kNested:
        absl::StrAppend(&program, "(Child, Child)");
        break;
    }
    if (layout != TagLayout::kImplicit) {
      absl::StrAppend(&program, " = ", DeclaredTag(layout, i));
    }
    absl::StrAppend(&program, ",\n");
  }
  // Repeated identical instantiations must share; changing N must produce a
  // distinct nominal definition, including when N is phantom in every payload.
  absl::StrAppend(&program, "}\ntype Inputs = (Generated<u32:", value_argument,
                  ", s", type_width, ">, Generated<u32:", value_argument, ", s",
                  type_width, ">, Generated<u32:", value_argument + 1, ", s",
                  type_width, ">);\nfn main(x: Inputs) -> Inputs { x }\n");
  return program;
}

// The wire contract stores bits in big-endian bytes. This independent writer
// handles only the small literal widths used by this declaration domain.
dslx::InterpValueProto ExpectedBits(int64_t width, bool is_signed,
                                    uint64_t value) {
  dslx::InterpValueProto result;
  auto* bits = result.mutable_bits();
  bits->set_is_signed(is_signed);
  bits->set_bit_count(width);
  value &= (uint64_t{1} << width) - 1;
  std::string bytes((width + 7) / 8, '\0');
  for (int64_t i = bytes.size(); i > 0; --i) {
    bytes[i - 1] = static_cast<char>(value & 0xff);
    value >>= 8;
  }
  bits->set_data(bytes);
  return result;
}

dslx::TypeDimProto ExpectedDimension(int64_t value) {
  dslx::TypeDimProto result;
  *result.mutable_interp_value() = ExpectedBits(32, false, value);
  return result;
}

dslx::TypeProto ExpectedBitsType(int64_t width, bool is_signed) {
  dslx::TypeProto result;
  result.mutable_bits_type()->set_is_signed(is_signed);
  *result.mutable_bits_type()->mutable_dim() = ExpectedDimension(width);
  return result;
}

dslx::TypeProto ExpectedChild(const dslx::SpanProto& child_span,
                              uint64_t& next_id,
                              std::optional<uint64_t>& child_id) {
  dslx::TypeProto result;
  if (child_id.has_value()) {
    result.set_sum_type_reference(*child_id);
  } else {
    child_id = next_id++;
    auto* sum = result.mutable_sum_type();
    sum->set_definition_id(*child_id);
    *sum->mutable_sum_def_span() = child_span;
    *sum->mutable_tag_bit_count() = ExpectedDimension(1);
    for (int64_t i = 0; i < 2; ++i) {
      auto* variant = sum->add_variants();
      *variant->mutable_discriminant() = ExpectedBits(1, false, i);
      *variant->add_payload_members() =
          ExpectedBitsType(i == 0 ? 2 : 3, i == 1);
    }
  }
  return result;
}

dslx::TypeProto ExpectedGenerated(const std::vector<PayloadKind>& kinds,
                                  TagLayout layout, int64_t tag_width,
                                  int64_t value_argument, int64_t type_width,
                                  const dslx::SpanProto& generated_span,
                                  const dslx::SpanProto& child_span,
                                  uint64_t& next_id,
                                  std::optional<uint64_t>& child_id) {
  dslx::TypeProto result;
  auto* sum = result.mutable_sum_type();
  sum->set_definition_id(next_id++);
  *sum->mutable_sum_def_span() = generated_span;
  *sum->mutable_tag_bit_count() = ExpectedDimension(tag_width);
  *sum->add_parametric_arguments()->mutable_value() =
      ExpectedBits(32, false, value_argument);
  *sum->add_parametric_arguments()->mutable_type() =
      ExpectedBitsType(type_width, true);
  for (int64_t i = 0; i < kinds.size(); ++i) {
    auto* variant = sum->add_variants();
    *variant->mutable_discriminant() = ExpectedBits(
        tag_width, layout == TagLayout::kSignedSparse, DeclaredTag(layout, i));
    switch (kinds[i]) {
      case PayloadKind::kUnit:
      case PayloadKind::kEmptyTuple:
      case PayloadKind::kEmptyStruct:
        break;
      case PayloadKind::kBits:
        *variant->add_payload_members() =
            ExpectedBitsType(value_argument, false);
        break;
      case PayloadKind::kMembers:
      case PayloadKind::kStruct:
        *variant->add_payload_members() = ExpectedBitsType(type_width, true);
        *variant->add_payload_members() =
            ExpectedBitsType(value_argument, false);
        break;
      case PayloadKind::kTuple: {
        auto* tuple = variant->add_payload_members()->mutable_tuple_type();
        *tuple->add_members() = ExpectedBitsType(type_width, true);
        *tuple->add_members() = ExpectedBitsType(value_argument, false);
        break;
      }
      case PayloadKind::kArray: {
        auto* array = variant->add_payload_members()->mutable_array_type();
        *array->mutable_element_type() = ExpectedBitsType(type_width, true);
        *array->mutable_size() = ExpectedDimension(2);
        break;
      }
      case PayloadKind::kNested:
        *variant->add_payload_members() =
            ExpectedChild(child_span, next_id, child_id);
        *variant->add_payload_members() =
            ExpectedChild(child_span, next_id, child_id);
        break;
    }
  }
  return result;
}

void GeneratedProgramHasConsistentMetadata(std::vector<PayloadKind> kinds,
                                           TagLayout layout,
                                           uint8_t explicit_tag_width,
                                           uint8_t value_argument,
                                           uint8_t type_width) {
  if (std::all_of(kinds.begin(), kinds.end(), [](PayloadKind kind) {
        return kind == PayloadKind::kUnit;
      })) {
    // At least one payload spelling distinguishes a semantic sum from an enum.
    kinds.front() = PayloadKind::kEmptyTuple;
  }
  const int64_t tag_width = layout == TagLayout::kImplicit
                                ? (kinds.size() == 1   ? 0
                                   : kinds.size() == 2 ? 1
                                                       : 2)
                                : explicit_tag_width;
  const std::string program =
      GenerateProgram(kinds, layout, tag_width, value_argument, type_width);
  SCOPED_TRACE(program);
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "generated_metadata.x",
                              "generated_metadata", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeInfoProto proto,
                           dslx::TypeInfoToProto(*tm.type_info, tm.module));
  dslx::TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
  const dslx::AstNodeTypeInfoProto* parameter = nullptr;
  for (const auto& node : parsed.nodes()) {
    if (node.kind() == dslx::AST_NODE_KIND_PARAM) {
      parameter = &node;
    }
  }
  ASSERT_NE(parameter, nullptr);
  dslx::SumDef* generated = nullptr;
  dslx::SumDef* child = nullptr;
  for (dslx::SumDef* sum : tm.module->GetSumDefs()) {
    if (sum->identifier() == "Generated") {
      generated = sum;
    } else if (sum->identifier() == "Child") {
      child = sum;
    }
  }
  ASSERT_NE(generated, nullptr);
  ASSERT_NE(child, nullptr);
  const dslx::SpanProto generated_span =
      dslx::ToProto(generated->span(), import_data.file_table());
  const dslx::SpanProto child_span =
      dslx::ToProto(child->span(), import_data.file_table());
  uint64_t next_id = 1;
  std::optional<uint64_t> child_id;
  dslx::TypeProto expected;
  auto* tuple = expected.mutable_tuple_type();
  *tuple->add_members() =
      ExpectedGenerated(kinds, layout, tag_width, value_argument, type_width,
                        generated_span, child_span, next_id, child_id);
  tuple->add_members()->set_sum_type_reference(1);
  *tuple->add_members() = ExpectedGenerated(
      kinds, layout, tag_width, value_argument + 1, type_width, generated_span,
      child_span, next_id, child_id);
  EXPECT_THAT(parameter->type(), xls::proto_testing::EqualsProto(expected));
  // Human printing exercises another consumer, but supplies no oracle fields.
  XLS_ASSERT_OK(
      dslx::ToHumanString(*parameter, import_data, import_data.file_table())
          .status());
}

TEST(SemanticSumTypeinfoLayoutFuzzTest, GeneratedMetadataWitnesses) {
  for (TagLayout layout : {TagLayout::kImplicit, TagLayout::kUnsignedSparse,
                           TagLayout::kSignedSparse}) {
    GeneratedProgramHasConsistentMetadata({PayloadKind::kUnit}, layout, 3, 1,
                                          8);
    GeneratedProgramHasConsistentMetadata({PayloadKind::kEmptyStruct}, layout,
                                          4, 2, 7);
    GeneratedProgramHasConsistentMetadata(
        {PayloadKind::kBits, PayloadKind::kMembers, PayloadKind::kStruct,
         PayloadKind::kNested},
        layout, 5, 8, 1);
    GeneratedProgramHasConsistentMetadata(
        {PayloadKind::kEmptyTuple, PayloadKind::kTuple, PayloadKind::kArray},
        layout, 4, 3, 5);
  }
}

FUZZ_TEST(SemanticSumTypeinfoLayoutFuzzTest,
          GeneratedProgramHasConsistentMetadata)
    .WithDomains(
        fuzztest::VectorOf(fuzztest::ElementOf<PayloadKind>(
                               {PayloadKind::kUnit, PayloadKind::kEmptyTuple,
                                PayloadKind::kEmptyStruct, PayloadKind::kBits,
                                PayloadKind::kMembers, PayloadKind::kStruct,
                                PayloadKind::kTuple, PayloadKind::kArray,
                                PayloadKind::kNested}))
            .WithMinSize(1)
            .WithMaxSize(4),
        fuzztest::ElementOf<TagLayout>({TagLayout::kImplicit,
                                        TagLayout::kUnsignedSparse,
                                        TagLayout::kSignedSparse}),
        fuzztest::InRange<uint8_t>(3, 5), fuzztest::InRange<uint8_t>(1, 8),
        fuzztest::InRange<uint8_t>(1, 8));

}  // namespace
}  // namespace xls
