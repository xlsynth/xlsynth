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

// Checks value generation against an independent inhabitance oracle. Alongside
// the synthetic empty-enum probe, bounded declared nested sums vary payload
// widths and aggregate placement, including empty and nonempty arrays of an
// uninhabited type. Malformed raw values belong to the raw-boundary property.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/fuzzer/value_generator.h"
#include "xls/ir/value.h"

namespace xls {
namespace {

using ::absl_testing::IsOkAndHolds;

// Resolves the checked-in corpus manifest from Bazel runfiles.
std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

absl::StatusOr<bool> OracleTypeIsInhabited(const dslx::Type& type);

// Keep this oracle independent of production inhabitance analysis so a shared
// generation/type-analysis mistake cannot validate its own output.
absl::StatusOr<bool> OracleSumVariantIsInhabited(
    const dslx::SumTypeVariant& variant) {
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(bool member_is_inhabited,
                         OracleTypeIsInhabited(variant.GetMemberType(i)));
    if (!member_is_inhabited) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<bool> OracleTypeIsInhabited(const dslx::Type& type) {
  if (dslx::GetBitsLike(type).has_value()) {
    return true;
  } else if (const auto* sum = dynamic_cast<const dslx::SumType*>(&type)) {
    for (const dslx::SumTypeVariant& variant : sum->variants()) {
      XLS_ASSIGN_OR_RETURN(bool inhabited,
                           OracleSumVariantIsInhabited(variant));
      if (inhabited) {
        return true;
      }
    }
    return false;
  } else if (const auto* enumeration =
                 dynamic_cast<const dslx::EnumType*>(&type)) {
    return !enumeration->members().empty();
  } else if (const auto* array = dynamic_cast<const dslx::ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t size, array->size().GetAsInt64());
    if (size == 0) {
      return true;
    } else {
      return OracleTypeIsInhabited(array->element_type());
    }
  } else if (const auto* tuple = dynamic_cast<const dslx::TupleType*>(&type)) {
    for (const std::unique_ptr<dslx::Type>& member : tuple->members()) {
      XLS_ASSIGN_OR_RETURN(bool inhabited, OracleTypeIsInhabited(*member));
      if (!inhabited) {
        return false;
      }
    }
    return true;
  } else if (const auto* structure =
                 dynamic_cast<const dslx::StructTypeBase*>(&type)) {
    for (const std::unique_ptr<dslx::Type>& member : structure->members()) {
      XLS_ASSIGN_OR_RETURN(bool inhabited, OracleTypeIsInhabited(*member));
      if (!inhabited) {
        return false;
      }
    }
    return true;
  } else if (const auto* channel =
                 dynamic_cast<const dslx::ChannelType*>(&type)) {
    return OracleTypeIsInhabited(channel->payload_type());
  } else if (dynamic_cast<const dslx::TokenType*>(&type) != nullptr) {
    return true;
  } else {
    return absl::UnimplementedError(
        absl::StrCat("Inhabitance oracle does not support ", type.ToString()));
  }
}

absl::Status VerifyGeneratedValue(const dslx::Type& type,
                                  const dslx::InterpValue& value);

// Validates the encoded variant and recursively checks its active payload.
absl::Status VerifyGeneratedSumValue(const dslx::SumType& sum_type,
                                     const dslx::InterpValue& value) {
  const std::vector<dslx::InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != 2) {
    return absl::FailedPreconditionError(
        "Generated sum value was not encoded as a pair.");
  }
  std::optional<int64_t> variant_index;
  for (int64_t i = 0; i < sum_type.variant_count(); ++i) {
    if (elements.at(0).GetBitsOrDie() ==
        sum_type.GetDiscriminant(i).GetBitsOrDie()) {
      variant_index = i;
      break;
    }
  }
  if (!variant_index.has_value()) {
    return absl::FailedPreconditionError(
        "Generated sum selected an undeclared discriminant.");
  }
  const dslx::SumTypeVariant& variant = sum_type.variants().at(*variant_index);
  XLS_ASSIGN_OR_RETURN(bool variant_is_inhabited,
                       OracleSumVariantIsInhabited(variant));
  if (!variant_is_inhabited) {
    return absl::FailedPreconditionError(
        absl::StrCat("Generated uninhabited sum variant '",
                     variant.variant().identifier(), "'."));
  }
  XLS_ASSIGN_OR_RETURN(std::vector<dslx::InterpValue> payload_values,
                       dslx::GetSumPayloadValues(sum_type, value));
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        VerifyGeneratedValue(variant.GetMemberType(i), payload_values.at(i)));
  }
  return absl::OkStatus();
}

// Checks generated tuples, arrays, sums, and leaf raw round-trips.
absl::Status VerifyGeneratedValue(const dslx::Type& type,
                                  const dslx::InterpValue& value) {
  if (auto* tuple_type = dynamic_cast<const dslx::TupleType*>(&type)) {
    const std::vector<dslx::InterpValue>& members = value.GetValuesOrDie();
    for (int64_t i = 0; i < tuple_type->size(); ++i) {
      XLS_RETURN_IF_ERROR(
          VerifyGeneratedValue(tuple_type->GetMemberType(i), members.at(i)));
    }
    return absl::OkStatus();
  } else if (auto* array_type = dynamic_cast<const dslx::ArrayType*>(&type)) {
    for (const dslx::InterpValue& element : value.GetValuesOrDie()) {
      XLS_RETURN_IF_ERROR(
          VerifyGeneratedValue(array_type->element_type(), element));
    }
    return absl::OkStatus();
  } else if (auto* sum_type = dynamic_cast<const dslx::SumType*>(&type)) {
    return VerifyGeneratedSumValue(*sum_type, value);
  } else if (auto* structure =
                 dynamic_cast<const dslx::StructTypeBase*>(&type)) {
    const auto& members = value.GetValuesOrDie();
    for (int64_t i = 0; i < structure->members().size(); ++i) {
      XLS_RETURN_IF_ERROR(
          VerifyGeneratedValue(structure->GetMemberType(i), members.at(i)));
    }
    return absl::OkStatus();
  } else {
    XLS_ASSIGN_OR_RETURN(Value raw_value, value.ConvertToIr());
    XLS_ASSIGN_OR_RETURN(dslx::InterpValue roundtrip,
                         dslx::ValueToInterpValue(raw_value, &type));
    if (roundtrip != value) {
      return absl::FailedPreconditionError(
          absl::StrCat("Generated value did not roundtrip through raw form: '",
                       value.ToString(), "' vs '", roundtrip.ToString(), "'."));
    }
    return absl::OkStatus();
  }
}

// Builds an AST type reference for the synthetic empty enum payload.
absl::StatusOr<dslx::TypeRefTypeAnnotation*> MakeTypeAnnotation(
    dslx::Module* module, std::string_view name) {
  XLS_ASSIGN_OR_RETURN(dslx::TypeDefinition type_definition,
                       module->GetTypeDefinition(name));
  auto* type_ref =
      module->Make<dslx::TypeRef>(dslx::FakeSpan(), type_definition);
  return module->Make<dslx::TypeRefTypeAnnotation>(
      dslx::FakeSpan(), type_ref, std::vector<dslx::ExprOrType>{});
}

// Creates Unit | Impossible(Empty), the fixed partial-inhabitance probe.
absl::StatusOr<dslx::SumType> MakePartiallyInhabitedEnumPayloadSumType(
    dslx::Module& module) {
  const dslx::Span kFakeSpan = dslx::FakeSpan();

  auto* enum_name = module.Make<dslx::NameDef>(kFakeSpan, "Empty", nullptr);
  auto* u2_type = module.Make<dslx::BuiltinTypeAnnotation>(
      kFakeSpan, dslx::BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  auto* enum_def = module.Make<dslx::EnumDef>(kFakeSpan, enum_name, u2_type,
                                              std::vector<dslx::EnumMember>{},
                                              /*is_public=*/false);
  enum_name->set_definer(enum_def);
  XLS_RETURN_IF_ERROR(
      module.AddTop(enum_def, /*make_collision_error=*/nullptr));

  auto* sum_name =
      module.Make<dslx::NameDef>(kFakeSpan, "MaybeImpossible", nullptr);
  auto* unit_name = module.Make<dslx::NameDef>(kFakeSpan, "Unit", nullptr);
  auto* impossible_name =
      module.Make<dslx::NameDef>(kFakeSpan, "Impossible", nullptr);
  auto* unit_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, unit_name, dslx::SumVariant::PayloadShape::kUnit,
      std::vector<dslx::TypeAnnotation*>{},
      std::vector<dslx::StructMemberNode*>{});
  XLS_ASSIGN_OR_RETURN(dslx::TypeRefTypeAnnotation * enum_type_annotation,
                       MakeTypeAnnotation(&module, "Empty"));
  auto* impossible_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, impossible_name, dslx::SumVariant::PayloadShape::kTuple,
      std::vector<dslx::TypeAnnotation*>{enum_type_annotation},
      std::vector<dslx::StructMemberNode*>{});
  auto* sum_def = module.Make<dslx::SumDef>(
      kFakeSpan, sum_name, std::vector<dslx::ParametricBinding*>{},
      std::vector<dslx::SumVariant*>{unit_variant, impossible_variant},
      /*is_public=*/false);
  sum_name->set_definer(sum_def);
  XLS_RETURN_IF_ERROR(module.AddTop(sum_def, /*make_collision_error=*/nullptr));

  std::vector<dslx::SumTypeVariant> variants;
  variants.push_back(dslx::SumTypeVariant::MakeUnit(*unit_variant));
  std::vector<std::unique_ptr<dslx::Type>> payload_members;
  payload_members.push_back(std::make_unique<dslx::EnumType>(
      *enum_def, dslx::TypeDim::CreateU32(2), /*is_signed=*/false,
      std::vector<dslx::InterpValue>{}));
  variants.push_back(dslx::SumTypeVariant::MakeTuple(
      *impossible_variant, std::move(payload_members)));
  return dslx::SumType(*sum_def, std::move(variants));
}

TEST(SemanticSumInhabitanceFuzzTest, OracleDistinguishesEmptyPayloadDomains) {
  dslx::FileTable file_table;
  dslx::Module module("oracle_test", std::nullopt, file_table);
  XLS_ASSERT_OK_AND_ASSIGN(dslx::SumType sum,
                           MakePartiallyInhabitedEnumPayloadSumType(module));
  EXPECT_THAT(OracleSumVariantIsInhabited(sum.variants().at(0)),
              IsOkAndHolds(true));
  EXPECT_THAT(OracleSumVariantIsInhabited(sum.variants().at(1)),
              IsOkAndHolds(false));
  EXPECT_THAT(OracleTypeIsInhabited(sum), IsOkAndHolds(true));

  const dslx::Type& empty_enum = sum.variants().at(1).GetMemberType(0);
  EXPECT_THAT(OracleTypeIsInhabited(empty_enum), IsOkAndHolds(false));
  dslx::ArrayType zero_elements(empty_enum.CloneToUnique(),
                                dslx::TypeDim::CreateU32(0));
  dslx::ArrayType one_element(empty_enum.CloneToUnique(),
                              dslx::TypeDim::CreateU32(1));
  EXPECT_THAT(OracleTypeIsInhabited(zero_elements), IsOkAndHolds(true));
  EXPECT_THAT(OracleTypeIsInhabited(one_element), IsOkAndHolds(false));
  std::vector<std::unique_ptr<dslx::Type>> members;
  members.push_back(sum.CloneToUnique());
  members.push_back(one_element.CloneToUnique());
  dslx::TupleType tuple(std::move(members));
  EXPECT_THAT(OracleTypeIsInhabited(tuple), IsOkAndHolds(false));
}

// Selects main, or the only function, from a reviewed source fixture.
absl::StatusOr<dslx::Function*> GetEntryFunction(dslx::Module& module) {
  auto functions = module.GetFunctionByName();
  auto it = functions.find("main");
  if (it != functions.end()) {
    return it->second;
  }
  if (functions.size() == 1) {
    return functions.begin()->second;
  }
  return absl::NotFoundError(
      absl::StrCat("Expected a 'main' function or exactly one top-level "
                   "function in module '",
                   module.name(), "'"));
}

// Nominal types borrow their declaration AST even after CloneToUnique. Keep the
// import owner alive through generation and recursive inspection of those
// types.
struct SourceInhabitanceContext {
  std::unique_ptr<dslx::ImportData> import_data;
  const dslx::FunctionType* function_type;
};

absl::StatusOr<SourceInhabitanceContext> PrepareSourceContext(
    const std::string& seed_text, std::string_view seed_id) {
  auto import_data =
      std::make_unique<dslx::ImportData>(dslx::CreateImportDataForTest());
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(seed_text, absl::StrCat(seed_id, ".x"), seed_id,
                              import_data.get()));
  XLS_ASSIGN_OR_RETURN(dslx::Function * function, GetEntryFunction(*tm.module));
  XLS_ASSIGN_OR_RETURN(dslx::FunctionType * function_type,
                       tm.type_info->GetItemAs<dslx::FunctionType>(function));
  return SourceInhabitanceContext{.import_data = std::move(import_data),
                                  .function_type = function_type};
}

// Verifies: reviewed inhabitance seeds generate only valid values.
// Catches: generator regressions that select an uninhabited variant.
TEST(SemanticSumInhabitanceFuzzTest, ReplaysManifestCases) {
  std::mt19937_64 generator(0);
  absl::BitGenRef bit_gen(generator);
  int64_t verified = 0;
  XLS_ASSERT_OK(ReplaySemanticSumSeeds(
      GetManifestPath(), fuzzer::SEMANTIC_SUM_SEED_SURFACE_INHABITANCE,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        if (seed.outcome() != fuzzer::SEMANTIC_SUM_SEED_OUTCOME_SHOULD_PASS) {
          return absl::OkStatus();
        }
        XLS_ASSIGN_OR_RETURN(SourceInhabitanceContext context,
                             PrepareSourceContext(seed_text, seed.seed_id()));
        const auto& params = context.function_type->params();
        std::vector<const dslx::Type*> param_ptrs;
        param_ptrs.reserve(params.size());
        for (const std::unique_ptr<dslx::Type>& param : params) {
          param_ptrs.push_back(param.get());
        }
        for (int64_t i = 0; i < 4; ++i) {
          XLS_ASSIGN_OR_RETURN(std::vector<dslx::InterpValue> values,
                               GenerateInterpValues(bit_gen, param_ptrs));
          for (int64_t j = 0; j < values.size(); ++j) {
            XLS_RETURN_IF_ERROR(
                VerifyGeneratedValue(*params.at(j), values.at(j)));
            ++verified;
          }
        }
        return absl::OkStatus();
      }));
  EXPECT_GT(verified, 0);
}

// Generates one value for one of three fixed shapes from an explicit seed.
// It validates inhabitance recursively but does not fuzz declaration syntax.
void GeneratedValueIsInhabited(uint64_t generator_seed,
                               uint8_t shape_selector) {
  std::mt19937_64 generator(generator_seed);
  absl::BitGenRef bit_gen(generator);
  dslx::FileTable file_table;
  dslx::Module module("semantic_sum_inhabitance_fuzz",
                      /*fs_path=*/std::nullopt, file_table);
  XLS_ASSERT_OK_AND_ASSIGN(dslx::SumType partial_sum,
                           MakePartiallyInhabitedEnumPayloadSumType(module));
  std::vector<std::unique_ptr<dslx::Type>> tuple_members;
  tuple_members.push_back(partial_sum.CloneToUnique());
  tuple_members.push_back(dslx::BitsType::MakeU8());
  auto nested_tuple =
      std::make_unique<dslx::TupleType>(std::move(tuple_members));
  auto nested_array = std::make_unique<dslx::ArrayType>(
      partial_sum.CloneToUnique(), dslx::TypeDim::CreateU32(2));

  const dslx::Type* selected_type = nullptr;
  if (shape_selector == 0) {
    selected_type = &partial_sum;
  } else if (shape_selector == 1) {
    selected_type = nested_tuple.get();
  } else {
    selected_type = nested_array.get();
  }
  XLS_ASSERT_OK_AND_ASSIGN(
      dslx::InterpValue value,
      GenerateInterpValue(bit_gen, *selected_type,
                          absl::Span<const dslx::InterpValue>()));
  XLS_ASSERT_OK(VerifyGeneratedValue(*selected_type, value));
}

FUZZ_TEST(SemanticSumInhabitanceFuzzTest, GeneratedValueIsInhabited)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(),
                 fuzztest::InRange<uint8_t>(0, 2));

std::string DeclaredNestedProgram(uint8_t payload_width,
                                  uint8_t shape_selector) {
  std::string program = absl::StrCat(
      "enum Empty: u2 {}\n", "struct Blocked { empty: Empty }\n",
      "struct Live { flag: u1, value: u", payload_width, " }\n",
      "enum Leaf: u3 { Vacant(Empty[0]) = 0, Value(u", payload_width,
      ") = 2, Impossible(Empty[1]) = 5 }\n",
      "enum Outer: u3 { Unit = 0, Nested(Leaf[2], (Leaf, u2)) = 2,\n",
      "Record { payload: Live } = 5, Impossible(Blocked) = 7 }\n",
      "struct Container { value: Outer, pair: (Outer, u1), items: Outer[2] "
      "}\n");
  std::string parameter_type;
  if (shape_selector == 0) {
    parameter_type = "Outer";
  } else if (shape_selector == 1) {
    parameter_type = "(Outer, u8)";
  } else if (shape_selector == 2) {
    parameter_type = "Outer[2]";
  } else {
    parameter_type = "Container";
  }
  absl::StrAppend(&program, "fn main(x: ", parameter_type, ", _leaf: Leaf) -> ",
                  parameter_type, " { x }\n");
  return program;
}

void DeclaredNestedValuesAreInhabited(uint64_t generator_seed,
                                      uint8_t payload_width,
                                      uint8_t shape_selector) {
  const std::string program =
      DeclaredNestedProgram(payload_width, shape_selector);
  SCOPED_TRACE(program);
  SCOPED_TRACE(generator_seed);
  XLS_ASSERT_OK_AND_ASSIGN(
      SourceInhabitanceContext context,
      PrepareSourceContext(program, "declared_inhabitance"));
  const auto& params = context.function_type->params();
  const auto* leaf = dynamic_cast<const dslx::SumType*>(params.at(1).get());
  ASSERT_NE(leaf, nullptr);
  ASSERT_EQ(leaf->variant_count(), 3);
  // The zero-length array has one inhabitant even though Empty does not. Pin
  // all variants so mistakenly filtering out Vacant cannot hide in generation.
  for (int64_t i = 0; i < leaf->variant_count(); ++i) {
    XLS_ASSERT_OK_AND_ASSIGN(
        bool oracle_inhabited,
        OracleSumVariantIsInhabited(leaf->variants().at(i)));
    EXPECT_EQ(oracle_inhabited, i != 2);
    EXPECT_THAT(dslx::SumVariantIsInhabited(leaf->variants().at(i)),
                IsOkAndHolds(oracle_inhabited));
  }
  std::vector<const dslx::Type*> param_ptrs;
  for (const auto& param : params) {
    XLS_ASSERT_OK_AND_ASSIGN(bool oracle_inhabited,
                             OracleTypeIsInhabited(*param));
    EXPECT_TRUE(oracle_inhabited);
    EXPECT_THAT(dslx::TypeIsInhabited(*param), IsOkAndHolds(oracle_inhabited));
    param_ptrs.push_back(param.get());
  }
  std::mt19937_64 generator(generator_seed);
  absl::BitGenRef bit_gen(generator);
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> values,
                           GenerateInterpValues(bit_gen, param_ptrs));
  ASSERT_EQ(values.size(), params.size());
  for (int64_t i = 0; i < values.size(); ++i) {
    XLS_ASSERT_OK(VerifyGeneratedValue(*params.at(i), values.at(i)));
  }
}

TEST(SemanticSumInhabitanceFuzzTest, DeclaredNestedInhabitanceWitnesses) {
  for (uint8_t width : {uint8_t{1}, uint8_t{8}}) {
    for (uint8_t shape = 0; shape < 4; ++shape) {
      for (uint64_t seed : {uint64_t{0}, uint64_t{2026031503}}) {
        DeclaredNestedValuesAreInhabited(seed, width, shape);
      }
    }
  }
}

FUZZ_TEST(SemanticSumInhabitanceFuzzTest, DeclaredNestedValuesAreInhabited)
    .WithDomains(fuzztest::Arbitrary<uint64_t>(),
                 fuzztest::InRange<uint8_t>(1, 8),
                 fuzztest::InRange<uint8_t>(0, 3));

}  // namespace
}  // namespace xls
