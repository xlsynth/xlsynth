// Copyright 2022 The XLS Authors
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
#include "xls/fuzzer/value_generator.h"

#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"

namespace xls {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::HasSubstr;
using ::testing::MatchesRegex;

dslx::SumType MakeTestSumType(dslx::Module& module) {
  const dslx::Span kFakeSpan = dslx::FakeSpan();

  auto* sum_name = module.Make<dslx::NameDef>(kFakeSpan, "Choice", nullptr);
  auto* none_name = module.Make<dslx::NameDef>(kFakeSpan, "None", nullptr);
  auto* byte_name = module.Make<dslx::NameDef>(kFakeSpan, "Byte", nullptr);
  auto* pair_name = module.Make<dslx::NameDef>(kFakeSpan, "Pair", nullptr);

  auto* u8_type = module.Make<dslx::BuiltinTypeAnnotation>(
      kFakeSpan, dslx::BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU8));
  auto* u16_type = module.Make<dslx::BuiltinTypeAnnotation>(
      kFakeSpan, dslx::BuiltinType::kU16,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU16));
  auto* u1_type = module.Make<dslx::BuiltinTypeAnnotation>(
      kFakeSpan, dslx::BuiltinType::kU1,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU1));

  auto* none_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, none_name, dslx::SumVariant::PayloadKind::kUnit,
      std::vector<dslx::TypeAnnotation*>{},
      std::vector<dslx::StructMemberNode*>{});
  auto* byte_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, byte_name, dslx::SumVariant::PayloadKind::kTuple,
      std::vector<dslx::TypeAnnotation*>{u8_type},
      std::vector<dslx::StructMemberNode*>{});
  auto* pair_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, pair_name, dslx::SumVariant::PayloadKind::kTuple,
      std::vector<dslx::TypeAnnotation*>{u16_type, u1_type},
      std::vector<dslx::StructMemberNode*>{});

  auto* sum_def = module.Make<dslx::SumDef>(
      kFakeSpan, sum_name, std::vector<dslx::ParametricBinding*>{},
      std::vector<dslx::SumVariant*>{none_variant, byte_variant, pair_variant},
      /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<dslx::SumTypeVariant> variants;
  variants.emplace_back(*none_variant, std::vector<std::unique_ptr<dslx::Type>>{});

  std::vector<std::unique_ptr<dslx::Type>> byte_members;
  byte_members.push_back(dslx::BitsType::MakeU8());
  variants.emplace_back(*byte_variant, std::move(byte_members));

  std::vector<std::unique_ptr<dslx::Type>> pair_members;
  pair_members.push_back(std::make_unique<dslx::BitsType>(false, 16));
  pair_members.push_back(dslx::BitsType::MakeU1());
  variants.emplace_back(*pair_variant, std::move(pair_members));
  return dslx::SumType(*sum_def, std::move(variants));
}

dslx::SumType MakeEmptySumType(dslx::Module& module) {
  const dslx::Span kFakeSpan = dslx::FakeSpan();

  auto* sum_name = module.Make<dslx::NameDef>(kFakeSpan, "Empty", nullptr);
  auto* sum_def = module.Make<dslx::SumDef>(
      kFakeSpan, sum_name, std::vector<dslx::ParametricBinding*>{},
      std::vector<dslx::SumVariant*>{}, /*is_public=*/false);
  sum_name->set_definer(sum_def);
  return dslx::SumType(*sum_def, std::vector<dslx::SumTypeVariant>{});
}

absl::StatusOr<dslx::TypeRefTypeAnnotation*> MakeTypeAnnotation(
    dslx::Module* module, std::string_view name) {
  XLS_ASSIGN_OR_RETURN(dslx::TypeDefinition type_definition,
                       module->GetTypeDefinition(name));
  auto* type_ref = module->Make<dslx::TypeRef>(dslx::FakeSpan(), type_definition);
  return module->Make<dslx::TypeRefTypeAnnotation>(
      dslx::FakeSpan(), type_ref, std::vector<dslx::ExprOrType>{});
}

void ExpectValueMatchesType(const dslx::Type& type,
                            const dslx::InterpValue& value);

void ExpectCanonicalSumValue(const dslx::SumType& sum_type,
                             const dslx::InterpValue& value) {
  ASSERT_TRUE(value.IsTuple());
  ASSERT_EQ(value.GetValuesOrDie().size(), 2);

  const dslx::InterpValue& tag = value.GetValuesOrDie().at(0);
  const dslx::InterpValue& payload_tuple = value.GetValuesOrDie().at(1);
  ASSERT_TRUE(tag.IsUBits());
  ASSERT_TRUE(payload_tuple.IsTuple());

  XLS_ASSERT_OK_AND_ASSIGN(uint64_t variant_index, tag.GetBitValueUnsigned());
  ASSERT_LT(variant_index, sum_type.variant_count());

  const std::vector<dslx::InterpValue>& payload_slots =
      payload_tuple.GetValuesOrDie();
  int64_t slot_index = 0;
  for (int64_t i = 0; i < sum_type.variant_count(); ++i) {
    const dslx::SumTypeVariant& variant = sum_type.variants().at(i);
    for (int64_t j = 0; j < variant.size(); ++j, ++slot_index) {
      const dslx::InterpValue& slot_value = payload_slots.at(slot_index);
      if (i == variant_index) {
        ExpectValueMatchesType(variant.GetMemberType(j), slot_value);
      } else {
        XLS_ASSERT_OK_AND_ASSIGN(dslx::InterpValue zero,
                                 dslx::CreateZeroValueFromType(
                                     variant.GetMemberType(j)));
        EXPECT_TRUE(slot_value.Eq(zero));
      }
    }
  }
  ASSERT_EQ(payload_slots.size(), slot_index);
}

void ExpectValueMatchesType(const dslx::Type& type,
                            const dslx::InterpValue& value) {
  if (std::optional<dslx::BitsLikeProperties> bits_like = dslx::GetBitsLike(type);
      bits_like.has_value()) {
    ASSERT_TRUE(value.IsBits());
    XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_count, bits_like->size.GetAsInt64());
    XLS_ASSERT_OK_AND_ASSIGN(bool is_signed, bits_like->is_signed.GetAsBool());
    EXPECT_THAT(value.GetBitCount(), IsOkAndHolds(bit_count));
    EXPECT_EQ(value.IsSigned(), is_signed);
    return;
  }

  if (auto* tuple_type = dynamic_cast<const dslx::TupleType*>(&type)) {
    ASSERT_TRUE(value.IsTuple());
    ASSERT_EQ(value.GetValuesOrDie().size(), tuple_type->size());
    for (int64_t i = 0; i < tuple_type->size(); ++i) {
      ExpectValueMatchesType(tuple_type->GetMemberType(i),
                             value.GetValuesOrDie().at(i));
    }
    return;
  }

  if (auto* struct_type = dynamic_cast<const dslx::StructType*>(&type)) {
    ASSERT_TRUE(value.IsTuple());
    ASSERT_EQ(value.GetValuesOrDie().size(), struct_type->size());
    for (int64_t i = 0; i < struct_type->size(); ++i) {
      ExpectValueMatchesType(struct_type->GetMemberType(i),
                             value.GetValuesOrDie().at(i));
    }
    return;
  }

  if (auto* array_type = dynamic_cast<const dslx::ArrayType*>(&type)) {
    ASSERT_TRUE(value.IsArray());
    XLS_ASSERT_OK_AND_ASSIGN(int64_t size, array_type->size().GetAsInt64());
    ASSERT_EQ(value.GetValuesOrDie().size(), size);
    for (const dslx::InterpValue& element : value.GetValuesOrDie()) {
      ExpectValueMatchesType(array_type->element_type(), element);
    }
    return;
  }

  if (auto* sum_type = dynamic_cast<const dslx::SumType*>(&type)) {
    ExpectCanonicalSumValue(*sum_type, value);
    return;
  }

  FAIL() << "Unsupported test type: " << type.ToString();
}

TEST(ValueGeneratorTest, GenerateEmptyValues) {
  std::mt19937_64 rng;
  std::vector<const dslx::Type*> param_type_ptrs;
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> values,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_TRUE(values.empty());
}

TEST(ValueGeneratorTest, GenerateSingleBitsArgument) {
  std::mt19937_64 rng;
  std::vector<std::unique_ptr<dslx::Type>> param_types;
  param_types.push_back(std::make_unique<dslx::BitsType>(
      /*signed=*/false,
      /*size=*/dslx::TypeDim::CreateU32(42)));

  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (auto& t : param_types) {
    param_type_ptrs.push_back(t.get());
  }
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> values,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(values.size(), 1);
  ASSERT_TRUE(values[0].IsUBits());
  EXPECT_THAT(values[0].GetBitCount(), IsOkAndHolds(42));
}

TEST(ValueGeneratorTest, GenerateMixedBitsArguments) {
  std::mt19937_64 rng;
  std::vector<std::unique_ptr<dslx::Type>> param_types;
  param_types.push_back(std::make_unique<dslx::BitsType>(
      /*signed=*/false,
      /*size=*/dslx::TypeDim::CreateU32(123)));
  param_types.push_back(std::make_unique<dslx::BitsType>(
      /*signed=*/true,
      /*size=*/dslx::TypeDim::CreateU32(22)));
  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (auto& t : param_types) {
    param_type_ptrs.push_back(t.get());
  }
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> arguments,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(arguments.size(), 2);
  ASSERT_TRUE(arguments[0].IsUBits());
  EXPECT_THAT(arguments[0].GetBitCount(), IsOkAndHolds(123));
  ASSERT_TRUE(arguments[1].IsSBits());
  EXPECT_THAT(arguments[1].GetBitCount(), IsOkAndHolds(22));
}

TEST(ValueGeneratorTest, GenerateTupleArgument) {
  std::mt19937_64 rng;
  std::vector<std::unique_ptr<dslx::Type>> param_types;
  std::vector<std::unique_ptr<dslx::Type>> tuple_members;
  tuple_members.push_back(
      std::make_unique<dslx::BitsType>(/*signed=*/false, /*size=*/123));
  tuple_members.push_back(
      std::make_unique<dslx::BitsType>(/*signed=*/true, /*size=*/22));
  param_types.push_back(
      std::make_unique<dslx::TupleType>(std::move(tuple_members)));

  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (auto& t : param_types) {
    param_type_ptrs.push_back(t.get());
  }
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> arguments,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(arguments.size(), 1);
  EXPECT_TRUE(arguments[0].IsTuple());
  EXPECT_THAT(arguments[0].GetValuesOrDie()[0].GetBitCount(),
              IsOkAndHolds(123));
  EXPECT_THAT(arguments[0].GetValuesOrDie()[1].GetBitCount(), IsOkAndHolds(22));
}

TEST(ValueGeneratorTest, GenerateArrayArgument) {
  std::mt19937_64 rng;
  std::vector<std::unique_ptr<dslx::Type>> param_types;
  param_types.push_back(std::make_unique<dslx::ArrayType>(
      std::make_unique<dslx::BitsType>(
          /*signed=*/true,
          /*size=*/dslx::TypeDim::CreateU32(4)),
      dslx::TypeDim::CreateU32(24)));

  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (auto& t : param_types) {
    param_type_ptrs.push_back(t.get());
  }
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> arguments,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(arguments.size(), 1);
  ASSERT_TRUE(arguments[0].IsArray());
  EXPECT_THAT(arguments[0].GetLength(), IsOkAndHolds(24));
  EXPECT_TRUE(arguments[0].GetValuesOrDie()[0].IsSBits());
  EXPECT_THAT(arguments[0].GetValuesOrDie()[0].GetBitCount(), IsOkAndHolds(4));
}

TEST(ValueGeneratorTest, GenerateSemanticSumArgument) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  dslx::SumType sum_type = MakeTestSumType(module);

  std::mt19937_64 rng{0};
  XLS_ASSERT_OK_AND_ASSIGN(dslx::InterpValue value,
                           GenerateInterpValue(rng, sum_type, {}));
  ExpectCanonicalSumValue(sum_type, value);
}

TEST(ValueGeneratorTest, GenerateInterpValuesWithSemanticSumTypes) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  dslx::SumType sum_type = MakeTestSumType(module);

  std::vector<std::unique_ptr<dslx::Type>> param_types;
  param_types.push_back(dslx::BitsType::MakeU32());
  param_types.push_back(sum_type.CloneToUnique());
  param_types.push_back(std::make_unique<dslx::ArrayType>(
      sum_type.CloneToUnique(), dslx::TypeDim::CreateU32(2)));

  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (const std::unique_ptr<dslx::Type>& type : param_types) {
    param_type_ptrs.push_back(type.get());
  }

  std::mt19937_64 rng{1};
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> values,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(values.size(), param_types.size());
  for (int64_t i = 0; i < values.size(); ++i) {
    ExpectValueMatchesType(*param_types.at(i), values.at(i));
  }
}

TEST(ValueGeneratorTest, GenerateEmptySemanticSumValueFails) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  dslx::SumType empty_sum_type = MakeEmptySumType(module);

  std::mt19937_64 rng{2};
  EXPECT_THAT(
      GenerateInterpValue(rng, empty_sum_type, {}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("empty sum type")));
}

TEST(ValueGeneratorTest, GenerateDslxConstantBits) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  std::mt19937_64 rng;
  dslx::BuiltinTypeAnnotation* type = module.Make<dslx::BuiltinTypeAnnotation>(
      dslx::FakeSpan(), dslx::BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * expr,
                           GenerateDslxConstant(rng, &module, type));
  ASSERT_NE(expr, nullptr);
  EXPECT_THAT(expr->ToString(), HasSubstr("u32:"));
}

TEST(ValueGeneratorTest, GenerateDslxConstantArrayOfBuiltinLessThan64) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  std::mt19937_64 rng;
  dslx::BuiltinTypeAnnotation* dim_type =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kU32,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  dslx::Number* dim = module.Make<dslx::Number>(
      dslx::FakeSpan(), "32", dslx::NumberKind::kOther, dim_type);
  dslx::BuiltinTypeAnnotation* element_type =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kSN,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kSN));
  dslx::ArrayTypeAnnotation* type = module.Make<dslx::ArrayTypeAnnotation>(
      dslx::FakeSpan(), element_type, dim);
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * expr,
                           GenerateDslxConstant(rng, &module, type));
  ASSERT_NE(expr, nullptr);
  EXPECT_THAT(expr->ToString(), HasSubstr("sN[u32:32]:"));
}

TEST(ValueGeneratorTest, GenerateDslxConstantArrayOfBuiltinGreaterThan64) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  std::mt19937_64 rng;
  dslx::BuiltinTypeAnnotation* dim_type =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kU32,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  dslx::Number* dim = module.Make<dslx::Number>(
      dslx::FakeSpan(), "65", dslx::NumberKind::kOther, dim_type);
  dslx::BuiltinTypeAnnotation* element_type =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kSN,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kSN));
  dslx::ArrayTypeAnnotation* type = module.Make<dslx::ArrayTypeAnnotation>(
      dslx::FakeSpan(), element_type, dim);
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * expr,
                           GenerateDslxConstant(rng, &module, type));
  ASSERT_NE(expr, nullptr);
  EXPECT_THAT(expr->ToString(), HasSubstr("sN[u32:65]:"));
}

TEST(ValueGeneratorTest, GenerateDslxConstantArrayOfBitsConstructor) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  std::mt19937_64 rng;

  // The type annotation for a bits constructor is:
  // `x: xN[false][7]`
  // i.e. a nested array type annotation.
  auto* element_type = module.Make<dslx::BuiltinTypeAnnotation>(
      dslx::FakeSpan(), dslx::BuiltinType::kXN,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kXN));

  // Wrap that in an array of dimension `false`.
  auto* expr_false = module.Make<dslx::Number>(
      dslx::FakeSpan(), "0", dslx::NumberKind::kOther, /*type=*/nullptr);
  auto* xn_false = module.Make<dslx::ArrayTypeAnnotation>(
      dslx::FakeSpan(), element_type, expr_false);

  // Wrap that in an array of dimension `7`.
  auto* expr_7 = module.Make<dslx::Number>(
      dslx::FakeSpan(), "7", dslx::NumberKind::kOther, /*type=*/nullptr);
  auto* xn_false_7 = module.Make<dslx::ArrayTypeAnnotation>(dslx::FakeSpan(),
                                                            xn_false, expr_7);
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * expr,
                           GenerateDslxConstant(rng, &module, xn_false_7));
  ASSERT_NE(expr, nullptr);
  EXPECT_THAT(expr->ToString(), HasSubstr("xN[0][7]:"));
}

TEST(ValueGeneratorTest, GenerateDslxConstantTuple) {
  dslx::FileTable file_table;
  dslx::Module module("test", /*fs_path=*/std::nullopt, file_table);
  std::mt19937_64 rng;
  dslx::BuiltinTypeAnnotation* element0 =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kU32,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  dslx::BuiltinTypeAnnotation* element1 =
      module.Make<dslx::BuiltinTypeAnnotation>(
          dslx::FakeSpan(), dslx::BuiltinType::kS32,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kS32));
  dslx::TupleTypeAnnotation* type = module.Make<dslx::TupleTypeAnnotation>(
      dslx::FakeSpan(), std::vector<dslx::TypeAnnotation*>{element0, element1});
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * expr,
                           GenerateDslxConstant(rng, &module, type));
  ASSERT_NE(expr, nullptr);
  constexpr const char* kWantPattern = R"(\(u32:[0-9]+, s32:[-0-9]+\))";
  EXPECT_THAT(expr->ToString(), MatchesRegex(kWantPattern));
}

TEST(ValueGeneratorTest, GenerateDslxConstantSemanticSums) {
  dslx::FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<dslx::Module> module,
      dslx::ParseModule(R"(
enum UnitOnly {
  Only,
}

enum TupleOnly {
  Only(u32),
}

enum StructOnly {
  Only { x: u32 },
}

enum Empty {}
)",
                        "test.x", "test", file_table));

  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeRefTypeAnnotation * unit_type,
                           MakeTypeAnnotation(module.get(), "UnitOnly"));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeRefTypeAnnotation * tuple_type,
                           MakeTypeAnnotation(module.get(), "TupleOnly"));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeRefTypeAnnotation * struct_type,
                           MakeTypeAnnotation(module.get(), "StructOnly"));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeRefTypeAnnotation * empty_type,
                           MakeTypeAnnotation(module.get(), "Empty"));

  std::mt19937_64 unit_rng{0};
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * unit_expr,
                           GenerateDslxConstant(unit_rng, module.get(),
                                                unit_type));
  EXPECT_NE(dynamic_cast<dslx::ColonRef*>(unit_expr), nullptr);
  EXPECT_EQ(unit_expr->ToString(), "UnitOnly::Only");

  std::mt19937_64 tuple_rng{1};
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * tuple_expr,
                           GenerateDslxConstant(tuple_rng, module.get(),
                                                tuple_type));
  EXPECT_NE(dynamic_cast<dslx::Invocation*>(tuple_expr), nullptr);
  EXPECT_THAT(tuple_expr->ToString(), HasSubstr("TupleOnly::Only("));
  EXPECT_THAT(tuple_expr->ToString(), HasSubstr("u32:"));

  std::mt19937_64 struct_rng{2};
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Expr * struct_expr,
                           GenerateDslxConstant(struct_rng, module.get(),
                                                struct_type));
  EXPECT_NE(dynamic_cast<dslx::StructInstance*>(struct_expr), nullptr);
  EXPECT_THAT(struct_expr->ToString(), HasSubstr("StructOnly::Only {"));
  EXPECT_THAT(struct_expr->ToString(), HasSubstr("x: u32:"));

  std::mt19937_64 empty_rng{3};
  EXPECT_THAT(GenerateDslxConstant(empty_rng, module.get(), empty_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("empty sum type")));
}

}  // namespace
}  // namespace xls
