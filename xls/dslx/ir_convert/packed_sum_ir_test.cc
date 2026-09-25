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

#include "xls/dslx/ir_convert/packed_sum_ir.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/function.h"
#include "xls/ir/function_base.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/package.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"

namespace xls::dslx::internal {
namespace {

class PackedSumIrTest : public ::testing::Test {
 protected:
  using Members = std::vector<std::unique_ptr<Type>>;

  static Members Payload(std::unique_ptr<Type> first,
                         std::unique_ptr<Type> second = nullptr) {
    Members result;
    result.push_back(std::move(first));
    if (second != nullptr) {
      result.push_back(std::move(second));
    }
    return result;
  }

  std::unique_ptr<SumType> Choice(Members first, Members second,
                                  std::vector<InterpValue> tags = {}) {
    const Span span = Span::Fake();
    auto* annotation = module_.Make<BuiltinTypeAnnotation>(
        span, BuiltinType::kU8,
        module_.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
    auto make_variant = [&](const char* name, int64_t count) {
      return module_.Make<SumVariant>(
          span, module_.Make<NameDef>(span, name, nullptr),
          count == 0 ? SumVariant::PayloadShape::kUnit
                     : SumVariant::PayloadShape::kTuple,
          std::vector<TypeAnnotation*>(count, annotation),
          std::vector<StructMemberNode*>{});
    };
    auto* left = make_variant("First", first.size());
    auto* right = make_variant("Last", second.size());
    auto* def =
        module_.Make<SumDef>(span, module_.Make<NameDef>(span, "S", nullptr),
                             std::vector<ParametricBinding*>{},
                             std::vector<SumVariant*>{left, right}, false);
    def->name_def()->set_definer(def);
    auto make_type = [](SumVariant* variant, Members members) {
      if (members.empty()) {
        return SumTypeVariant::MakeUnit(*variant);
      } else {
        return SumTypeVariant::MakeTuple(*variant, std::move(members));
      }
    };
    std::vector<SumTypeVariant> variants;
    variants.push_back(make_type(left, std::move(first)));
    variants.push_back(make_type(right, std::move(second)));
    return std::make_unique<SumType>(*def, std::move(variants),
                                     TypeDim::CreateU32(tags.empty() ? 1 : 4),
                                     std::move(tags));
  }

  static Value Raw(uint64_t tag, int64_t tag_width, uint64_t slot,
                   int64_t slot_width) {
    return Value::Tuple({Value(UBits(tag, tag_width)),
                         Value::Tuple({Value(UBits(slot, slot_width))})});
  }

  absl::StatusOr<Value> Run(BValue result) {
    XLS_ASSIGN_OR_RETURN(xls::Function * function,
                         builder_.BuildWithReturnValue(result));
    XLS_ASSIGN_OR_RETURN(auto interpreted, InterpretFunction(function, {}));
    return interpreted.value;
  }

  FileTable file_table_;
  Module module_{"test", std::nullopt, file_table_};
  Package package_{"p"};
  FunctionBuilder builder_{"f", &package_};
};

TEST_F(PackedSumIrTest, ConstructsSignedSparseTagsAndZeroPadding) {
  auto sum =
      Choice(Payload(BitsType::MakeU8()),
             Payload(std::make_unique<BitsType>(false, 3)),
             {InterpValue::MakeSBits(4, -2), InterpValue::MakeSBits(4, 3)});
  const SumTypeEncoding encoding(*sum);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(auto last, encoding.GetVariant("Last"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue a, BuildPackedSumValue(builder_, *sum, first,
                                    {builder_.Literal(UBits(0xa5, 8))}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue b, BuildPackedSumValue(builder_, *sum, last,
                                    {builder_.Literal(UBits(5, 3))}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result, Run(builder_.Tuple({a, b})));
  EXPECT_EQ(result, Value::Tuple({Raw(14, 4, 0xa5, 8), Raw(3, 4, 5, 8)}));
}

TEST_F(PackedSumIrTest, RejectsInactiveOverflowBeforePackingActiveArray) {
  auto sum = Choice(Payload(std::make_unique<ArrayType>(BitsType::MakeU8(),
                                                        TypeDim::CreateU32(3))),
                    Payload(std::make_unique<ArrayType>(
                        std::make_unique<BitsType>(false, 1'000'000),
                        TypeDim::CreateU32(5'000))));
  XLS_ASSERT_OK_AND_ASSIGN(auto first,
                           SumTypeEncoding(*sum).GetVariant("First"));
  BValue value = builder_.Param(
      "value", package_.GetArrayType(3, package_.GetBitsType(8)));
  const int64_t initial_nodes = builder_.function()->node_count();

  const auto result = BuildPackedSumValue(builder_, *sum, first, {value}, {});
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(
      result.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);
}

TEST_F(PackedSumIrTest, RejectsCombinedTagAndSlotOverflowBeforeBuildingNodes) {
  auto sum = Choice(Payload(BitsType::MakeU8()),
                    Payload(std::make_unique<BitsType>(false, 4'294'967'295)));
  XLS_ASSERT_OK_AND_ASSIGN(auto first,
                           SumTypeEncoding(*sum).GetVariant("First"));
  BValue value = builder_.Param("value", package_.GetBitsType(8));
  const int64_t initial_nodes = builder_.function()->node_count();

  const auto result = BuildPackedSumValue(builder_, *sum, first, {value}, {});
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(
      result.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);
}

TEST_F(PackedSumIrTest,
       ComparesActiveBitsAndInvalidTagsWithoutInspectingPadding) {
  auto sum =
      Choice(Payload(BitsType::MakeU8()),
             Payload(std::make_unique<BitsType>(false, 3)),
             {InterpValue::MakeSBits(4, -2), InterpValue::MakeSBits(4, 3)});
  auto equal = [&](uint64_t lhs_tag, uint64_t lhs, uint64_t rhs_tag,
                   uint64_t rhs) {
    return BuildPackedSumEquality(
        builder_, *sum, builder_.Literal(Raw(lhs_tag, 4, lhs, 8)),
        builder_.Literal(Raw(rhs_tag, 4, rhs, 8)), {});
  };
  XLS_ASSERT_OK_AND_ASSIGN(BValue padding, equal(3, 0x85, 3, 5));
  XLS_ASSERT_OK_AND_ASSIGN(BValue active, equal(14, 0x85, 14, 5));
  XLS_ASSERT_OK_AND_ASSIGN(BValue different_tags, equal(3, 5, 4, 5));
  XLS_ASSERT_OK_AND_ASSIGN(BValue invalid_padding, equal(7, 0x85, 7, 5));
  XLS_ASSERT_OK_AND_ASSIGN(BValue invalid_active, equal(7, 5, 7, 6));
  XLS_ASSERT_OK_AND_ASSIGN(
      Value result, Run(builder_.Tuple({padding, active, different_tags,
                                        invalid_padding, invalid_active})));
  EXPECT_EQ(result, Value::Tuple({Value::Bool(true), Value::Bool(false),
                                  Value::Bool(false), Value::Bool(true),
                                  Value::Bool(false)}));
}

TEST_F(PackedSumIrTest, ProjectsNestedSharedPayloadAndIgnoresInactivePayload) {
  auto inner = Choice(Payload(BitsType::MakeU8()),
                      Payload(std::make_unique<BitsType>(false, 3)));
  auto tuple = TupleType::Create2(inner->CloneToUnique(), BitsType::MakeU8());
  auto outer = Choice(Payload(tuple->CloneToUnique()), {});
  XLS_ASSERT_OK_AND_ASSIGN(int64_t width, GetPackedSumBitCount(*tuple));
  EXPECT_EQ(width, 17);
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, *tuple,
                             builder_.Literal(UBits(0x18555, 17)), {}));
  const SumTypeEncoding encoding(*outer);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(auto last, encoding.GetVariant("Last"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue constructed,
      BuildPackedSumValue(builder_, *outer, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(BValue unit,
                           BuildPackedSumValue(builder_, *outer, last, {}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue equal,
      BuildPackedSumEquality(builder_, *outer, constructed,
                             builder_.Literal(Raw(0, 1, 0x10555, 17)), {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue inactive,
      BuildPackedSumEquality(builder_, *outer, unit,
                             builder_.Literal(Raw(1, 1, 0x1ffff, 17)), {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      Value result,
      Run(builder_.Tuple({unpacked, constructed, unit, equal, inactive})));
  EXPECT_EQ(
      result,
      Value::Tuple({Value::Tuple({Raw(1, 1, 0x85, 8), Value(UBits(0x55, 8))}),
                    Raw(0, 1, 0x18555, 17), Raw(1, 1, 0, 17), Value::Bool(true),
                    Value::Bool(true)}));
}

TEST_F(PackedSumIrTest, ConcreteBitsConstructorUsesScalarBits) {
  ArrayType bits(
      std::make_unique<BitsConstructorType>(TypeDim::CreateBool(true)),
      TypeDim::CreateU32(5));
  auto sum = Choice(Payload(bits.CloneToUnique()), {});
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, bits, builder_.Literal(UBits(0x1e, 5)),
                             {}));
  XLS_ASSERT_OK_AND_ASSIGN(auto first,
                           SumTypeEncoding(*sum).GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue constructed,
      BuildPackedSumValue(builder_, *sum, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({unpacked, constructed})));
  EXPECT_EQ(result, Value::Tuple({Value(UBits(0x1e, 5)), Raw(0, 1, 0x1e, 5)}));
}

TEST_F(PackedSumIrTest, ArraysUsePackedElementTypesAndIndexZeroIsLow) {
  auto inner = Choice(Payload(BitsType::MakeU8()), Payload(BitsType::MakeU8()));
  ArrayType array(inner->CloneToUnique(), TypeDim::CreateU32(2));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, array,
                             builder_.Literal(UBits((0x134 << 9) | 0x12, 18)),
                             {}));
  auto outer = Choice(Payload(array.CloneToUnique()), {});
  XLS_ASSERT_OK_AND_ASSIGN(auto first,
                           SumTypeEncoding(*outer).GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue repacked,
      BuildPackedSumValue(builder_, *outer, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({unpacked, repacked})));
  EXPECT_EQ(
      result,
      Value::Tuple({Value::ArrayOrDie({Raw(0, 1, 0x12, 8), Raw(1, 1, 0x34, 8)}),
                    Raw(0, 1, (0x134 << 9) | 0x12, 18)}));
}

TEST_F(PackedSumIrTest, EmptyArrayDoesNotConstructRepresentativeElements) {
  auto inner = Choice(Payload(BitsType::MakeU8()), Payload(BitsType::MakeU8()));
  ArrayType empty(std::make_unique<ArrayType>(inner->CloneToUnique(),
                                              TypeDim::CreateU32(65536)),
                  TypeDim::CreateU32(0));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * projected,
                           GetPackedSumIrType(package_, empty));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked, UnpackPackedSumPayload(
                           builder_, empty, builder_.Literal(UBits(0, 0)), {}));
  auto* packed_element =
      package_.GetTupleType({package_.GetBitsType(1),
                             package_.GetTupleType({package_.GetBitsType(8)})});
  EXPECT_EQ(projected, package_.GetArrayType(
                           0, package_.GetArrayType(65536, packed_element)));
  EXPECT_EQ(unpacked.GetType(), projected);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           builder_.BuildWithReturnValue(unpacked));
  EXPECT_EQ(function->node_count(), 2);
}

}  // namespace
}  // namespace xls::dslx::internal
