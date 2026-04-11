// Copyright 2021 The XLS Authors
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
#include "xls/dslx/interp_value_internal_utils.h"
#include "xls/dslx/interp_value_utils.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"
#include "xls/ir/value.h"

namespace xls::dslx {
namespace {
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::Eq;
using ::testing::HasSubstr;

SumType MakeMixedPayloadSumType(Module& module) {
  const Span kFakeSpan = Span::Fake();

  auto* sum_name = module.Make<NameDef>(kFakeSpan, "Example", nullptr);
  auto* none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* byte_name = module.Make<NameDef>(kFakeSpan, "Byte", nullptr);
  auto* wide_name = module.Make<NameDef>(kFakeSpan, "Wide", nullptr);

  auto* u8_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU8));
  auto* u16_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU16,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU16));

  auto* none =
      module.Make<SumVariant>(kFakeSpan, none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* byte =
      module.Make<SumVariant>(kFakeSpan, byte_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u8_type},
                              std::vector<StructMemberNode*>{});
  auto* wide =
      module.Make<SumVariant>(kFakeSpan, wide_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u16_type},
                              std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, byte, wide}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  std::vector<std::unique_ptr<Type>> byte_members;
  byte_members.push_back(BitsType::MakeU8());
  variants.push_back(SumTypeVariant::MakeTuple(*byte, std::move(byte_members)));
  std::vector<std::unique_ptr<Type>> wide_members;
  wide_members.push_back(std::make_unique<BitsType>(/*is_signed=*/false, 16));
  variants.push_back(SumTypeVariant::MakeTuple(*wide, std::move(wide_members)));
  return SumType(*sum_def, std::move(variants));
}

SumType MakeOuterSumWithInactiveEmptyPayloadType(Module& module) {
  const Span kFakeSpan = Span::Fake();

  auto* empty_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  auto* empty_def = module.Make<SumDef>(
      kFakeSpan, empty_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{}, /*is_public=*/false);
  empty_name->set_definer(empty_def);
  SumType empty_type(*empty_def, std::vector<SumTypeVariant>{});

  auto* outer_name = module.Make<NameDef>(kFakeSpan, "Outer", nullptr);
  auto* wrapped_name = module.Make<NameDef>(kFakeSpan, "Wrapped", nullptr);
  auto* nothing_name = module.Make<NameDef>(kFakeSpan, "Nothing", nullptr);
  auto* wrapped =
      module.Make<SumVariant>(kFakeSpan, wrapped_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{
                                  module.Make<TypeRefTypeAnnotation>(
                                      kFakeSpan,
                                      module.Make<TypeRef>(kFakeSpan, empty_def),
                                      std::vector<ExprOrType>{})},
                              std::vector<StructMemberNode*>{});
  auto* nothing =
      module.Make<SumVariant>(kFakeSpan, nothing_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* outer_def = module.Make<SumDef>(
      kFakeSpan, outer_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{wrapped, nothing}, /*is_public=*/false);
  outer_name->set_definer(outer_def);

  std::vector<SumTypeVariant> outer_variants;
  std::vector<std::unique_ptr<Type>> wrapped_members;
  wrapped_members.push_back(empty_type.CloneToUnique());
  outer_variants.push_back(
      SumTypeVariant::MakeTuple(*wrapped, std::move(wrapped_members)));
  outer_variants.push_back(SumTypeVariant::MakeUnit(*nothing));
  return SumType(*outer_def, std::move(outer_variants));
}

SumType MakeOuterSumWithInactiveEmptyEnumPayloadType(Module& module) {
  const Span kFakeSpan = Span::Fake();

  auto* enum_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  TypeAnnotation* enum_element_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  EnumDef* enum_def = module.Make<EnumDef>(
      kFakeSpan, enum_name, enum_element_type, std::vector<EnumMember>{},
      /*is_public=*/false);
  enum_name->set_definer(enum_def);
  EnumType enum_type(*enum_def, TypeDim::CreateU32(2), /*is_signed=*/false, {});

  auto* outer_name = module.Make<NameDef>(kFakeSpan, "MaybeImpossible", nullptr);
  auto* unit_name = module.Make<NameDef>(kFakeSpan, "Unit", nullptr);
  auto* impossible_name = module.Make<NameDef>(kFakeSpan, "Impossible", nullptr);
  auto* unit =
      module.Make<SumVariant>(kFakeSpan, unit_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* impossible =
      module.Make<SumVariant>(kFakeSpan, impossible_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{
                                  module.Make<TypeRefTypeAnnotation>(
                                      kFakeSpan,
                                      module.Make<TypeRef>(kFakeSpan, enum_def),
                                      std::vector<ExprOrType>{})},
                              std::vector<StructMemberNode*>{});
  auto* outer_def = module.Make<SumDef>(
      kFakeSpan, outer_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{unit, impossible}, /*is_public=*/false);
  outer_name->set_definer(outer_def);

  std::vector<SumTypeVariant> outer_variants;
  outer_variants.push_back(SumTypeVariant::MakeUnit(*unit));
  std::vector<std::unique_ptr<Type>> impossible_members;
  impossible_members.push_back(enum_type.CloneToUnique());
  outer_variants.push_back(
      SumTypeVariant::MakeTuple(*impossible, std::move(impossible_members)));
  return SumType(*outer_def, std::move(outer_variants));
}

SumType MakeEnumPayloadSumType(Module& module, EnumDef** enum_def_out) {
  const Span kFakeSpan = Span::Fake();

  auto* enum_name = module.Make<NameDef>(kFakeSpan, "Flavor", nullptr);
  TypeAnnotation* enum_element_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  auto* vanilla_name = module.Make<NameDef>(kFakeSpan, "Vanilla", nullptr);
  auto* vanilla_value =
      module.Make<Number>(kFakeSpan, "0", NumberKind::kOther, enum_element_type);
  vanilla_name->set_definer(vanilla_value);
  auto* mint_name = module.Make<NameDef>(kFakeSpan, "Mint", nullptr);
  auto* mint_value =
      module.Make<Number>(kFakeSpan, "1", NumberKind::kOther, enum_element_type);
  mint_name->set_definer(mint_value);
  auto* enum_def = module.Make<EnumDef>(
      kFakeSpan, enum_name, enum_element_type,
      std::vector<EnumMember>{
          EnumMember{.name_def = vanilla_name, .value = vanilla_value},
          EnumMember{.name_def = mint_name, .value = mint_value},
      },
      /*is_public=*/false);
  enum_name->set_definer(enum_def);
  EnumType enum_type(
      *enum_def, TypeDim::CreateU32(2), /*is_signed=*/false,
      std::vector<InterpValue>{
          InterpValue::MakeEnum(UBits(0, 2), /*is_signed=*/false, enum_def),
          InterpValue::MakeEnum(UBits(1, 2), /*is_signed=*/false, enum_def),
      });

  auto* sum_name = module.Make<NameDef>(kFakeSpan, "Choice", nullptr);
  auto* some_name = module.Make<NameDef>(kFakeSpan, "Some", nullptr);
  auto* none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* some =
      module.Make<SumVariant>(kFakeSpan, some_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{
                                  module.Make<TypeRefTypeAnnotation>(
                                      kFakeSpan,
                                      module.Make<TypeRef>(kFakeSpan, enum_def),
                                      std::vector<ExprOrType>{})},
                              std::vector<StructMemberNode*>{});
  auto* none =
      module.Make<SumVariant>(kFakeSpan, none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{some, none}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  std::vector<std::unique_ptr<Type>> some_members;
  some_members.push_back(enum_type.CloneToUnique());
  variants.push_back(SumTypeVariant::MakeTuple(*some, std::move(some_members)));
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  *enum_def_out = enum_def;
  return SumType(*sum_def, std::move(variants));
}

TEST(InterpValueHelpersTest, CastBitsToArray) {
  InterpValue input(InterpValue::MakeU32(0xa5a5a5a5));

  ArrayType array_type(BitsType::MakeU8(), TypeDim::CreateU32(4));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue converted,
                           CastBitsToArray(input, array_type));
  ASSERT_TRUE(converted.IsArray());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t length, converted.GetLength());
  ASSERT_EQ(length, 4);
  for (int i = 0; i < 4; i++) {
    XLS_ASSERT_OK_AND_ASSIGN(InterpValue value, converted.Index(i));
    ASSERT_TRUE(value.IsBits());
    XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitValueViaSign());
    ASSERT_EQ(int_value, 0xa5);
  }
}

TEST(InterpValueHelpersTest, CastBitsToEnumAndCreatZeroValue) {
  constexpr int kBitCount = 13;
  constexpr int kNumMembers = 16;
  FileTable file_table;
  Module module("my_test_module", /*fs_path=*/std::nullopt, file_table);

  std::vector<EnumMember> members;
  std::vector<InterpValue> member_values;
  BuiltinNameDef* builtin_name_def =
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU13);
  TypeAnnotation* element_type = module.Make<BuiltinTypeAnnotation>(
      Span::Fake(), BuiltinType::kU13, builtin_name_def);
  for (int i = 0; i < kNumMembers; i++) {
    NameDef* name_def =
        module.Make<NameDef>(Span::Fake(), absl::StrCat("member_", i), nullptr);
    Number* number = module.Make<Number>(Span::Fake(), absl::StrCat(i),
                                         NumberKind::kOther, element_type);
    name_def->set_definer(number);
    members.push_back(EnumMember{.name_def = name_def, .value = number});
    member_values.push_back(InterpValue::MakeUBits(kBitCount, i));
  }

  NameDef* name_def =
      module.Make<NameDef>(Span::Fake(), "my_test_enum", nullptr);
  EnumDef* enum_def = module.Make<EnumDef>(Span::Fake(), name_def, element_type,
                                           members, /*is_public=*/true);

  EnumType enum_type(*enum_def, TypeDim::CreateU32(kBitCount),
                     /*is_signed=*/false, member_values);

  InterpValue bits_value(InterpValue::MakeUBits(kBitCount, 11));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue converted,
                           CastBitsToEnum(bits_value, enum_type));
  ASSERT_TRUE(converted.IsEnum());
  InterpValue::EnumData enum_data = converted.GetEnumData().value();
  ASSERT_EQ(enum_data.def, enum_def);
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_value, enum_data.value.ToUint64());
  ASSERT_EQ(int_value, 11);

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue enum_zero,
                           CreateZeroValueFromType(enum_type));
  EXPECT_TRUE(
      InterpValue::MakeEnum(Bits(kBitCount), /*is_signed=*/false, enum_def)
          .Eq(enum_zero));
}

TEST(InterpValueHelpersTest, CreateZeroBitsAndArrayValues) {
  // Create zero bits.
  std::unique_ptr<BitsType> u8 = BitsType::MakeU8();
  std::unique_ptr<BitsType> s32 = BitsType::MakeS32();

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue u8_zero, CreateZeroValueFromType(*u8));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue s32_zero, CreateZeroValueFromType(*s32));

  EXPECT_TRUE(InterpValue::MakeUBits(/*bit_count=*/8, 0).Eq(u8_zero));
  EXPECT_FALSE(u8_zero.IsSigned());

  EXPECT_TRUE(InterpValue::MakeSBits(/*bit_count=*/32, 0).Eq(s32_zero));
  EXPECT_TRUE(s32_zero.IsSigned());

  // Create a zero tuple.
  std::vector<std::unique_ptr<Type>> tuple_members;
  tuple_members.push_back(u8->CloneToUnique());
  tuple_members.push_back(s32->CloneToUnique());
  TupleType tuple(std::move(tuple_members));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue tuple_zero,
                           CreateZeroValueFromType(tuple));
  EXPECT_TRUE(InterpValue::MakeTuple({u8_zero, s32_zero}).Eq(tuple_zero));

  // Create a zero array of tuples.
  ArrayType array_type(tuple.CloneToUnique(), TypeDim::CreateU32(2));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue array_zero,
                           CreateZeroValueFromType(array_type));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue array_zero_golden,
                           InterpValue::MakeArray({tuple_zero, tuple_zero}));
  EXPECT_TRUE(array_zero_golden.Eq(array_zero));
}

TEST(InterpValueHelpersTest, CreateZeroStructValue) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  std::vector<StructMemberNode*> ast_members;
  ast_members.emplace_back(module.Make<StructMemberNode>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "x", nullptr), kFakeSpan,
      module.Make<BuiltinTypeAnnotation>(
          kFakeSpan, BuiltinType::kU8,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU8))));
  ast_members.emplace_back(module.Make<StructMemberNode>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "y", nullptr), kFakeSpan,
      module.Make<BuiltinTypeAnnotation>(
          kFakeSpan, BuiltinType::kU1,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU1))));

  auto* struct_def = module.Make<StructDef>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "S", nullptr),
      std::vector<ParametricBinding*>{}, ast_members, /*is_public=*/false);
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  members.push_back(BitsType::MakeU1());
  StructType s(std::move(members), *struct_def);

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue struct_zero, CreateZeroValueFromType(s));

  InterpValue u8_zero = InterpValue::MakeUBits(/*bit_count=*/8, 0);
  InterpValue u1_zero = InterpValue::MakeUBits(/*bit_count=*/1, 0);

  EXPECT_TRUE(InterpValue::MakeTuple({u8_zero, u1_zero}).Eq(struct_zero));
}

TEST(InterpValueHelpersTest, CreateZeroSumValueUsesFirstVariantRecursively) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* inner_name = module.Make<NameDef>(kFakeSpan, "Inner", nullptr);
  auto* inner_none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* inner_some_name = module.Make<NameDef>(kFakeSpan, "Some", nullptr);
  auto* u32_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  auto* inner_none =
      module.Make<SumVariant>(kFakeSpan, inner_none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* inner_some =
      module.Make<SumVariant>(kFakeSpan, inner_some_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u32_type},
                              std::vector<StructMemberNode*>{});
  auto* inner_def = module.Make<SumDef>(
      kFakeSpan, inner_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{inner_none, inner_some}, /*is_public=*/false);
  inner_name->set_definer(inner_def);

  std::vector<SumTypeVariant> inner_variants;
  inner_variants.push_back(SumTypeVariant::MakeUnit(*inner_none));
  std::vector<std::unique_ptr<Type>> inner_some_members;
  inner_some_members.push_back(BitsType::MakeU32());
  inner_variants.push_back(
      SumTypeVariant::MakeTuple(*inner_some, std::move(inner_some_members)));
  SumType inner_type(*inner_def, std::move(inner_variants));

  auto* outer_name = module.Make<NameDef>(kFakeSpan, "Outer", nullptr);
  auto* outer_wrap_name = module.Make<NameDef>(kFakeSpan, "Wrap", nullptr);
  auto* outer_none_name = module.Make<NameDef>(kFakeSpan, "Nothing", nullptr);
  auto* outer_wrap =
      module.Make<SumVariant>(kFakeSpan, outer_wrap_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u32_type},
                              std::vector<StructMemberNode*>{});
  auto* outer_none =
      module.Make<SumVariant>(kFakeSpan, outer_none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* outer_def = module.Make<SumDef>(
      kFakeSpan, outer_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{outer_wrap, outer_none}, /*is_public=*/false);
  outer_name->set_definer(outer_def);

  std::vector<SumTypeVariant> outer_variants;
  std::vector<std::unique_ptr<Type>> outer_wrap_members;
  outer_wrap_members.push_back(inner_type.CloneToUnique());
  outer_variants.push_back(
      SumTypeVariant::MakeTuple(*outer_wrap, std::move(outer_wrap_members)));
  outer_variants.push_back(SumTypeVariant::MakeUnit(*outer_none));
  SumType outer_type(*outer_def, std::move(outer_variants));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue zero, CreateZeroValueFromType(outer_type));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(1, 0),
                   InterpValue::MakeTuple(
                       {InterpValue::MakeTuple({InterpValue::MakeUBits(1, 0),
                                               InterpValue::MakeTuple(
                                                   {InterpValue::MakeU32(0)})})})})
                  .Eq(zero));
}

TEST(InterpValueHelpersTest, CreateZeroEmptySumValueFails) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* empty_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  auto* empty_def = module.Make<SumDef>(
      kFakeSpan, empty_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{}, /*is_public=*/false);
  empty_name->set_definer(empty_def);
  SumType empty_type(*empty_def, std::vector<SumTypeVariant>{});

  EXPECT_THAT(CreateZeroValueFromType(empty_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uninhabited sum type `Empty`")));
}

TEST(InterpValueHelpersTest, CreateInternalPlaceholderEmptySumValueUsesZeroTag) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* empty_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  auto* empty_def = module.Make<SumDef>(
      kFakeSpan, empty_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{}, /*is_public=*/false);
  empty_name->set_definer(empty_def);
  SumType empty_type(*empty_def, std::vector<SumTypeVariant>{});

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue zero,
                           internal::CreateInternalPlaceholderValueFromType(
                               empty_type));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(1, 0), InterpValue::MakeTuple({})})
                  .Eq(zero));
}

TEST(InterpValueHelpersTest,
     CreateInternalPlaceholderEmptyEnumValueUsesZeroBits) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* enum_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  TypeAnnotation* enum_element_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  EnumDef* enum_def = module.Make<EnumDef>(
      kFakeSpan, enum_name, enum_element_type, std::vector<EnumMember>{},
      /*is_public=*/false);
  enum_name->set_definer(enum_def);
  EnumType enum_type(*enum_def, TypeDim::CreateU32(2), /*is_signed=*/false, {});

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue placeholder,
                           internal::CreateInternalPlaceholderValueFromType(
                               enum_type));
  EXPECT_TRUE(
      InterpValue::MakeEnum(Bits(2), /*is_signed=*/false, enum_def)
          .Eq(placeholder));
  EXPECT_THAT(CreateZeroValueFromType(enum_type),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Cannot create zero value")));
}

TEST(InterpValueHelpersTest,
     CreateSumValueUsesInternalPlaceholderForInactiveEmptySumPayload) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* empty_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  auto* empty_def = module.Make<SumDef>(
      kFakeSpan, empty_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{}, /*is_public=*/false);
  empty_name->set_definer(empty_def);
  SumType empty_type(*empty_def, std::vector<SumTypeVariant>{});

  auto* outer_name = module.Make<NameDef>(kFakeSpan, "Outer", nullptr);
  auto* wrapped_name = module.Make<NameDef>(kFakeSpan, "Wrapped", nullptr);
  auto* nothing_name = module.Make<NameDef>(kFakeSpan, "Nothing", nullptr);
  auto* wrapped =
      module.Make<SumVariant>(kFakeSpan, wrapped_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{
                                  module.Make<TypeRefTypeAnnotation>(
                                      kFakeSpan,
                                      module.Make<TypeRef>(kFakeSpan, empty_def),
                                      std::vector<ExprOrType>{})},
                              std::vector<StructMemberNode*>{});
  auto* nothing =
      module.Make<SumVariant>(kFakeSpan, nothing_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* outer_def = module.Make<SumDef>(
      kFakeSpan, outer_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{wrapped, nothing}, /*is_public=*/false);
  outer_name->set_definer(outer_def);

  std::vector<SumTypeVariant> outer_variants;
  std::vector<std::unique_ptr<Type>> wrapped_members;
  wrapped_members.push_back(empty_type.CloneToUnique());
  outer_variants.push_back(
      SumTypeVariant::MakeTuple(*wrapped, std::move(wrapped_members)));
  outer_variants.push_back(SumTypeVariant::MakeUnit(*nothing));
  SumType outer_type(*outer_def, std::move(outer_variants));

  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           CreateSumValue(outer_type, "Nothing",
                                          no_payload_values));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(1, 1),
                   InterpValue::MakeTuple({InterpValue::MakeTuple(
                       {InterpValue::MakeUBits(1, 0),
                        InterpValue::MakeTuple({})})})})
                  .Eq(value));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsPayloadTypeMismatch) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  EXPECT_THAT(CreateSumValue(sum_type, "Byte", {InterpValue::MakeUBits(16, 1)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match")));
}

TEST(InterpValueHelpersTest, SignConvertValuePreservesSumEnumPayload) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* enum_name = module.Make<NameDef>(kFakeSpan, "Tag", nullptr);
  auto* enum_member_name = module.Make<NameDef>(kFakeSpan, "One", nullptr);
  TypeAnnotation* enum_element_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  Number* enum_member_value = module.Make<Number>(
      kFakeSpan, "1", NumberKind::kOther, enum_element_type);
  enum_member_name->set_definer(enum_member_value);
  EnumDef* enum_def = module.Make<EnumDef>(
      kFakeSpan, enum_name, enum_element_type,
      std::vector<EnumMember>{
          EnumMember{.name_def = enum_member_name, .value = enum_member_value}},
      /*is_public=*/false);
  enum_name->set_definer(enum_def);
  EnumType enum_type(*enum_def, TypeDim::CreateU32(2),
                     /*is_signed=*/false,
                     {InterpValue::MakeUBits(/*bit_count=*/2, /*value=*/1)});

  auto* sum_name = module.Make<NameDef>(kFakeSpan, "Example", nullptr);
  auto* some_name = module.Make<NameDef>(kFakeSpan, "Some", nullptr);
  auto* none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* some =
      module.Make<SumVariant>(kFakeSpan, some_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{
                                  module.Make<TypeRefTypeAnnotation>(
                                      kFakeSpan,
                                      module.Make<TypeRef>(kFakeSpan, enum_def),
                                      std::vector<ExprOrType>{})},
                              std::vector<StructMemberNode*>{});
  auto* none =
      module.Make<SumVariant>(kFakeSpan, none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{some, none}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  std::vector<std::unique_ptr<Type>> some_members;
  some_members.push_back(enum_type.CloneToUnique());
  variants.push_back(SumTypeVariant::MakeTuple(*some, std::move(some_members)));
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  SumType sum_type(*sum_def, std::move(variants));

  const InterpValue enum_value =
      InterpValue::MakeEnum(UBits(/*value=*/1, /*bit_count=*/2),
                            /*is_signed=*/false, enum_def);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue sum_value,
                           CreateSumValue(sum_type, "Some", {enum_value}));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue converted,
                           SignConvertValue(sum_type, sum_value));
  EXPECT_TRUE(converted.Eq(sum_value));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsActiveNonMemberEnumPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  EnumDef* enum_def = nullptr;
  SumType sum_type = MakeEnumPayloadSumType(module, &enum_def);

  InterpValue invalid_enum_value =
      InterpValue::MakeEnum(UBits(/*value=*/3, /*bit_count=*/2),
                            /*is_signed=*/false, enum_def);
  EXPECT_THAT(CreateSumValue(sum_type, "Some", {invalid_enum_value}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("declared member")));
}

TEST(InterpValueHelpersTest, InterpValueAsStringWorks) {
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue hello_world_u8_array,
                           InterpValue::MakeArray({
                               InterpValue::MakeUBits(/*bit_count=*/8, 72),
                               InterpValue::MakeUBits(/*bit_count=*/8, 101),
                               InterpValue::MakeUBits(/*bit_count=*/8, 108),
                               InterpValue::MakeUBits(/*bit_count=*/8, 108),
                               InterpValue::MakeUBits(/*bit_count=*/8, 111),
                               InterpValue::MakeUBits(/*bit_count=*/8, 32),
                               InterpValue::MakeUBits(/*bit_count=*/8, 119),
                               InterpValue::MakeUBits(/*bit_count=*/8, 111),
                               InterpValue::MakeUBits(/*bit_count=*/8, 114),
                               InterpValue::MakeUBits(/*bit_count=*/8, 108),
                               InterpValue::MakeUBits(/*bit_count=*/8, 100),
                               InterpValue::MakeUBits(/*bit_count=*/8, 33),
                           }));
  EXPECT_THAT(InterpValueAsString(hello_world_u8_array),
              IsOkAndHolds("Hello world!"));

  EXPECT_THAT(InterpValueAsString(InterpValue::MakeUBits(/*bit_count=*/8, 72)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("must be an array")));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue u9_array,
      InterpValue::MakeArray({InterpValue::MakeUBits(/*bit_count=*/9, 257)}));
  EXPECT_THAT(InterpValueAsString(u9_array),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("Array elements must be u8")));
}

TEST(InterpValueHelpersTest, ValueToInterpValue) {
  EXPECT_THAT(ValueToInterpValue(Value(UBits(3, 32))),
              IsOkAndHolds(Eq(InterpValue::MakeUBits(32, 3))));
  EXPECT_THAT(
      ValueToInterpValue(Value(UBits(3, 32)), BitsType::MakeU32().get()),
      IsOkAndHolds(Eq(InterpValue::MakeU32(3))));

  EXPECT_THAT(
      ValueToInterpValue(Value::UBitsArray({3, 4, 5}, 32).value()),
      IsOkAndHolds(Eq(InterpValue::MakeArray({
                                                 InterpValue::MakeU32(3),
                                                 InterpValue::MakeU32(4),
                                                 InterpValue::MakeU32(5),
                                             })
                          .value())));
  ArrayType array_type(BitsType::MakeU32(), TypeDim::CreateU32(3));
  EXPECT_THAT(
      ValueToInterpValue(Value::UBitsArray({3, 4, 5}, 32).value(), &array_type),
      IsOkAndHolds(Eq(InterpValue::MakeArray({
                                                 InterpValue::MakeU32(3),
                                                 InterpValue::MakeU32(4),
                                                 InterpValue::MakeU32(5),
                                             })
                          .value())));

  EXPECT_THAT(ValueToInterpValue(
                  Value::Tuple({Value(UBits(3, 32)), Value(UBits(4, 32))})),
              IsOkAndHolds(Eq(InterpValue::MakeTuple(
                  {InterpValue::MakeU32(3), InterpValue::MakeU32(4)}))));
  // Tuple values can either come from structs or tuples, try passing in a
  // compatible concrete type of both.
  EXPECT_THAT(
      ValueToInterpValue(
          Value::Tuple({Value(UBits(3, 32)), Value(UBits(4, 32))}),
          TupleType::Create2(BitsType::MakeU32(), BitsType::MakeU32()).get()),
      IsOkAndHolds(Eq(InterpValue::MakeTuple(
          {InterpValue::MakeU32(3), InterpValue::MakeU32(4)}))));
  NameDef struct_name_def(/*owner=*/nullptr, /*span=*/Span::Fake(), "my_struct",
                          /*definer=*/nullptr);

  NameDef struct_member_name_def(/*owner=*/nullptr, /*span=*/Span::Fake(),
                                 "member",
                                 /*definer=*/nullptr);
  StructMemberNode member(/* owner= */ nullptr, Span::Fake(),
                          /*name_def= */ &struct_member_name_def,
                          /*colon_span=*/Span::Fake(), /*type=*/nullptr);
  StructDef struct_def(/*owner=*/nullptr, /*span=*/Span::Fake(),
                       /*name_def=*/&struct_name_def,
                       /*parametric_bindings=*/{},
                       // these members are unused, but need to have the same
                       // number of elements as members in 'struct_type'.
                       /*members=*/
                       std::vector<StructMemberNode*>{&member, &member},
                       /*is_public=*/false);
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  members.push_back(BitsType::MakeU1());
  StructType struct_type(std::move(members), struct_def);
  EXPECT_THAT(ValueToInterpValue(
                  Value::Tuple({Value(UBits(3, 32)), Value(UBits(4, 32))}),
                  &struct_type),
              IsOkAndHolds(Eq(InterpValue::MakeTuple(
                  {InterpValue::MakeU32(3), InterpValue::MakeU32(4)}))));
}

TEST(InterpValueHelpersTest, ValueToInterpValueEnum) {
  EnumDef enum_def(/*owner=*/nullptr, /*span=*/Span::Fake(),
                   /*name_def=*/nullptr, /*type=*/{},
                   /*values=*/{}, /*is_public=*/false);
  EnumType enum_type(enum_def, TypeDim::CreateU32(32), /*is_signed=*/false, {});
  EXPECT_THAT(ValueToInterpValue(Value(UBits(3, 32)), &enum_type),
              IsOkAndHolds(Eq(InterpValue::MakeEnum(
                  UBits(3, 32), /*is_signed=*/false, &enum_def))));
}

TEST(InterpValueHelpersTest, ValueToInterpValueSumRejectsInvalidTag) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  Value raw = Value::Tuple({Value(UBits(3, 2)),
                            Value::Tuple({Value(UBits(0, 8)),
                                          Value(UBits(0, 16))})});
  EXPECT_THAT(ValueToInterpValue(raw, &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("invalid tag")));
}

TEST(InterpValueHelpersTest,
     ValueToInterpValueSumAcceptsInactiveEmptySumPlaceholder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyPayloadType(module);

  Value raw =
      Value::Tuple({Value(UBits(1, 1)),
                    Value::Tuple({Value::Tuple(
                        {Value(UBits(0, 1)), Value::Tuple({})})})});
  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(outer_type, "Nothing", no_payload_values));
  EXPECT_THAT(ValueToInterpValue(raw, &outer_type), IsOkAndHolds(Eq(expected)));
}

TEST(InterpValueHelpersTest,
     ValueToInterpValueSumAcceptsInactiveEmptyEnumPlaceholder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyEnumPayloadType(module);

  Value raw = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0, 2))})});
  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(outer_type, "Unit", no_payload_values));
  EXPECT_THAT(ValueToInterpValue(raw, &outer_type), IsOkAndHolds(Eq(expected)));
}

TEST(InterpValueHelpersTest,
     ValueToInterpValueSumRejectsNoncanonicalInactivePayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  Value raw = Value::Tuple({Value(UBits(0, 2)),
                            Value::Tuple({Value(UBits(1, 8)),
                                          Value(UBits(0, 16))})});
  EXPECT_THAT(ValueToInterpValue(raw, &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("noncanonical inactive payload slot")));
}

}  // namespace
}  // namespace xls::dslx
