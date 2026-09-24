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
#include "xls/dslx/interp_value_utils.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/make_value_format_descriptor.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/value.h"

namespace xls::dslx {
namespace {
using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::Eq;
using ::testing::HasSubstr;

SumType MakeMixedPayloadSumType(Module& module,
                                SumDef** sum_def_out = nullptr) {
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

  auto* none = module.Make<SumVariant>(
      kFakeSpan, none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* byte = module.Make<SumVariant>(
      kFakeSpan, byte_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u8_type}, std::vector<StructMemberNode*>{});
  auto* wide = module.Make<SumVariant>(
      kFakeSpan, wide_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u16_type}, std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, byte, wide}, /*is_public=*/false);
  sum_name->set_definer(sum_def);
  if (sum_def_out != nullptr) {
    *sum_def_out = sum_def;
  }

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
  auto* wrapped = module.Make<SumVariant>(
      kFakeSpan, wrapped_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, empty_def),
          std::vector<ExprOrType>{})},
      std::vector<StructMemberNode*>{});
  auto* nothing = module.Make<SumVariant>(
      kFakeSpan, nothing_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
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

  auto* outer_name =
      module.Make<NameDef>(kFakeSpan, "MaybeImpossible", nullptr);
  auto* unit_name = module.Make<NameDef>(kFakeSpan, "Unit", nullptr);
  auto* impossible_name =
      module.Make<NameDef>(kFakeSpan, "Impossible", nullptr);
  auto* unit = module.Make<SumVariant>(
      kFakeSpan, unit_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* impossible = module.Make<SumVariant>(
      kFakeSpan, impossible_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, enum_def),
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
  auto* vanilla_value = module.Make<Number>(kFakeSpan, "0", NumberKind::kOther,
                                            enum_element_type);
  vanilla_name->set_definer(vanilla_value);
  auto* mint_name = module.Make<NameDef>(kFakeSpan, "Mint", nullptr);
  auto* mint_value = module.Make<Number>(kFakeSpan, "1", NumberKind::kOther,
                                         enum_element_type);
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
  auto* some = module.Make<SumVariant>(
      kFakeSpan, some_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, enum_def),
          std::vector<ExprOrType>{})},
      std::vector<StructMemberNode*>{});
  auto* none = module.Make<SumVariant>(
      kFakeSpan, none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
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

SumType MakeOptionalPayloadSumType(Module& module, TypeAnnotation* annotation,
                                   std::unique_ptr<Type> payload_type) {
  const Span span = Span::Fake();
  auto* sum_name = module.Make<NameDef>(span, "Option", nullptr);
  auto* none_name = module.Make<NameDef>(span, "None", nullptr);
  auto* some_name = module.Make<NameDef>(span, "Some", nullptr);
  auto* none = module.Make<SumVariant>(
      span, none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* some =
      module.Make<SumVariant>(span, some_name, SumVariant::PayloadShape::kTuple,
                              std::vector<TypeAnnotation*>{annotation},
                              std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      span, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, some}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  std::vector<std::unique_ptr<Type>> payload_members;
  payload_members.push_back(std::move(payload_type));
  variants.push_back(
      SumTypeVariant::MakeTuple(*some, std::move(payload_members)));
  return SumType(*sum_def, std::move(variants));
}

SumType MakeOptionalPayloadSumType(Module& module, BuiltinType annotation_kind,
                                   std::unique_ptr<Type> payload_type) {
  auto* annotation = module.Make<BuiltinTypeAnnotation>(
      Span::Fake(), annotation_kind,
      module.GetOrCreateBuiltinNameDef(annotation_kind));
  return MakeOptionalPayloadSumType(module, annotation,
                                    std::move(payload_type));
}

// Counts actual leaf construction while preserving the counter through clones.
class DescriptorCountingBitsType : public BitsType {
 public:
  explicit DescriptorCountingBitsType(int64_t& visits)
      : BitsType(/*is_signed=*/false, 8), visits_(visits) {}

  absl::Status Accept(TypeVisitor& visitor) const override {
    ++visits_;
    return BitsType::Accept(visitor);
  }

  std::unique_ptr<Type> CloneToUnique() const override {
    return std::make_unique<DescriptorCountingBitsType>(visits_);
  }

 private:
  int64_t& visits_;
};

// enum Wrap<T: type> { Left(T), Right(T) }
SumDef* MakeBinarySumDef(Module& module) {
  const Span span = Span::Fake();
  auto* type_name = module.Make<NameDef>(span, "T", nullptr);
  auto* binding = module.Make<ParametricBinding>(
      type_name, module.Make<GenericTypeAnnotation>(span), std::nullopt);
  type_name->set_definer(binding);
  auto* annotation = module.Make<TypeVariableTypeAnnotation>(
      module.Make<NameRef>(span, "T", type_name), /*internal=*/false);
  std::vector<SumVariant*> variants;
  for (const char* name : {"Left", "Right"}) {
    variants.push_back(
        module.Make<SumVariant>(span, module.Make<NameDef>(span, name, nullptr),
                                SumVariant::PayloadShape::kTuple,
                                std::vector<TypeAnnotation*>{annotation},
                                std::vector<StructMemberNode*>{}));
  }
  auto* sum_def = module.Make<SumDef>(
      span, module.Make<NameDef>(span, "Wrap", nullptr),
      std::vector<ParametricBinding*>{binding}, std::move(variants),
      /*is_public=*/false);
  sum_def->name_def()->set_definer(sum_def);
  return sum_def;
}

std::unique_ptr<SumType> MakeBinarySumType(const SumDef& sum_def,
                                           const Type& payload_type) {
  std::vector<SumTypeVariant> variants;
  for (const SumVariant* variant : sum_def.variants()) {
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(payload_type.CloneToUnique());
    variants.push_back(SumTypeVariant::MakeTuple(*variant, std::move(members)));
  }
  std::vector<SumType::ParametricArgument> arguments;
  std::unique_ptr<const Type> argument = payload_type.CloneToUnique();
  arguments.emplace_back(std::move(argument));
  return std::make_unique<SumType>(
      sum_def, std::move(variants), /*tag_bit_count=*/std::nullopt,
      /*discriminants=*/std::vector<InterpValue>{}, std::move(arguments));
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

TEST(InterpValueHelpersTest, CastUnsignedBitsToSignedEnumPreservesEnumSign) {
  const Span kFakeSpan = Span::Fake();
  FileTable file_table;
  Module module("signed_enum_test", /*fs_path=*/std::nullopt, file_table);
  TypeAnnotation* element_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kS2,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kS2));
  NameDef* member_name = module.Make<NameDef>(kFakeSpan, "Neg", nullptr);
  Number* member_value =
      module.Make<Number>(kFakeSpan, "-1", NumberKind::kOther, element_type);
  member_name->set_definer(member_value);
  NameDef* enum_name = module.Make<NameDef>(kFakeSpan, "Signed", nullptr);
  EnumDef* enum_def =
      module.Make<EnumDef>(kFakeSpan, enum_name, element_type,
                           std::vector<EnumMember>{EnumMember{
                               .name_def = member_name, .value = member_value}},
                           /*is_public=*/false);
  enum_name->set_definer(enum_def);
  EnumType enum_type(*enum_def, TypeDim::CreateU32(2), /*is_signed=*/true,
                     std::vector<InterpValue>{InterpValue::MakeSBits(2, -1)});

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue directly_cast,
      CastBitsToEnum(InterpValue::MakeUBits(2, 3), enum_type));
  ASSERT_TRUE(directly_cast.IsEnum());
  EXPECT_TRUE(directly_cast.GetEnumData().value().is_signed);
  EXPECT_THAT(directly_cast.GetBitValueViaSign(), IsOkAndHolds(-1));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue converted,
      SignConvertValue(enum_type, InterpValue::MakeUBits(2, 3)));
  ASSERT_TRUE(converted.IsEnum());
  EXPECT_TRUE(converted.GetEnumData().value().is_signed);
  EXPECT_THAT(converted.GetBitValueViaSign(), IsOkAndHolds(-1));
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

TEST(InterpValueHelpersTest, CreateZeroSumValueFails) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* inner_name = module.Make<NameDef>(kFakeSpan, "Inner", nullptr);
  auto* inner_none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* inner_some_name = module.Make<NameDef>(kFakeSpan, "Some", nullptr);
  auto* u32_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));
  auto* inner_none = module.Make<SumVariant>(
      kFakeSpan, inner_none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* inner_some = module.Make<SumVariant>(
      kFakeSpan, inner_some_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u32_type}, std::vector<StructMemberNode*>{});
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
  auto* outer_wrap = module.Make<SumVariant>(
      kFakeSpan, outer_wrap_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u32_type}, std::vector<StructMemberNode*>{});
  auto* outer_none = module.Make<SumVariant>(
      kFakeSpan, outer_none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
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

  EXPECT_THAT(CreateZeroValueFromType(outer_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("semantic sum type `Outer`")));
}

TEST(InterpValueHelpersTest, CreatesSumWithBitsConstructorPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto payload_type = std::make_unique<ArrayType>(
      std::make_unique<BitsConstructorType>(TypeDim::CreateBool(false)),
      TypeDim::CreateU32(8));
  SumType sum_type = MakeOptionalPayloadSumType(module, BuiltinType::kU8,
                                                std::move(payload_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue result,
      CreateSumValue(sum_type, "Some", {InterpValue::MakeUBits(8, 7)}));
  const std::vector<InterpValue>& slots =
      result.GetValuesOrDie().at(1).GetValuesOrDie();
  ASSERT_EQ(slots.size(), 1);
  EXPECT_EQ(slots.at(0).GetBitValueUnsigned().value(), 7);
}

TEST(InterpValueHelpersTest, ConstructsIndexedPackedSumAndIgnoresOnlyPadding) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  const SumType sum_type = MakeMixedPayloadSumType(module);
  auto packed = [](uint64_t tag, uint64_t payload) {
    return InterpValue::MakeTuple(
        {InterpValue::MakeUBits(2, tag),
         InterpValue::MakeTuple({InterpValue::MakeUBits(16, payload)})});
  };

  EXPECT_THAT(CreateSumValue(sum_type, 0, {}), IsOkAndHolds(packed(0, 0)));
  EXPECT_THAT(CreateSumValue(sum_type, 1, {InterpValue::MakeU8(0x5a)}),
              IsOkAndHolds(packed(1, 0x5a)));
  EXPECT_THAT(CreateSumValue(sum_type, 2, {InterpValue::MakeUBits(16, 0xbeef)}),
              IsOkAndHolds(packed(2, 0xbeef)));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(1, 0xff5a)),
              IsOkAndHolds(testing::ElementsAre(InterpValue::MakeU8(0x5a))));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(0, 0xffff)),
              IsOkAndHolds(testing::IsEmpty()));
  EXPECT_THAT(
      CreateSumValue(sum_type, 1, {InterpValue::MakeUBits(16, 7)}),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("expected 8")));
  EXPECT_THAT(CreateSumValue(sum_type, 0, {InterpValue::MakeU8(7)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 0 payload values")));
  EXPECT_THAT(CreateSumValue(sum_type, 3, {}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("no constructor at index 3")));
}

TEST(InterpValueHelpersTest, IndexedPackedSumUsesSparseSemanticTags) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  const SumType implicit = MakeMixedPayloadSumType(module);
  std::vector<SumTypeVariant> variants;
  for (const SumTypeVariant& variant : implicit.variants()) {
    variants.push_back(variant.Clone());
  }
  const SumType sum_type(
      implicit.nominal_type(), std::move(variants), TypeDim::CreateU32(3),
      {InterpValue::MakeUBits(3, 5), InterpValue::MakeUBits(3, 1),
       InterpValue::MakeUBits(3, 7)});
  auto packed = [](uint64_t tag, uint64_t payload) {
    return InterpValue::MakeTuple(
        {InterpValue::MakeUBits(3, tag),
         InterpValue::MakeTuple({InterpValue::MakeUBits(16, payload)})});
  };

  EXPECT_THAT(CreateSumValue(sum_type, 0, {}), IsOkAndHolds(packed(5, 0)));
  EXPECT_THAT(CreateSumValue(sum_type, 1, {InterpValue::MakeU8(0xa6)}),
              IsOkAndHolds(packed(1, 0xa6)));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(5, 0)),
              IsOkAndHolds(testing::IsEmpty()));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(1, 0xa6)),
              IsOkAndHolds(testing::ElementsAre(InterpValue::MakeU8(0xa6))));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(0, 0)),
              StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
}

TEST(InterpValueHelpersTest, IndexedPackedSumEncodesNestedPayloadOrder) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* u4 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU4,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU4));
  auto* u32 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU32));
  auto* dimension = module.Make<Number>(span, "2", NumberKind::kOther, u32);
  auto* array_annotation =
      module.Make<ArrayTypeAnnotation>(span, u4, dimension);
  auto* tuple_annotation = module.Make<TupleTypeAnnotation>(
      span, std::vector<TypeAnnotation*>{u4, array_annotation});
  auto* sum_name = module.Make<NameDef>(span, "Example", nullptr);
  auto* none = module.Make<SumVariant>(
      span, module.Make<NameDef>(span, "None", nullptr),
      SumVariant::PayloadShape::kUnit, std::vector<TypeAnnotation*>{},
      std::vector<StructMemberNode*>{});
  auto* some = module.Make<SumVariant>(
      span, module.Make<NameDef>(span, "Some", nullptr),
      SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u4, tuple_annotation, u4},
      std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      span, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, some}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  const BitsType nibble_type(/*is_signed=*/false, 4);
  std::vector<std::unique_ptr<Type>> tuple_members;
  tuple_members.push_back(nibble_type.CloneToUnique());
  tuple_members.push_back(std::make_unique<ArrayType>(
      nibble_type.CloneToUnique(), TypeDim::CreateU32(2)));
  std::vector<std::unique_ptr<Type>> payload_members;
  payload_members.push_back(nibble_type.CloneToUnique());
  payload_members.push_back(
      std::make_unique<TupleType>(std::move(tuple_members)));
  payload_members.push_back(nibble_type.CloneToUnique());
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  variants.push_back(
      SumTypeVariant::MakeTuple(*some, std::move(payload_members)));
  const SumType sum_type(*sum_def, std::move(variants));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue array,
      InterpValue::MakeArray(
          {InterpValue::MakeUBits(4, 3), InterpValue::MakeUBits(4, 4)}));
  const std::vector<InterpValue> payload = {
      InterpValue::MakeUBits(4, 1),
      InterpValue::MakeTuple({InterpValue::MakeUBits(4, 2), array}),
      InterpValue::MakeUBits(4, 5)};
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue encoded,
                           CreateSumValue(sum_type, 1, payload));

  // Variant and tuple members are MSB first; array element zero is LSB first.
  EXPECT_EQ(
      encoded,
      InterpValue::MakeTuple(
          {InterpValue::MakeUBits(1, 1),
           InterpValue::MakeTuple({InterpValue::MakeUBits(20, 0x12435)})}));
  EXPECT_THAT(GetSumPayloadValues(sum_type, encoded), IsOkAndHolds(payload));
}

TEST(InterpValueHelpersTest, ShallowPackedObservationChecksShapeAndOuterTag) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  const SumType sum_type = MakeMixedPayloadSumType(module);
  auto packed = [](const InterpValue& tag, const InterpValue& payload) {
    return InterpValue::MakeTuple({tag, InterpValue::MakeTuple({payload})});
  };

  EXPECT_THAT(
      GetSumPayloadValues(sum_type, packed(InterpValue::MakeU8(1),
                                           InterpValue::MakeUBits(16, 0))),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("2-bit tag")));
  EXPECT_THAT(GetSumPayloadValues(sum_type, packed(InterpValue::MakeUBits(2, 1),
                                                   InterpValue::MakeU8(0))),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("16-bit payload slot")));
  EXPECT_THAT(
      GetSumPayloadValues(sum_type, packed(InterpValue::MakeSBits(2, 1),
                                           InterpValue::MakeUBits(16, 0))),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("to be unsigned bits")));
  EXPECT_THAT(
      GetSumPayloadValues(sum_type, packed(InterpValue::MakeUBits(2, 3),
                                           InterpValue::MakeUBits(16, 0))),
      StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));

  EnumDef* enum_def = nullptr;
  const SumType enum_sum = MakeEnumPayloadSumType(module, &enum_def);
  EXPECT_THAT(
      GetSumPayloadValues(enum_sum, packed(InterpValue::MakeUBits(1, 0),
                                           InterpValue::MakeUBits(2, 2))),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("expected a declared member")));
}

TEST(InterpValueHelpersTest, MatchObservationPreservesUnobservedNestedTag) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* inner_def = nullptr;
  const SumType inner_type = MakeMixedPayloadSumType(module, &inner_def);
  auto* annotation = module.Make<TypeRefTypeAnnotation>(
      span, module.Make<TypeRef>(span, inner_def), std::vector<ExprOrType>{});
  const SumType outer_type = MakeOptionalPayloadSumType(
      module, annotation, inner_type.CloneToUnique());
  const InterpValue invalid_inner = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(2, 3),
       InterpValue::MakeTuple({InterpValue::MakeUBits(16, 0xff5a)})});
  const InterpValue outer = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(1, 1),
       InterpValue::MakeTuple({InterpValue::MakeUBits(18, 0x3ff5a)})});
  EXPECT_THAT(CreateSumValue(outer_type, 1, {invalid_inner}),
              IsOkAndHolds(outer));
  EXPECT_THAT(GetSumPayloadValues(outer_type, outer),
              IsOkAndHolds(testing::ElementsAre(invalid_inner)));

  internal::MatchValueObservation observation;
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::vector<InterpValue>* first,
      observation.GetSumPayloadValues(outer_type, outer, {}));
  EXPECT_THAT(*first, testing::ElementsAre(invalid_inner));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::vector<InterpValue>* again,
      observation.GetSumPayloadValues(outer_type, outer, {}));
  EXPECT_EQ(first, again);
  EXPECT_THAT(observation.GetSumPayloadValues(inner_type, first->at(0), {0}),
              StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
  EXPECT_THAT(*first, testing::ElementsAre(invalid_inner));

  const InterpValue valid_outer = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(1, 1),
       InterpValue::MakeTuple({InterpValue::MakeUBits(18, 0x1005a)})});
  for (int i = 0; i < 2; ++i) {
    EXPECT_THAT(observation.EqualsConstant(valid_outer, outer, outer_type, {}),
                StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
  }
  EXPECT_THAT(*first, testing::ElementsAre(invalid_inner));
}

TEST(InterpValueHelpersTest, PackedEqualityIgnoresOnlyInactiveBits) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* inner_def = nullptr;
  const SumType inner_type = MakeMixedPayloadSumType(module, &inner_def);
  auto* annotation = module.Make<TypeRefTypeAnnotation>(
      Span::Fake(), module.Make<TypeRef>(Span::Fake(), inner_def),
      std::vector<ExprOrType>{});
  const SumType outer_type = MakeOptionalPayloadSumType(
      module, annotation, inner_type.CloneToUnique());
  auto packed = [](int64_t tag, int64_t payload) {
    return InterpValue::MakeTuple(
        {InterpValue::MakeUBits(1, tag),
         InterpValue::MakeTuple({InterpValue::MakeUBits(18, payload)})});
  };
  const InterpValue byte = packed(1, 0x1005a);
  const InterpValue padded_byte = packed(1, 0x1ff5a);
  const InterpValue malformed = packed(1, 0x3ff5a);

  EXPECT_TRUE(byte.Ne(padded_byte));
  EXPECT_THAT(internal::PackedValuesEqual(byte, padded_byte, outer_type),
              IsOkAndHolds(true));
  EXPECT_THAT(internal::PackedValuesEqual(byte, packed(1, 0x1005b), outer_type),
              IsOkAndHolds(false));
  EXPECT_THAT(internal::PackedValuesEqual(byte, packed(1, 0x2005a), outer_type),
              IsOkAndHolds(false));
  EXPECT_THAT(
      internal::PackedValuesEqual(packed(0, 0), packed(0, 0x3ff5a), outer_type),
      IsOkAndHolds(true));
  EXPECT_THAT(internal::PackedValuesEqual(packed(0, 0), malformed, outer_type),
              StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
  EXPECT_THAT(internal::PackedValuesEqual(malformed, byte, outer_type),
              StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));

  internal::MatchValueObservation observation;
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::vector<InterpValue>* first,
      observation.GetSumPayloadValues(outer_type, padded_byte, {}));
  EXPECT_THAT(observation.EqualsConstant(byte, padded_byte, outer_type, {}),
              IsOkAndHolds(true));
  EXPECT_THAT(observation.EqualsConstant(packed(1, 0x1005b), padded_byte,
                                         outer_type, {}),
              IsOkAndHolds(false));
  EXPECT_THAT(
      observation.EqualsConstant(malformed, padded_byte, outer_type, {}),
      StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
  EXPECT_THAT(observation.GetSumPayloadValues(outer_type, padded_byte, {}),
              IsOkAndHolds(first));
}

TEST(InterpValueHelpersTest, PackedEqualityChecksBothCompleteAggregates) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  const SumType sum_type = MakeMixedPayloadSumType(module);
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  members.push_back(std::make_unique<ArrayType>(sum_type.CloneToUnique(),
                                                TypeDim::CreateU32(1)));
  const TupleType type(std::move(members));
  auto aggregate = [](int64_t prefix, int64_t sum_tag) {
    InterpValue sum = InterpValue::MakeTuple(
        {InterpValue::MakeUBits(2, sum_tag),
         InterpValue::MakeTuple({InterpValue::MakeUBits(16, 0)})});
    return InterpValue::MakeTuple(
        {InterpValue::MakeU8(prefix), InterpValue::MakeArray({sum}).value()});
  };

  EXPECT_THAT(
      internal::PackedValuesEqual(aggregate(1, 0), aggregate(2, 0), type),
      IsOkAndHolds(false));
  EXPECT_THAT(
      internal::PackedValuesEqual(aggregate(1, 0), aggregate(2, 3), type),
      StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));
  EXPECT_THAT(
      internal::PackedValuesEqual(aggregate(1, 3), aggregate(2, 0), type),
      StatusIs(absl::StatusCode::kNotFound, HasSubstr("No variant")));

  EnumDef* enum_def = nullptr;
  const SumType enum_sum = MakeEnumPayloadSumType(module, &enum_def);
  const InterpValue invalid_enum = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(1, 0),
       InterpValue::MakeTuple({InterpValue::MakeUBits(2, 2)})});
  const InterpValue none = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(1, 1),
       InterpValue::MakeTuple({InterpValue::MakeUBits(2, 2)})});
  EXPECT_THAT(internal::PackedValuesEqual(none, invalid_enum, enum_sum),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected a declared member")));
}

TEST(InterpValueHelpersTest, CreatesActiveAndInactiveTokenSumPayloads) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeOptionalPayloadSumType(module, BuiltinType::kToken,
                                                std::make_unique<TokenType>());

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue inactive,
                           CreateSumValue(sum_type, "None", {}));
  EXPECT_TRUE(inactive.GetValuesOrDie().at(1).GetValuesOrDie().at(0).IsUBits());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue another_inactive,
                           CreateSumValue(sum_type, "None", {}));
  EXPECT_TRUE(inactive.Eq(another_inactive));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue active,
      CreateSumValue(sum_type, "Some", {InterpValue::MakeToken()}));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<InterpValue> payload,
                           GetSumPayloadValues(sum_type, active));
  ASSERT_EQ(payload.size(), 1);
  EXPECT_TRUE(payload.at(0).IsToken());
  EXPECT_THAT(CreateSumValue(sum_type, "Some", {InterpValue::MakeU8(0)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected token-typed value")));

  XLS_ASSERT_OK_AND_ASSIGN(Value raw, inactive.ConvertToIr());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue restored,
                           ValueToInterpValue(raw, &sum_type));
  EXPECT_TRUE(restored.GetValuesOrDie().at(1).GetValuesOrDie().at(0).IsUBits());
  EXPECT_TRUE(inactive.Eq(restored));
}

TEST(InterpValueHelpersTest,
     RoundTripsInactivePayloadWithTokenAndUninhabitedSum) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* never_name = module.Make<NameDef>(span, "Never", nullptr);
  auto* never_def =
      module.Make<SumDef>(span, never_name, std::vector<ParametricBinding*>{},
                          std::vector<SumVariant*>{}, /*is_public=*/false);
  never_name->set_definer(never_def);
  auto* token_annotation = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kToken,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kToken));
  auto* never_annotation = module.Make<TypeRefTypeAnnotation>(
      span, module.Make<TypeRef>(span, never_def), std::vector<ExprOrType>{});
  auto* tuple_annotation = module.Make<TupleTypeAnnotation>(
      span, std::vector<TypeAnnotation*>{token_annotation, never_annotation});

  std::vector<std::unique_ptr<Type>> tuple_members;
  tuple_members.push_back(std::make_unique<TokenType>());
  tuple_members.push_back(
      std::make_unique<SumType>(*never_def, std::vector<SumTypeVariant>{}));
  SumType sum_type = MakeOptionalPayloadSumType(
      module, tuple_annotation,
      std::make_unique<TupleType>(std::move(tuple_members)));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue inactive,
                           CreateSumValue(sum_type, "None", {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value raw, inactive.ConvertToIr());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue restored,
                           ValueToInterpValue(raw, &sum_type));
  EXPECT_TRUE(restored.GetValuesOrDie().at(1).GetValuesOrDie().at(0).IsUBits());
}

TEST(InterpValueHelpersTest, RoundTripsActiveAndInactiveProcSumPayloads) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* proc_name = module.Make<NameDef>(span, "Worker", nullptr);
  auto* proc_def = module.Make<ProcDef>(
      span, proc_name, std::vector<ParametricBinding*>{},
      std::vector<StructMemberNode*>{}, /*is_public=*/false);
  proc_name->set_definer(proc_def);
  auto* proc_annotation = module.Make<TypeRefTypeAnnotation>(
      span, module.Make<TypeRef>(span, proc_def), std::vector<ExprOrType>{});
  SumType sum_type = MakeOptionalPayloadSumType(
      module, proc_annotation,
      std::make_unique<ProcType>(std::vector<std::unique_ptr<Type>>{},
                                 *proc_def));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue inactive,
                           CreateSumValue(sum_type, "None", {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue active,
      CreateSumValue(sum_type, "Some", {InterpValue::MakeTuple({})}));

  for (const InterpValue& value : {inactive, active}) {
    XLS_ASSERT_OK_AND_ASSIGN(Value raw, value.ConvertToIr());
    XLS_ASSERT_OK_AND_ASSIGN(InterpValue restored,
                             ValueToInterpValue(raw, &sum_type));
    EXPECT_TRUE(restored.Eq(value));
  }
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
                       HasSubstr("semantic sum type `Empty`")));
}

TEST(InterpValueHelpersTest,
     CreateInternalPlaceholderEmptySumValueUsesZeroWidthTag) {
  const Span kFakeSpan = Span::Fake();

  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);

  auto* empty_name = module.Make<NameDef>(kFakeSpan, "Empty", nullptr);
  auto* empty_def = module.Make<SumDef>(
      kFakeSpan, empty_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{}, /*is_public=*/false);
  empty_name->set_definer(empty_def);
  SumType empty_type(*empty_def, std::vector<SumTypeVariant>{});

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue zero,
      internal::CreateInternalPlaceholderValueFromType(empty_type));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(0, 0),
                   InterpValue::MakeTuple({InterpValue::MakeUBits(0, 0)})})
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

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue placeholder,
      internal::CreateInternalPlaceholderValueFromType(enum_type));
  EXPECT_TRUE(InterpValue::MakeEnum(UBits(0, 2), /*is_signed=*/false, enum_def)
                  .Eq(placeholder));
}

TEST(InterpValueHelpersTest, CreateZeroEmptyEnumValueFails) {
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

  EXPECT_THAT(CreateZeroValueFromType(enum_type),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Cannot create zero value")));
}

TEST(InterpValueHelpersTest, CreateSumValueUsesSharedZeroedPayloadSlot) {
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
  auto* wrapped = module.Make<SumVariant>(
      kFakeSpan, wrapped_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, empty_def),
          std::vector<ExprOrType>{})},
      std::vector<StructMemberNode*>{});
  auto* nothing = module.Make<SumVariant>(
      kFakeSpan, nothing_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
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
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue value,
      CreateSumValue(outer_type, /*variant_index=*/1, no_payload_values));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(1, 1),
                   InterpValue::MakeTuple({InterpValue::MakeUBits(0, 0)})})
                  .Eq(value));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsPayloadTypeMismatch) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  EXPECT_THAT(CreateSumValue(sum_type, "Byte", {InterpValue::MakeUBits(16, 1)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match")));
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/1,
                             {InterpValue::MakeUBits(16, 1)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match")));
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/1, {}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 1 payload values; got 0")));
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/3, {}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("no constructor at index 3")));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsMalformedTuplePayload) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  auto* tuple_annotation =
      module.Make<TupleTypeAnnotation>(span, std::vector<TypeAnnotation*>{u8});
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  SumType sum_type = MakeOptionalPayloadSumType(
      module, tuple_annotation,
      std::make_unique<TupleType>(std::move(members)));

  EXPECT_THAT(CreateSumValue(sum_type, "Some", {InterpValue::MakeU8(7)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected tuple-typed value")));
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/1,
                             {InterpValue::MakeTuple({})}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 1 members; got 0")));
  EXPECT_THAT(
      CreateSumValue(sum_type, "Some",
                     {InterpValue::MakeTuple({InterpValue::MakeUBits(16, 7)})}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("does not match")));

  Value malformed_raw =
      Value::Tuple({Value(UBits(1, 1)), Value::Tuple({Value::Tuple({})})});
  EXPECT_THAT(ValueToInterpValue(malformed_raw, &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("tag and payload slot to be unsigned bits")));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsMalformedStructPayload) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  auto* struct_name = module.Make<NameDef>(span, "Payload", nullptr);
  std::vector<StructMemberNode*> fields = {
      module.Make<StructMemberNode>(
          span, module.Make<NameDef>(span, "value", nullptr), span, u8),
  };
  auto* struct_def = module.Make<StructDef>(
      span, struct_name, std::vector<ParametricBinding*>{}, fields,
      /*is_public=*/false);
  struct_name->set_definer(struct_def);
  auto* annotation = module.Make<TypeRefTypeAnnotation>(
      span, module.Make<TypeRef>(span, struct_def), std::vector<ExprOrType>{});
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  SumType sum_type = MakeOptionalPayloadSumType(
      module, annotation,
      std::make_unique<StructType>(std::move(members), *struct_def));

  EXPECT_THAT(CreateSumValue(sum_type, "Some", {InterpValue::MakeU8(7)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected struct-typed value")));
  EXPECT_THAT(CreateSumValue(sum_type, "Some", {InterpValue::MakeTuple({})}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 1 members; got 0")));
  EXPECT_THAT(
      CreateSumValue(sum_type, "Some",
                     {InterpValue::MakeTuple({InterpValue::MakeUBits(16, 7)})}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("does not match")));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsMalformedArrayPayload) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  auto* u32 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU32));
  auto* dimension = module.Make<Number>(span, "2", NumberKind::kOther, u32);
  auto* annotation = module.Make<ArrayTypeAnnotation>(span, u8, dimension);
  SumType sum_type = MakeOptionalPayloadSumType(
      module, annotation,
      std::make_unique<ArrayType>(BitsType::MakeU8(), TypeDim::CreateU32(2)));

  EXPECT_THAT(CreateSumValue(sum_type, "Some", {InterpValue::MakeU8(7)}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected array-typed value")));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue short_array,
                           InterpValue::MakeArray({InterpValue::MakeU8(7)}));
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/1, {short_array}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 2 elements; got 1")));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue wide_array,
      InterpValue::MakeArray(
          {InterpValue::MakeUBits(16, 7), InterpValue::MakeUBits(16, 8)}));
  EXPECT_THAT(CreateSumValue(sum_type, "Some", {wide_array}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match")));
}

TEST(InterpValueHelpersTest, MakeSumValueFormatDescriptorRejectsWidthOverflow) {
  constexpr int64_t kMaxWidth = std::numeric_limits<uint32_t>::max();
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* unsigned_bits = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kUN,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kUN));
  auto* u32 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU32));
  auto make_sum = [&](int64_t payload_width) {
    auto* dimension = module.Make<Number>(span, absl::StrCat(payload_width),
                                          NumberKind::kOther, u32);
    auto* annotation =
        module.Make<ArrayTypeAnnotation>(span, unsigned_bits, dimension);
    return MakeOptionalPayloadSumType(
        module, annotation,
        std::make_unique<BitsType>(/*is_signed=*/false, payload_width));
  };

  const SumType boundary = make_sum(kMaxWidth - 1);
  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeValueFormatDescriptor(boundary, FormatPreference::kDefault));
  EXPECT_EQ(descriptor.sum_tag_bit_count(), 1);
  EXPECT_EQ(descriptor.sum_payload_slot_bit_count(), kMaxWidth - 1);
  EXPECT_EQ(descriptor.flat_bit_count(), kMaxWidth);

  // Neither input width overflows; only their combined sum width does. No value
  // or storage proportional to the represented payload is constructed.
  const SumType overflow = make_sum(kMaxWidth);
  EXPECT_THAT(
      MakeValueFormatDescriptor(overflow, FormatPreference::kDefault),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("shared sum bit count exceeds 4294967295 bits")));
}

// Verifies: Production sum formatting ignores inactive padding.
// Catches: Padding changing semantic text or the observed representation.
TEST(InterpValueHelpersTest,
     FormatsEverySumVariantFromItsCanonicalProductionTypeDescriptor) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* sum_name = module.Make<NameDef>(span, "Option", nullptr);
  auto* none_name = module.Make<NameDef>(span, "None", nullptr);
  auto* some_name = module.Make<NameDef>(span, "Some", nullptr);
  auto* pair_name = module.Make<NameDef>(span, "Pair", nullptr);
  auto* empty_tuple_name = module.Make<NameDef>(span, "EmptyTuple", nullptr);
  auto* empty_struct_name = module.Make<NameDef>(span, "EmptyStruct", nullptr);
  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  auto* u16 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU16,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU16));
  auto* none = module.Make<SumVariant>(
      span, none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* some = module.Make<SumVariant>(
      span, some_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{u8}, std::vector<StructMemberNode*>{});
  std::vector<StructMemberNode*> fields = {
      module.Make<StructMemberNode>(
          span, module.Make<NameDef>(span, "left", nullptr), span, u8),
      module.Make<StructMemberNode>(
          span, module.Make<NameDef>(span, "right", nullptr), span, u16),
  };
  auto* pair = module.Make<SumVariant>(span, pair_name,
                                       SumVariant::PayloadShape::kStruct,
                                       std::vector<TypeAnnotation*>{}, fields);
  auto* empty_tuple = module.Make<SumVariant>(
      span, empty_tuple_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* empty_struct = module.Make<SumVariant>(
      span, empty_struct_name, SumVariant::PayloadShape::kStruct,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      span, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, some, pair, empty_tuple, empty_struct},
      /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeUnit(*none));
  std::vector<std::unique_ptr<Type>> some_types;
  some_types.push_back(BitsType::MakeU8());
  variants.push_back(SumTypeVariant::MakeTuple(*some, std::move(some_types)));
  std::vector<std::unique_ptr<Type>> payload_types;
  payload_types.push_back(BitsType::MakeU8());
  payload_types.push_back(std::make_unique<BitsType>(false, 16));
  variants.push_back(
      SumTypeVariant::MakeStruct(*pair, std::move(payload_types)));
  variants.push_back(SumTypeVariant::MakeTuple(
      *empty_tuple, std::vector<std::unique_ptr<Type>>{}));
  variants.push_back(SumTypeVariant::MakeStruct(
      *empty_struct, std::vector<std::unique_ptr<Type>>{}));
  SumType sum_type(*sum_def, std::move(variants));

  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeValueFormatDescriptor(sum_type, FormatPreference::kDefault));
  ASSERT_EQ(descriptor.sum_variant_count(), 5);
  EXPECT_EQ(descriptor.sum_tag_bit_count(), 3);
  EXPECT_EQ(descriptor.sum_payload_slot_bit_count(), 24);
  const ValueFormatSumVariantView none_view = descriptor.sum_variant(0);
  EXPECT_EQ(none_view.name(), "None");
  EXPECT_EQ(none_view.kind(), ValueFormatSumVariantKind::kUnit);
  EXPECT_EQ(none_view.payload_member_count(), 0);
  EXPECT_THAT(none_view.field_names(), ::testing::IsEmpty());
  EXPECT_THAT(none_view.payload_formats(), ::testing::IsEmpty());

  const ValueFormatSumVariantView some_view = descriptor.sum_variant(1);
  EXPECT_EQ(some_view.name(), "Some");
  EXPECT_EQ(some_view.kind(), ValueFormatSumVariantKind::kTuple);
  EXPECT_EQ(some_view.payload_member_count(), 1);
  EXPECT_THAT(some_view.field_names(), ::testing::IsEmpty());
  ASSERT_EQ(some_view.payload_formats().size(), 1);
  EXPECT_TRUE(some_view.payload_formats().front().IsLeafValue());

  const ValueFormatSumVariantView pair_view = descriptor.sum_variant(2);
  EXPECT_EQ(pair_view.name(), "Pair");
  EXPECT_EQ(pair_view.kind(), ValueFormatSumVariantKind::kStruct);
  EXPECT_EQ(pair_view.payload_member_count(), 2);
  EXPECT_THAT(pair_view.field_names(), ::testing::ElementsAre("left", "right"));
  ASSERT_EQ(pair_view.payload_formats().size(), 2);
  EXPECT_TRUE(pair_view.payload_formats().front().IsLeafValue());
  EXPECT_TRUE(pair_view.payload_formats().back().IsLeafValue());

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue none_value,
                           CreateSumValue(sum_type, "None", {}));
  EXPECT_THAT(none_value.ToFormattedString(descriptor,
                                           /*include_type_prefix=*/true),
              IsOkAndHolds("Option::None"));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue some_value,
      CreateSumValue(sum_type, "Some", {InterpValue::MakeUBits(8, 7)}));
  EXPECT_THAT(some_value.ToFormattedString(descriptor,
                                           /*include_type_prefix=*/true),
              IsOkAndHolds("Option::Some(u8:7)"));
  const InterpValue dirty_some = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(3, 1), InterpValue::MakeUBits(24, 0xffff07));
  EXPECT_THAT(dirty_some.ToFormattedString(descriptor,
                                           /*include_type_prefix=*/true),
              IsOkAndHolds("Option::Some(u8:7)"));
  XLS_ASSERT_OK(ValidateInterpValueMatchesType(dirty_some, sum_type));
  EXPECT_EQ(dirty_some.GetValuesOrDie().at(1).GetValuesOrDie().at(0),
            InterpValue::MakeUBits(24, 0xffff07));
  const InterpValue undeclared = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(3, 7), InterpValue::MakeUBits(24, 0xffff07));
  EXPECT_THAT(undeclared.ToFormattedString(descriptor,
                                           /*include_type_prefix=*/true),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("is not declared")));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           CreateSumValue(sum_type, "Pair",
                                          {InterpValue::MakeUBits(8, 3),
                                           InterpValue::MakeUBits(16, 4)}));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string formatted,
      value.ToFormattedString(descriptor, /*include_type_prefix=*/true));
  EXPECT_EQ(formatted, "Option::Pair {\n    left: u8:3,\n    right: u16:4\n}");

  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor hex_descriptor,
      MakeValueFormatDescriptor(sum_type, FormatPreference::kHex));
  EXPECT_THAT(some_value.ToFormattedString(hex_descriptor,
                                           /*include_type_prefix=*/true),
              IsOkAndHolds("Option::Some(u8:0x7)"));
  EXPECT_THAT(
      value.ToFormattedString(hex_descriptor,
                              /*include_type_prefix=*/true),
      IsOkAndHolds("Option::Pair {\n    left: u8:0x3,\n    right: u16:0x4\n}"));

  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor binary_descriptor,
      MakeValueFormatDescriptor(sum_type, FormatPreference::kBinary));
  EXPECT_THAT(some_value.ToFormattedString(binary_descriptor,
                                           /*include_type_prefix=*/true),
              IsOkAndHolds("Option::Some(u8:0b111)"));
  EXPECT_THAT(value.ToFormattedString(binary_descriptor,
                                      /*include_type_prefix=*/true),
              IsOkAndHolds("Option::Pair {\n    left: u8:0b11,\n    right: "
                           "u16:0b100\n}"));

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue empty_tuple_value,
                           CreateSumValue(sum_type, "EmptyTuple", {}));
  EXPECT_THAT(empty_tuple_value.ToFormattedString(descriptor,
                                                  /*include_type_prefix=*/true),
              IsOkAndHolds("Option::EmptyTuple()"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue empty_struct_value,
                           CreateSumValue(sum_type, "EmptyStruct", {}));
  EXPECT_THAT(empty_struct_value.ToFormattedString(
                  descriptor, /*include_type_prefix=*/true),
              IsOkAndHolds("Option::EmptyStruct {}"));
}

// Both construction and retention must stay bounded: memoization alone still
// expands copies, and shared storage alone still rebuilds every alternative.
TEST(InterpValueHelpersTest,
     SumFormatConstructionAndRetentionPreserveSharedDescriptions) {
  constexpr int64_t kDepth = 8;
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = MakeBinarySumDef(module);
  int64_t leaf_visits = 0;
  std::unique_ptr<Type> type =
      std::make_unique<DescriptorCountingBitsType>(leaf_visits);
  for (int64_t i = 0; i < kDepth; ++i) {
    type = MakeBinarySumType(*sum_def, *type);
  }
  leaf_visits = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeValueFormatDescriptor(*type, FormatPreference::kDefault));
  EXPECT_EQ(leaf_visits, 2);
  EXPECT_EQ(descriptor.flat_bit_count(), 8 + kDepth);

  std::set<const ValueFormatDescriptor*> sum_payloads;
  std::vector<const ValueFormatDescriptor*> pending = {&descriptor};
  int64_t descriptor_count = 0;
  while (!pending.empty()) {
    const ValueFormatDescriptor* current = pending.back();
    pending.pop_back();
    ++descriptor_count;
    if (current->IsSum()) {
      ASSERT_EQ(current->sum_variant_count(), 2);
      auto payload = current->sum_variant(0).payload_formats();
      ASSERT_EQ(payload.size(), 1);
      if (sum_payloads.insert(payload.data()).second) {
        for (int64_t i = 0; i < 2; ++i) {
          auto members = current->sum_variant(i).payload_formats();
          ASSERT_EQ(members.size(), 1);
          pending.push_back(&members.front());
        }
      }
    } else {
      EXPECT_TRUE(current->IsLeafValue());
    }
  }
  EXPECT_EQ(sum_payloads.size(), kDepth);
  EXPECT_EQ(descriptor_count, 2 * kDepth + 1);

  ValueFormatDescriptor copy = descriptor;
  EXPECT_EQ(copy.sum_variant(0).payload_formats().data(),
            descriptor.sum_variant(0).payload_formats().data());
}

TEST(InterpValueHelpersTest, SumFormatMemoCrossesAggregateBoundaries) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = MakeBinarySumDef(module);
  int64_t leaf_visits = 0;
  DescriptorCountingBitsType bits(leaf_visits);
  std::unique_ptr<SumType> sum = MakeBinarySumType(*sum_def, bits);

  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  auto* annotation = module.Make<TypeRefTypeAnnotation>(
      span, module.Make<TypeRef>(span, sum_def), std::vector<ExprOrType>{u8});
  auto* struct_def = module.Make<StructDef>(
      span, module.Make<NameDef>(span, "Holder", nullptr),
      std::vector<ParametricBinding*>{},
      std::vector<StructMemberNode*>{module.Make<StructMemberNode>(
          span, module.Make<NameDef>(span, "value", nullptr), span,
          annotation)},
      /*is_public=*/false);
  struct_def->name_def()->set_definer(struct_def);
  std::vector<std::unique_ptr<Type>> struct_members;
  struct_members.push_back(sum->CloneToUnique());
  std::vector<std::unique_ptr<Type>> nested_tuple_members;
  nested_tuple_members.push_back(sum->CloneToUnique());
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(sum->CloneToUnique());
  members.push_back(
      std::make_unique<TupleType>(std::move(nested_tuple_members)));
  members.push_back(
      std::make_unique<ArrayType>(sum->CloneToUnique(), TypeDim::CreateU32(2)));
  members.push_back(
      std::make_unique<StructType>(std::move(struct_members), *struct_def));
  TupleType type(std::move(members));

  leaf_visits = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeValueFormatDescriptor(type, FormatPreference::kHex));
  EXPECT_EQ(leaf_visits, 2);
  auto elements = descriptor.tuple_elements();
  ASSERT_EQ(elements.size(), 4);
  const auto* payload = elements[0].sum_variant(0).payload_formats().data();
  const std::vector<const ValueFormatDescriptor*> nested = {
      &elements[1].tuple_elements()[0],
      &elements[2].array_element_format(),
      &elements[3].struct_elements()[0],
  };
  for (const ValueFormatDescriptor* element : nested) {
    EXPECT_EQ(element->sum_variant(0).payload_formats().data(), payload);
    EXPECT_EQ(element->sum_variant(0).payload_formats()[0].leaf_format(),
              FormatPreference::kHex);
  }
}

TEST(InterpValueHelpersTest, SumFormatMemoUsesCompleteDescriptionIdentity) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  Module foreign_module("foreign", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = MakeBinarySumDef(module);
  std::unique_ptr<SumType> byte_sum =
      MakeBinarySumType(*sum_def, *BitsType::MakeU8());
  std::unique_ptr<SumType> wide_sum =
      MakeBinarySumType(*sum_def, BitsType(/*is_signed=*/false, 16));
  std::unique_ptr<SumType> foreign_sum =
      MakeBinarySumType(*MakeBinarySumDef(foreign_module), *BitsType::MakeU8());
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(byte_sum->CloneToUnique());
  members.push_back(byte_sum->CloneToUnique());
  members.push_back(std::move(wide_sum));
  members.push_back(std::move(foreign_sum));
  for (const char* name : {"EmptyLeft", "EmptyRight"}) {
    auto* empty_def = module.Make<SumDef>(
        span, module.Make<NameDef>(span, name, nullptr),
        std::vector<ParametricBinding*>{}, std::vector<SumVariant*>{},
        /*is_public=*/false);
    empty_def->name_def()->set_definer(empty_def);
    members.push_back(
        std::make_unique<SumType>(*empty_def, std::vector<SumTypeVariant>{}));
  }
  TupleType type(std::move(members));
  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeValueFormatDescriptor(type, FormatPreference::kDefault));
  auto elements = descriptor.tuple_elements();
  ASSERT_EQ(elements.size(), 6);
  const auto* payload = elements[0].sum_variant(0).payload_formats().data();
  EXPECT_EQ(elements[1].sum_variant(0).payload_formats().data(), payload);
  EXPECT_NE(elements[2].sum_variant(0).payload_formats().data(), payload);
  EXPECT_NE(elements[3].sum_variant(0).payload_formats().data(), payload);
  EXPECT_EQ(elements[0].flat_bit_count(), 9);
  EXPECT_EQ(elements[2].flat_bit_count(), 17);
  EXPECT_EQ(elements[4].sum_name(), "EmptyLeft");
  EXPECT_EQ(elements[5].sum_name(), "EmptyRight");
  EXPECT_EQ(elements[4].sum_variant_count(), 0);
  EXPECT_EQ(elements[5].sum_variant_count(), 0);
}

TEST(InterpValueHelpersTest, SumFormatCopiesOwnMetadataAndPreservePreferences) {
  const std::vector<std::pair<FormatPreference, std::string>> cases = {
      {FormatPreference::kDefault, "Wrap::Left(Wrap::Right(u8:7))"},
      {FormatPreference::kHex, "Wrap::Left(Wrap::Right(u8:0x7))"},
      {FormatPreference::kBinary, "Wrap::Left(Wrap::Right(u8:0b111))"},
  };
  std::vector<ValueFormatDescriptor> descriptors;
  InterpValue value = InterpValue::MakeUnit();
  {
    FileTable file_table;
    Module module("test", /*fs_path=*/std::nullopt, file_table);
    SumDef* sum_def = MakeBinarySumDef(module);
    std::unique_ptr<SumType> inner =
        MakeBinarySumType(*sum_def, *BitsType::MakeU8());
    std::unique_ptr<SumType> outer = MakeBinarySumType(*sum_def, *inner);
    XLS_ASSERT_OK_AND_ASSIGN(
        InterpValue inner_value,
        CreateSumValue(*inner, "Right", {InterpValue::MakeU8(7)}));
    XLS_ASSERT_OK_AND_ASSIGN(value,
                             CreateSumValue(*outer, "Left", {inner_value}));
    for (const auto& format_case : cases) {
      XLS_ASSERT_OK_AND_ASSIGN(
          ValueFormatDescriptor descriptor,
          MakeValueFormatDescriptor(*outer, format_case.first));
      descriptors.push_back(descriptor);
      EXPECT_EQ(descriptors.back().sum_variant(0).payload_formats().data(),
                descriptor.sum_variant(0).payload_formats().data());
    }
  }
  // The original descriptors, Types, and source module have all been destroyed.
  for (int64_t i = 0; i < cases.size(); ++i) {
    EXPECT_THAT(value.ToFormattedString(descriptors[i],
                                        /*include_type_prefix=*/true),
                IsOkAndHolds(cases[i].second));
  }
}

TEST(InterpValueHelpersTest, SumFormatsKeepNumericEnumDefaultRendering) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  EnumDef* enum_def = nullptr;
  SumType type = MakeEnumPayloadSumType(module, &enum_def);
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue value,
      CreateSumValue(
          type, "Some",
          {InterpValue::MakeEnum(UBits(1, 2), /*is_signed=*/false, enum_def)}));
  const InterpValue invalid_enum = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(1, 0), InterpValue::MakeUBits(2, 3));
  for (FormatPreference preference :
       {FormatPreference::kDefault, FormatPreference::kHex,
        FormatPreference::kBinary}) {
    XLS_ASSERT_OK_AND_ASSIGN(ValueFormatDescriptor descriptor,
                             MakeValueFormatDescriptor(type, preference));
    EXPECT_THAT(value.ToFormattedString(descriptor,
                                        /*include_type_prefix=*/true),
                IsOkAndHolds("Choice::Some(Flavor::Mint  // u2:1)"));
    EXPECT_THAT(invalid_enum.ToFormattedString(descriptor,
                                               /*include_type_prefix=*/true),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("was not found in enum descriptor")));
  }
}

// Verifies: Owned payloads survive matchee destruction and ancestor validation.
// Catches: Invalidated references, Type-pointer keys, or distinct-path
// aliasing.
TEST(InterpValueHelpersTest, MatchObservationOwnsPayloadsAtStructuralPaths) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType type = MakeMixedPayloadSumType(module);
  internal::MatchValueObservation observation;
  const std::vector<InterpValue>* first_payload;
  {
    const InterpValue temporary = internal::CreateEncodedSumTuple(
        InterpValue::MakeUBits(2, 1), InterpValue::MakeUBits(16, 0xff2a));
    XLS_ASSERT_OK_AND_ASSIGN(
        first_payload, observation.GetSumPayloadValues(type, temporary, {0}));
  }
  EXPECT_THAT(*first_payload, ::testing::ElementsAre(InterpValue::MakeU8(42)));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue first,
      CreateSumValue(type, "Byte", {InterpValue::MakeU8(42)}));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue second,
      CreateSumValue(type, "Byte", {InterpValue::MakeU8(7)}));
  const InterpValue dirty_first = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(2, 1), InterpValue::MakeUBits(16, 0xff2a));
  const InterpValue values = InterpValue::MakeTuple({dirty_first, second});
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(type.CloneToUnique());
  members.push_back(type.CloneToUnique());
  const TupleType tuple_type(std::move(members));
  EXPECT_THAT(
      observation.EqualsConstant(InterpValue::MakeTuple({second, first}),
                                 values, tuple_type, {}),
      IsOkAndHolds(false));
  EXPECT_THAT(
      observation.EqualsConstant(InterpValue::MakeTuple({first, second}),
                                 values, *tuple_type.CloneToUnique(), {}),
      IsOkAndHolds(true));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* reused, observation.GetSumPayloadValues(
                                                   type, dirty_first, {0}));
  EXPECT_EQ(first_payload, reused);
  XLS_ASSERT_OK_AND_ASSIGN(const auto* other,
                           observation.GetSumPayloadValues(type, second, {1}));
  EXPECT_NE(first_payload, other);
  EXPECT_THAT(*other, ::testing::ElementsAre(InterpValue::MakeU8(7)));
  EXPECT_THAT(*first_payload, ::testing::ElementsAre(InterpValue::MakeU8(42)));
}

// Negative test: Failed aggregate validation must return its error again.
TEST(InterpValueHelpersTest, MatchObservationDoesNotRetainFailedValidation) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType type = MakeMixedPayloadSumType(module);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue none, CreateSumValue(type, "None", {}));
  const InterpValue malformed = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(2, 3), InterpValue::MakeUBits(16, 0));
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU1());
  members.push_back(type.CloneToUnique());
  const TupleType tuple_type(std::move(members));
  const InterpValue value =
      InterpValue::MakeTuple({InterpValue::MakeBool(false), malformed});
  internal::MatchValueObservation observation;
  for (bool first : {false, true}) {
    const InterpValue constant =
        InterpValue::MakeTuple({InterpValue::MakeBool(first), none});
    EXPECT_THAT(observation.EqualsConstant(constant, value, tuple_type, {}),
                StatusIs(absl::StatusCode::kNotFound,
                         HasSubstr("No variant with tag bits")));
  }
}

// Negative test: Invalid constants error before invalid or cached scrutinees.
TEST(InterpValueHelpersTest, MatchObservationValidatesConstantFirst) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType type = MakeMixedPayloadSumType(module);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue none, CreateSumValue(type, "None", {}));
  const InterpValue bad_constant = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(2, 3), InterpValue::MakeUBits(16, 0));
  const InterpValue bad_scrutinee = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(3, 0), InterpValue::MakeUBits(16, 0));
  // Source constants cannot construct malformed sums. Supply them directly at
  // the shared typed comparison boundary to verify operand error precedence.
  internal::MatchValueObservation observation;
  EXPECT_THAT(observation.EqualsConstant(none, none, type, {0}),
              IsOkAndHolds(true));
  EXPECT_THAT(observation.EqualsConstant(bad_constant, none, type, {0}),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("No variant with tag bits")));
  EXPECT_THAT(
      observation.EqualsConstant(bad_constant, bad_scrutinee, type, {1}),
      StatusIs(absl::StatusCode::kNotFound,
               HasSubstr("No variant with tag bits")));
  EXPECT_THAT(observation.EqualsConstant(none, bad_scrutinee, type, {1}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected a 2-bit tag")));
}

TEST(InterpValueHelpersTest,
     ValidatesDeeplyNestedAggregateWrappedSemanticSums) {
  const Span span = Span::Fake();
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* base_name = module.Make<NameDef>(span, "Base", nullptr);
  auto* unit_name = module.Make<NameDef>(span, "Unit", nullptr);
  auto* second_name = module.Make<NameDef>(span, "Second", nullptr);
  auto* third_name = module.Make<NameDef>(span, "Third", nullptr);
  auto* unit = module.Make<SumVariant>(
      span, unit_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* second = module.Make<SumVariant>(
      span, second_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* third = module.Make<SumVariant>(
      span, third_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* base_def =
      module.Make<SumDef>(span, base_name, std::vector<ParametricBinding*>{},
                          std::vector<SumVariant*>{unit, second, third},
                          /*is_public=*/false);
  base_name->set_definer(base_def);
  SumDef* current_def = base_def;
  std::vector<SumTypeVariant> base_variants;
  base_variants.push_back(SumTypeVariant::MakeUnit(*unit));
  base_variants.push_back(SumTypeVariant::MakeUnit(*second));
  base_variants.push_back(SumTypeVariant::MakeUnit(*third));
  auto current = std::make_unique<SumType>(*base_def, std::move(base_variants));
  InterpValue value = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(2, 0), InterpValue::MakeUBits(0, 0));
  InterpValue malformed = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(2, 3), InterpValue::MakeUBits(0, 0));

  for (int64_t depth = 0; depth < 24; ++depth) {
    auto* outer_name =
        module.Make<NameDef>(span, absl::StrCat("Outer", depth), nullptr);
    auto* wrap_name =
        module.Make<NameDef>(span, absl::StrCat("Wrap", depth), nullptr);
    TypeAnnotation* annotation = module.Make<TypeRefTypeAnnotation>(
        span, module.Make<TypeRef>(span, current_def),
        std::vector<ExprOrType>{});
    std::unique_ptr<Type> payload_type = current->CloneToUnique();
    if (depth % 3 == 0) {
      // An aggregate between sums must not make decoding revalidate and decode
      // the entire inner sum again. The packed image of a one-tuple is
      // unchanged.
      annotation = module.Make<TupleTypeAnnotation>(
          span, std::vector<TypeAnnotation*>{annotation});
      std::vector<std::unique_ptr<Type>> tuple_members;
      tuple_members.push_back(std::move(payload_type));
      payload_type = std::make_unique<TupleType>(std::move(tuple_members));
    }
    auto* wrap = module.Make<SumVariant>(
        span, wrap_name, SumVariant::PayloadShape::kTuple,
        std::vector<TypeAnnotation*>{annotation},
        std::vector<StructMemberNode*>{});
    auto* outer_def = module.Make<SumDef>(
        span, outer_name, std::vector<ParametricBinding*>{},
        std::vector<SumVariant*>{wrap}, /*is_public=*/false);
    outer_name->set_definer(outer_def);
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(std::move(payload_type));
    std::vector<SumTypeVariant> outer_variants;
    outer_variants.push_back(
        SumTypeVariant::MakeTuple(*wrap, std::move(members)));
    current = std::make_unique<SumType>(*outer_def, std::move(outer_variants));
    current_def = outer_def;
    XLS_ASSERT_OK_AND_ASSIGN(internal::EncodedSumView value_view,
                             internal::GetEncodedSumView(value));
    XLS_ASSERT_OK_AND_ASSIGN(internal::EncodedSumView malformed_view,
                             internal::GetEncodedSumView(malformed));
    XLS_ASSERT_OK_AND_ASSIGN(InterpValue packed_value,
                             value_view.tag.Concat(value_view.payload_slot));
    XLS_ASSERT_OK_AND_ASSIGN(
        InterpValue packed_malformed,
        malformed_view.tag.Concat(malformed_view.payload_slot));
    value = internal::CreateEncodedSumTuple(InterpValue::MakeUBits(0, 0),
                                            std::move(packed_value));
    malformed = internal::CreateEncodedSumTuple(InterpValue::MakeUBits(0, 0),
                                                std::move(packed_malformed));
  }

  XLS_ASSERT_OK_AND_ASSIGN(Value ir_value, value.ConvertToIr());
  EXPECT_THAT(ValueToInterpValue(ir_value, current.get()),
              ::absl_testing::IsOk());
  XLS_ASSERT_OK_AND_ASSIGN(Value malformed_ir_value, malformed.ConvertToIr());
  EXPECT_THAT(ValueToInterpValue(malformed_ir_value, current.get()),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("No variant with tag bits")));
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
  auto* some = module.Make<SumVariant>(
      kFakeSpan, some_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, enum_def),
          std::vector<ExprOrType>{})},
      std::vector<StructMemberNode*>{});
  auto* none = module.Make<SumVariant>(
      kFakeSpan, none_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
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

TEST(InterpValueHelpersTest, SignConvertValueReifiesRawSumEnumPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  EnumDef* enum_def = nullptr;
  SumType sum_type = MakeEnumPayloadSumType(module, &enum_def);

  InterpValue raw_value = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(/*bit_count=*/1, /*value=*/0),
       InterpValue::MakeTuple(
           {InterpValue::MakeUBits(/*bit_count=*/2, /*value=*/1)})});
  InterpValue enum_value =
      InterpValue::MakeEnum(UBits(/*value=*/1, /*bit_count=*/2),
                            /*is_signed=*/false, enum_def);
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(sum_type, "Some", std::vector<InterpValue>{enum_value}));

  EXPECT_THAT(SignConvertValue(sum_type, raw_value),
              IsOkAndHolds(Eq(expected)));
}

TEST(InterpValueHelpersTest,
     SignConvertValuePreservesInactiveEmptySumPlaceholder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyPayloadType(module);

  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue value,
      CreateSumValue(outer_type, "Nothing", no_payload_values));
  EXPECT_THAT(SignConvertValue(outer_type, value), IsOkAndHolds(Eq(value)));
}

TEST(InterpValueHelpersTest,
     SignConvertValuePreservesInactiveEmptyEnumPlaceholder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyEnumPayloadType(module);

  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue value, CreateSumValue(outer_type, "Unit", no_payload_values));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue converted,
                           SignConvertValue(outer_type, value));
  EXPECT_TRUE(converted.Eq(value));
  EXPECT_EQ(converted.GetValuesOrDie().at(1).GetValuesOrDie().at(0).tag(),
            InterpValueTag::kUBits);
}

TEST(InterpValueHelpersTest, SignConvertValuePreservesMalformedSumShape) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  InterpValue malformed = InterpValue::MakeTuple(
      {InterpValue::MakeUBits(/*bit_count=*/2, /*value=*/3),
       InterpValue::MakeTuple(
           {InterpValue::MakeUBits(/*bit_count=*/16, /*value=*/0xbeef)})});

  EXPECT_THAT(SignConvertValue(sum_type, malformed),
              IsOkAndHolds(Eq(malformed)));
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
  EXPECT_THAT(
      CreateSumValue(sum_type, /*variant_index=*/0, {invalid_enum_value}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("declared member")));
}

TEST(InterpValueHelpersTest, CreateSumValueRejectsForeignNominalEnumPayload) {
  FileTable file_table;
  Module expected_module("expected", /*fs_path=*/std::nullopt, file_table);
  Module foreign_module("foreign", /*fs_path=*/std::nullopt, file_table);
  EnumDef* expected_enum = nullptr;
  EnumDef* foreign_enum = nullptr;
  SumType sum_type = MakeEnumPayloadSumType(expected_module, &expected_enum);
  MakeEnumPayloadSumType(foreign_module, &foreign_enum);
  ASSERT_NE(expected_enum, foreign_enum);

  InterpValue foreign_value = InterpValue::MakeEnum(
      UBits(/*value=*/1, /*bit_count=*/2), /*is_signed=*/false, foreign_enum);
  EXPECT_THAT(CreateSumValue(sum_type, /*variant_index=*/0, {foreign_value}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match enum")));
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

TEST(InterpValueHelpersTest, UnflattenUsesOneSumPayloadAndPreservesRawBits) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  const auto packed_sum = [](int64_t tag, int64_t payload) {
    return internal::CreateEncodedSumTuple(InterpValue::MakeUBits(2, tag),
                                           InterpValue::MakeUBits(16, payload));
  };
  EXPECT_THAT(internal::UnflattenValueForType(sum_type, UBits(0, 18)),
              IsOkAndHolds(packed_sum(/*tag=*/0, /*payload=*/0)));
  // The Byte constructor uses only eight bits; its existing padding survives.
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue byte, internal::UnflattenValueForType(
                                                 sum_type, UBits(0x1ab5a, 18)));
  EXPECT_EQ(byte, packed_sum(/*tag=*/1, /*payload=*/0xab5a));
  XLS_ASSERT_OK_AND_ASSIGN(internal::EncodedSumView byte_view,
                           internal::GetEncodedSumView(byte));
  EXPECT_TRUE(byte_view.tag.IsUBits());
  EXPECT_TRUE(byte_view.payload_slot.IsUBits());
  EXPECT_THAT(internal::UnflattenValueForType(sum_type, UBits(0x2beef, 18)),
              IsOkAndHolds(packed_sum(/*tag=*/2, /*payload=*/0xbeef)));
  // Decoding also transports undeclared tags without observing them.
  EXPECT_THAT(internal::UnflattenValueForType(sum_type, UBits(0x3ffff, 18)),
              IsOkAndHolds(packed_sum(/*tag=*/3, /*payload=*/0xffff)));
  EXPECT_THAT(internal::UnflattenValueForType(sum_type, UBits(0, 17)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 18 bits; got 17")));
  EXPECT_THAT(internal::UnflattenValueForType(sum_type, UBits(0, 19)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected 18 bits; got 19")));
}

TEST(InterpValueHelpersTest, UnflattenNestedSumTupleIsMostSignificantFirst) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(std::make_unique<BitsType>(/*is_signed=*/true, 4));
  members.push_back(sum_type.CloneToUnique());
  members.push_back(std::make_unique<BitsType>(/*is_signed=*/false, 1));
  TupleType tuple(std::move(members));

  // The tuple stores [s4, tag2, payload16, u1], from MSB to LSB.
  const Bits bits = UBits((0xdu << 19) | (1u << 17) | (0xa5u << 1) | 1u, 23);
  InterpValue expected = InterpValue::MakeTuple(
      {InterpValue::MakeSBits(4, -3),
       internal::CreateEncodedSumTuple(InterpValue::MakeUBits(2, 1),
                                       InterpValue::MakeUBits(16, 0xa5)),
       InterpValue::MakeBool(true)});
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue actual,
                           internal::UnflattenValueForType(tuple, bits));
  EXPECT_EQ(actual, expected);
  EXPECT_TRUE(actual.GetValuesOrDie().front().IsSBits());
}

TEST(InterpValueHelpersTest, UnflattenNestedSumArrayIsLeastSignificantFirst) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);
  ArrayType array(sum_type.CloneToUnique(), TypeDim::CreateU32(2));

  // Each element is [tag2, payload16]; element zero occupies the low 18 bits.
  const Bits bits = UBits((uint64_t{2} << 34) | (uint64_t{0xbeef} << 18) |
                              (uint64_t{1} << 16) | 0x5a,
                          36);
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      InterpValue::MakeArray(
          {internal::CreateEncodedSumTuple(InterpValue::MakeUBits(2, 1),
                                           InterpValue::MakeUBits(16, 0x5a)),
           internal::CreateEncodedSumTuple(
               InterpValue::MakeUBits(2, 2),
               InterpValue::MakeUBits(16, 0xbeef))}));
  EXPECT_THAT(internal::UnflattenValueForType(array, bits),
              IsOkAndHolds(expected));
}

TEST(InterpValueHelpersTest, UnflattenOrdinaryLeavesPreservesTheirType) {
  const InterpValue unsigned_value = InterpValue::MakeUBits(8, 0x5a);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue flat, unsigned_value.Flatten());
  EXPECT_THAT(
      internal::UnflattenValueForType(*BitsType::MakeU8(), flat.GetBitsOrDie()),
      IsOkAndHolds(unsigned_value));
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue token, internal::UnflattenValueForType(TokenType(), Bits(0)));
  EXPECT_TRUE(token.IsToken());

  EnumDef enum_def(/*owner=*/nullptr, /*span=*/Span::Fake(),
                   /*name_def=*/nullptr, /*type=*/{}, /*values=*/{},
                   /*is_public=*/false);
  EnumType enum_type(enum_def, TypeDim::CreateU32(2), /*is_signed=*/true, {});
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue enum_value,
      internal::UnflattenValueForType(enum_type, UBits(3, 2)));
  std::optional<InterpValue::EnumData> enum_data = enum_value.GetEnumData();
  ASSERT_TRUE(enum_data.has_value());
  EXPECT_EQ(enum_data->value, UBits(3, 2));
  EXPECT_TRUE(enum_data->is_signed);
  EXPECT_EQ(enum_data->def, &enum_def);
}

TEST(InterpValueHelpersTest, GetLeafChannelReferences) {
  InterpValue ch0 = InterpValue::MakeChannelReference(ChannelDirection::kIn, 0);
  InterpValue ch1 = InterpValue::MakeChannelReference(ChannelDirection::kIn, 1);
  InterpValue ch2 = InterpValue::MakeChannelReference(ChannelDirection::kIn, 2);
  InterpValue ch3 = InterpValue::MakeChannelReference(ChannelDirection::kIn, 3);

  InterpValue sub_arr0 = InterpValue::MakeChannelArray(
      ChannelDirection::kIn, 10, /*definer=*/nullptr, {ch0, ch1});
  InterpValue sub_arr1 = InterpValue::MakeChannelArray(
      ChannelDirection::kIn, 11, /*definer=*/nullptr, {ch2, ch3});
  InterpValue arr2d = InterpValue::MakeChannelArray(
      ChannelDirection::kIn, 12, /*definer=*/nullptr, {sub_arr0, sub_arr1});

  EXPECT_THAT(GetLeafChannelReferences(ch0), testing::ElementsAre(ch0));
  EXPECT_THAT(GetLeafChannelReferences(sub_arr0),
              testing::ElementsAre(ch0, ch1));
  EXPECT_THAT(GetLeafChannelReferences(arr2d),
              testing::ElementsAre(ch0, ch1, ch2, ch3));
}

TEST(InterpValueHelpersTest, ValueToInterpValueSumPreservesMalformedTag) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  Value raw =
      Value::Tuple({Value(UBits(3, 2)), Value::Tuple({Value(UBits(0, 16))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue actual,
                           ValueToInterpValue(raw, &sum_type));
  EXPECT_TRUE(InterpValue::MakeTuple(
                  {InterpValue::MakeUBits(/*bit_count=*/2, /*value=*/3),
                   InterpValue::MakeTuple({InterpValue::MakeUBits(
                       /*bit_count=*/16, /*value=*/0)})})
                  .Eq(actual));
  EXPECT_THAT(GetSumPayloadValues(sum_type, actual),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("No variant with tag bits")));
}

TEST(InterpValueHelpersTest, ValueToInterpValueSumRejectsMalformedRawShape) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  Value narrow_tag =
      Value::Tuple({Value(UBits(0, 1)), Value::Tuple({Value(UBits(0, 16))})});
  EXPECT_THAT(ValueToInterpValue(narrow_tag, &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("expected a 2-bit tag")));
  Value non_bits_tag =
      Value::Tuple({Value::Tuple({}), Value::Tuple({Value(UBits(0, 16))})});
  EXPECT_THAT(
      ValueToInterpValue(non_bits_tag, &sum_type),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("unsigned bits")));
  InterpValue signed_tag = internal::CreateEncodedSumTuple(
      InterpValue::MakeSBits(2, 0), InterpValue::MakeUBits(16, 0));
  EXPECT_THAT(
      SignConvertValue(sum_type, signed_tag),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("unsigned bits")));

  EXPECT_THAT(ValueToInterpValue(Value(UBits(0, 2)), &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("tag and a payload tuple")));
  EXPECT_THAT(ValueToInterpValue(Value::Tuple({Value(UBits(0, 2))}), &sum_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("tag and a payload tuple")));
  EXPECT_THAT(
      ValueToInterpValue(Value::Tuple({Value(UBits(0, 2)), Value(UBits(0, 8))}),
                         &sum_type),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("tag and a payload tuple")));
  EXPECT_THAT(
      ValueToInterpValue(Value::Tuple({Value(UBits(0, 2)), Value::Tuple({})}),
                         &sum_type),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("must contain 1 payload slots")));
  EXPECT_THAT(
      ValueToInterpValue(Value::Tuple({Value(UBits(0, 3)),
                                       Value::Tuple({Value(UBits(0, 16))})}),
                         &sum_type),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("expected a 2-bit tag; got 3 bits")));
  EXPECT_THAT(
      ValueToInterpValue(Value::Tuple({Value(UBits(0, 2)),
                                       Value::Tuple({Value(UBits(0, 8))})}),
                         &sum_type),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("expected a 16-bit payload slot; got 8 bits")));
}

TEST(InterpValueHelpersTest, ValueToInterpValueRejectsMalformedTypedAggregate) {
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  TupleType tuple_type(std::move(members));

  EXPECT_THAT(
      ValueToInterpValue(Value::Tuple({Value(UBits(0, 8)), Value(UBits(0, 8))}),
                         &tuple_type),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("expected 1 elements; got 2")));
  XLS_ASSERT_OK_AND_ASSIGN(Value raw_array, Value::Array({Value(UBits(0, 8))}));
  EXPECT_THAT(ValueToInterpValue(raw_array, &tuple_type),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("does not match expected type")));
}

TEST(InterpValueHelpersTest,
     ValueToInterpValueSumRestoresLaterVariantPackedPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(sum_type, "Wide", {InterpValue::MakeUBits(16, 42)}));
  XLS_ASSERT_OK_AND_ASSIGN(Value raw, expected.ConvertToIr());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue actual,
                           ValueToInterpValue(raw, &sum_type));

  const std::vector<InterpValue>& payload_slots =
      actual.GetValuesOrDie().at(1).GetValuesOrDie();
  ASSERT_EQ(payload_slots.size(), 1);
  EXPECT_EQ(payload_slots.at(0).GetBitValueUnsigned().value(), 42);
  EXPECT_TRUE(actual.Eq(expected));
}

TEST(InterpValueHelpersTest,
     ValueToInterpValueSumAcceptsInactiveEmptySumPlaceholder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyPayloadType(module);

  Value raw =
      Value::Tuple({Value(UBits(1, 1)), Value::Tuple({Value(UBits(0, 0))})});
  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(outer_type, "Nothing", no_payload_values));
  EXPECT_THAT(ValueToInterpValue(raw, &outer_type), IsOkAndHolds(Eq(expected)));
}

// Verifies: Inactive enum payload bits remain opaque during raw conversion.
// Catches: Validating or rewriting storage unused by the selected variant.
TEST(InterpValueHelpersTest,
     ValueToInterpValueSumIgnoresUnusedEnumPayloadBits) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInactiveEmptyEnumPayloadType(module);

  Value raw =
      Value::Tuple({Value(UBits(0, 1)), Value::Tuple({Value(UBits(3, 2))})});
  const std::vector<InterpValue> no_payload_values;
  XLS_ASSERT_OK_AND_ASSIGN(
      InterpValue expected,
      CreateSumValue(outer_type, "Unit", no_payload_values));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue actual,
                           ValueToInterpValue(raw, &outer_type));
  EXPECT_FALSE(actual.Eq(expected));
  EXPECT_THAT(actual.ConvertToIr(), IsOkAndHolds(raw));
  EXPECT_EQ(actual.GetValuesOrDie().at(1).GetValuesOrDie().at(0).tag(),
            InterpValueTag::kUBits);
}

// Verifies: Unit-variant padding survives conversion and is semantically valid.
// Catches: Raw conversion rebuilding values or requiring zero inactive bits.
TEST(InterpValueHelpersTest, ValueToInterpValueSumPreservesUnusedPayloadBits) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeMixedPayloadSumType(module);

  Value raw = Value::Tuple(
      {Value(UBits(0, 2)), Value::Tuple({Value(UBits(0xffff, 16))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue actual,
                           ValueToInterpValue(raw, &sum_type));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue expected,
                           CreateSumValue(sum_type, "None", {}));
  EXPECT_FALSE(actual.Eq(expected));
  EXPECT_THAT(actual.ConvertToIr(), IsOkAndHolds(raw));
  XLS_ASSERT_OK(ValidateInterpValueMatchesType(actual, sum_type));
  EXPECT_THAT(GetSumPayloadValues(sum_type, actual),
              IsOkAndHolds(::testing::IsEmpty()));
}

}  // namespace
}  // namespace xls::dslx
