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
#include "xls/ir/nodes.h"
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

  std::unique_ptr<SumType> EmptySum(uint32_t tag_width) {
    const Span span = Span::Fake();
    auto* def = module_.Make<SumDef>(
        span, module_.Make<NameDef>(span, "Empty", nullptr),
        std::vector<ParametricBinding*>{}, std::vector<SumVariant*>{}, false);
    def->name_def()->set_definer(def);
    return std::make_unique<SumType>(*def, std::vector<SumTypeVariant>{},
                                     TypeDim::CreateU32(tag_width));
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

  auto NoArrayElements() {
    return [this](const Type& type) -> absl::StatusOr<xls::Type*> {
      ADD_FAILURE() << "Unexpected array element type: " << type.ToString();
      return package_.GetBitsType(0);
    };
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
      BValue a, BuildPackedSumValue(builder_, encoding, first,
                                    {builder_.Literal(UBits(0xa5, 8))}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue b, BuildPackedSumValue(builder_, encoding, last,
                                    {builder_.Literal(UBits(5, 3))}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result, Run(builder_.Tuple({a, b})));
  EXPECT_EQ(result, Value::Tuple({Raw(14, 4, 0xa5, 8), Raw(3, 4, 5, 8)}));
}

TEST_F(PackedSumIrTest, PacksAndComparesVariantMembersInDeclaredBitOrder) {
  auto inner = Choice(Payload(std::make_unique<BitsType>(false, 2)),
                      Payload(std::make_unique<BitsType>(false, 4)));
  auto outer = Choice(
      Payload(inner->CloneToUnique(), std::make_unique<BitsType>(false, 3)),
      Payload(std::make_unique<BitsType>(false, 12)));
  const SumTypeEncoding encoding(*outer);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue constructed,
      BuildPackedSumValue(
          builder_, encoding, first,
          {builder_.Literal(Raw(0, 1, 2, 4)), builder_.Literal(UBits(5, 3))},
          {}));
  // The first member is the five-bit inner sum; the last is the three-bit
  // value. Set both outer padding and the inner sum's ignored high bits.
  BValue dirty = builder_.Literal(Raw(0, 1, 0xf75, 12));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue equal,
      BuildPackedSumEquality(builder_, *outer, constructed, dirty, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue different_first,
      BuildPackedSumEquality(builder_, *outer, constructed,
                             builder_.Literal(Raw(0, 1, 0xf7d, 12)), {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue different_last,
      BuildPackedSumEquality(builder_, *outer, constructed,
                             builder_.Literal(Raw(0, 1, 0xf74, 12)), {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      Value result,
      Run(builder_.Tuple({constructed, GetPackedSumRawBits(builder_, dirty, {}),
                          equal, different_first, different_last})));
  EXPECT_EQ(result, Value::Tuple({Raw(0, 1, 0x15, 12), Value(UBits(0xf75, 13)),
                                  Value::Bool(true), Value::Bool(false),
                                  Value::Bool(false)}));
}

TEST_F(PackedSumIrTest, RejectsVariantFromAnotherSumBeforeBuildingNodes) {
  auto sum =
      Choice(Payload(BitsType::MakeU8()), {},
             {InterpValue::MakeUBits(4, 1), InterpValue::MakeUBits(4, 2)});
  auto other_sum =
      Choice(Payload(BitsType::MakeU8()),
             Payload(std::make_unique<BitsType>(false, 12)),
             {InterpValue::MakeUBits(4, 6), InterpValue::MakeUBits(4, 9)});
  const SumTypeEncoding encoding(*sum);
  const SumTypeEncoding other_encoding(*other_sum);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, other_encoding.GetVariant("First"));
  BValue value = builder_.Literal(UBits(0xa5, 8));
  const int64_t initial_nodes = builder_.function()->node_count();

  const auto result =
      BuildPackedSumValue(builder_, encoding, first, {value}, {});
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(),
              ::testing::HasSubstr("does not belong to sum"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);

  XLS_ASSERT_OK_AND_ASSIGN(
      BValue other,
      BuildPackedSumValue(builder_, other_encoding, first, {value}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value other_result, Run(other));
  EXPECT_EQ(other_result, Raw(6, 4, 0xa5, 12));
}

TEST_F(PackedSumIrTest, RejectsInactiveOverflowBeforePackingActiveArray) {
  auto sum = Choice(Payload(std::make_unique<ArrayType>(BitsType::MakeU8(),
                                                        TypeDim::CreateU32(3))),
                    Payload(std::make_unique<ArrayType>(
                        std::make_unique<BitsType>(false, 1'000'000),
                        TypeDim::CreateU32(5'000))));
  const SumTypeEncoding encoding(*sum);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  BValue value = builder_.Param(
      "value", package_.GetArrayType(3, package_.GetBitsType(8)));
  const int64_t initial_nodes = builder_.function()->node_count();

  const auto result =
      BuildPackedSumValue(builder_, encoding, first, {value}, {});
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(
      result.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);
}

TEST_F(PackedSumIrTest, RejectsCombinedTagAndSlotOverflowBeforeBuildingNodes) {
  auto sum = Choice(Payload(BitsType::MakeU8()),
                    Payload(std::make_unique<BitsType>(false, 4'294'967'295)));
  const SumTypeEncoding encoding(*sum);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  BValue value = builder_.Param("value", package_.GetBitsType(8));
  const int64_t initial_nodes = builder_.function()->node_count();

  const auto result =
      BuildPackedSumValue(builder_, encoding, first, {value}, {});
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(
      result.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);
}

TEST_F(PackedSumIrTest, RejectsPackedArrayTotalOverflowBeforeUnpacking) {
  constexpr int64_t kHalfWidth = 2'147'483'648;
  auto element =
      Choice(Payload(std::make_unique<BitsType>(false, kHalfWidth - 1)), {});
  ArrayType oversized(element->CloneToUnique(), TypeDim::CreateU32(2));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t element_width,
                           GetPackedSumBitCount(*element));
  ASSERT_EQ(element_width, kHalfWidth);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * element_ir,
                           GetPackedSumIrType(package_, *element));
  BValue bits =
      builder_.Param("oversized", package_.GetBitsType(2 * kHalfWidth));
  const int64_t initial_nodes = builder_.function()->node_count();
  int64_t resolver_calls = 0;
  const auto rejected = UnpackPackedSumPayload(builder_, oversized, bits,
                                               [&](const Type&) {
                                                 ++resolver_calls;
                                                 return element_ir;
                                               },
                                               {});
  EXPECT_FALSE(rejected.ok());
  EXPECT_THAT(
      rejected.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), initial_nodes);
  EXPECT_EQ(resolver_calls, 0);

  auto largest_element = Choice(
      Payload(std::make_unique<BitsType>(false, 2 * kHalfWidth - 2)), {});
  ArrayType largest(largest_element->CloneToUnique(), TypeDim::CreateU32(1));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t largest_width,
                           GetPackedSumBitCount(largest));
  ASSERT_EQ(largest_width, 2 * kHalfWidth - 1);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * largest_element_ir,
                           GetPackedSumIrType(package_, *largest_element));
  BValue largest_bits =
      builder_.Param("largest", package_.GetBitsType(largest_width));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, largest, largest_bits,
                             [&](const Type&) { return largest_element_ir; },
                             {}));
  EXPECT_EQ(unpacked.GetType(), package_.GetArrayType(1, largest_element_ir));
  XLS_EXPECT_OK(builder_.BuildWithReturnValue(unpacked));
}

TEST_F(PackedSumIrTest, RejectsDirectSumTotalOverflowBeforeComparing) {
  constexpr int64_t kMaximumWidth = 4'294'967'295;
  auto compare = [&](int64_t payload_width, const char* name) {
    auto sum =
        Choice(Payload(std::make_unique<BitsType>(false, payload_width)), {});
    xls::Type* carrier = package_.GetTupleType(
        {package_.GetBitsType(1),
         package_.GetTupleType({package_.GetBitsType(payload_width)})});
    BValue value = builder_.Param(name, carrier);
    return BuildPackedSumEquality(builder_, *sum, value, value, {});
  };
  const int64_t before = builder_.function()->node_count();
  const auto rejected = compare(kMaximumWidth, "oversized");
  EXPECT_FALSE(rejected.ok());
  EXPECT_THAT(
      rejected.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  EXPECT_EQ(builder_.function()->node_count(), before + 1);

  XLS_ASSERT_OK_AND_ASSIGN(BValue equal, compare(kMaximumWidth - 1, "largest"));
  EXPECT_EQ(equal.GetType(), package_.GetBitsType(1));
  XLS_EXPECT_OK(builder_.BuildWithReturnValue(equal));
}

TEST_F(PackedSumIrTest, ComparesOversizedOrdinaryStructureWithoutFlattening) {
  constexpr int64_t kHalfWidth = 2'147'483'648;
  auto ordinary =
      TupleType::Create2(std::make_unique<BitsType>(false, kHalfWidth),
                         std::make_unique<BitsType>(false, kHalfWidth));
  auto tiny = Choice({}, {});
  auto mixed =
      TupleType::Create2(ordinary->CloneToUnique(), tiny->CloneToUnique());
  xls::Type* ordinary_ir = package_.GetTupleType(
      {package_.GetBitsType(kHalfWidth), package_.GetBitsType(kHalfWidth)});
  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * tiny_ir,
                           GetPackedSumIrType(package_, *tiny));
  BValue plain = builder_.Param("plain", ordinary_ir);
  BValue structured = builder_.Param(
      "structured", package_.GetTupleType({ordinary_ir, tiny_ir}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue plain_equal,
      BuildPackedSumEquality(builder_, *ordinary, plain, plain, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue mixed_equal,
      BuildPackedSumEquality(builder_, *mixed, structured, structured, {}));
  XLS_EXPECT_OK(builder_.BuildWithReturnValue(
      builder_.Tuple({plain_equal, mixed_equal})));
}

TEST_F(PackedSumIrTest, EmptyArrayEqualityRejectsIntrinsicallyOversizedSum) {
  constexpr int64_t kMaximumWidth = 4'294'967'295;
  constexpr int64_t kHalfRange = 2'147'483'648;
  const auto overflow =
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits");
  auto invalid_sum =
      Choice(Payload(std::make_unique<BitsType>(false, kMaximumWidth)), {});
  const ArrayType invalid_empty(invalid_sum->CloneToUnique(),
                                TypeDim::CreateU32(0));
  ASSERT_THAT(GetPackedSumBitCount(invalid_empty).status().message(), overflow);

  auto valid_sum =
      Choice(Payload(std::make_unique<BitsType>(false, kHalfRange - 1)), {});
  XLS_ASSERT_OK_AND_ASSIGN(int64_t valid_sum_width,
                           GetPackedSumBitCount(*valid_sum));
  ASSERT_EQ(valid_sum_width, kHalfRange);
  auto ordinary_tuple = TupleType::Create2(valid_sum->CloneToUnique(),
                                           valid_sum->CloneToUnique());
  ASSERT_THAT(GetPackedSumBitCount(*ordinary_tuple).status().message(),
              overflow);
  const ArrayType valid_empty(std::move(ordinary_tuple), TypeDim::CreateU32(0));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t valid_array_width,
                           GetPackedSumBitCount(valid_empty));
  ASSERT_EQ(valid_array_width, 0);

  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * valid_sum_ir,
                           GetPackedSumIrType(package_, *valid_sum));
  BValue valid_value =
      builder_.Array({}, package_.GetTupleType({valid_sum_ir, valid_sum_ir}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue valid_equal, BuildPackedSumEquality(builder_, valid_empty,
                                                 valid_value, valid_value, {}));
  ASSERT_TRUE(valid_equal.node()->Is<Literal>());
  EXPECT_EQ(valid_equal.node()->As<Literal>()->value(), Value::Bool(true));

  // Build the physical empty array directly: canonical DSLX-to-IR conversion
  // rejects its declared sum, and no payload value should ever be allocated.
  xls::Type* invalid_sum_ir = package_.GetTupleType(
      {package_.GetBitsType(1),
       package_.GetTupleType({package_.GetBitsType(kMaximumWidth)})});
  BValue invalid_value = builder_.Array({}, invalid_sum_ir);
  const auto invalid_equal = BuildPackedSumEquality(
      builder_, invalid_empty, invalid_value, invalid_value, {});
  EXPECT_FALSE(invalid_equal.ok());
  EXPECT_THAT(invalid_equal.status().message(), overflow);

  // IR interpretation cannot represent an empty array Value. If a broken
  // helper returns a value, inspect its literal to expose the erroneous true.
  if (invalid_equal.ok()) {
    ASSERT_TRUE(invalid_equal->node()->Is<Literal>());
    EXPECT_NE(invalid_equal->node()->As<Literal>()->value(), Value::Bool(true));
  }
  XLS_EXPECT_OK(builder_.BuildWithReturnValue(valid_equal));
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

TEST_F(PackedSumIrTest, ComparesRawTagsForSumWithNoVariants) {
  auto empty = EmptySum(2);
  auto equal = [&](uint64_t lhs, uint64_t rhs) {
    return BuildPackedSumEquality(builder_, *empty,
                                  builder_.Literal(Raw(lhs, 2, 0, 0)),
                                  builder_.Literal(Raw(rhs, 2, 0, 0)), {});
  };
  XLS_ASSERT_OK_AND_ASSIGN(BValue same, equal(1, 1));
  XLS_ASSERT_OK_AND_ASSIGN(BValue different, equal(1, 2));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({same, different})));
  EXPECT_EQ(result, Value::Tuple({Value::Bool(true), Value::Bool(false)}));
}

TEST_F(PackedSumIrTest, ComparesRawTagsForActiveNestedSumWithNoVariants) {
  auto outer = Choice(Payload(EmptySum(2)), {});
  auto equal = [&](uint64_t outer_tag, uint64_t lhs, uint64_t rhs) {
    return BuildPackedSumEquality(
        builder_, *outer, builder_.Literal(Raw(outer_tag, 1, lhs, 2)),
        builder_.Literal(Raw(outer_tag, 1, rhs, 2)), {});
  };
  XLS_ASSERT_OK_AND_ASSIGN(BValue same, equal(0, 1, 1));
  XLS_ASSERT_OK_AND_ASSIGN(BValue different, equal(0, 1, 2));
  XLS_ASSERT_OK_AND_ASSIGN(BValue inactive, equal(1, 1, 2));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({same, different, inactive})));
  EXPECT_EQ(result, Value::Tuple({Value::Bool(true), Value::Bool(false),
                                  Value::Bool(true)}));
}

TEST_F(PackedSumIrTest, ComparesEmptySumPayloadWithOversizedArrayElement) {
  auto oversized = std::make_unique<ArrayType>(
      std::make_unique<ArrayType>(BitsType::MakeU1(),
                                  TypeDim::CreateU32(65536)),
      TypeDim::CreateU32(65536));
  // The nested sum is valid; only the ordinary tuple around it is oversized.
  auto tiny = Choice({}, {});
  auto element = TupleType::Create2(std::move(oversized), std::move(tiny));
  auto empty =
      std::make_unique<ArrayType>(std::move(element), TypeDim::CreateU32(0));
  auto sum = Choice(Payload(std::move(empty)), {});
  XLS_ASSERT_OK_AND_ASSIGN(int64_t width, GetPackedSumBitCount(*sum));
  EXPECT_EQ(width, 1);

  BValue first = builder_.Literal(Raw(0, 1, 0, 0));
  BValue last = builder_.Literal(Raw(1, 1, 0, 0));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue same, BuildPackedSumEquality(builder_, *sum, first, first, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue different,
      BuildPackedSumEquality(builder_, *sum, first, last, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({same, different})));
  EXPECT_EQ(result, Value::Tuple({Value::Bool(true), Value::Bool(false)}));
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
                             builder_.Literal(UBits(0x18555, 17)),
                             NoArrayElements(), {}));
  const SumTypeEncoding encoding(*outer);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(auto last, encoding.GetVariant("Last"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue constructed,
      BuildPackedSumValue(builder_, encoding, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unit, BuildPackedSumValue(builder_, encoding, last, {}, {}));
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
                             NoArrayElements(), {}));
  const SumTypeEncoding encoding(*sum);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue constructed,
      BuildPackedSumValue(builder_, encoding, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({unpacked, constructed})));
  EXPECT_EQ(result, Value::Tuple({Value(UBits(0x1e, 5)), Raw(0, 1, 0x1e, 5)}));
}

TEST_F(PackedSumIrTest, ArraysUsePackedElementTypesAndIndexZeroIsLow) {
  auto inner = Choice(Payload(BitsType::MakeU8()), Payload(BitsType::MakeU8()));
  ArrayType array(inner->CloneToUnique(), TypeDim::CreateU32(2));
  xls::Type* packed_element =
      package_.GetTupleType({package_.GetBitsType(1),
                             package_.GetTupleType({package_.GetBitsType(8)})});
  XLS_ASSERT_OK_AND_ASSIGN(xls::Type * sum_type,
                           GetPackedSumIrType(package_, *inner));
  EXPECT_EQ(sum_type, packed_element);
  int64_t resolver_calls = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, array,
                             builder_.Literal(UBits((0x134 << 9) | 0x12, 18)),
                             [&](const Type& type) {
                               EXPECT_EQ(&type, &array.element_type());
                               ++resolver_calls;
                               return packed_element;
                             },
                             {}));
  EXPECT_EQ(resolver_calls, 1);
  auto outer = Choice(Payload(array.CloneToUnique()), {});
  const SumTypeEncoding encoding(*outer);
  XLS_ASSERT_OK_AND_ASSIGN(auto first, encoding.GetVariant("First"));
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue repacked,
      BuildPackedSumValue(builder_, encoding, first, {unpacked}, {}));
  XLS_ASSERT_OK_AND_ASSIGN(Value result,
                           Run(builder_.Tuple({unpacked, repacked})));
  EXPECT_EQ(
      result,
      Value::Tuple({Value::ArrayOrDie({Raw(0, 1, 0x12, 8), Raw(1, 1, 0x34, 8)}),
                    Raw(0, 1, (0x134 << 9) | 0x12, 18)}));
}

TEST_F(PackedSumIrTest, EmptyArrayDoesNotConstructRepresentativeElements) {
  // Each sum is representable, but two exceed the checked packed width. A wide
  // scalar keeps the IR element type small: each sum still has only two leaves.
  constexpr int64_t kPayloadWidth = 2'147'483'647;
  auto inner = Choice(Payload(std::make_unique<BitsType>(false, kPayloadWidth)),
                      Payload(BitsType::MakeU8()));
  ArrayType empty(std::make_unique<ArrayType>(inner->CloneToUnique(),
                                              TypeDim::CreateU32(2)),
                  TypeDim::CreateU32(0));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t inner_width, GetPackedSumBitCount(*inner));
  EXPECT_EQ(inner_width, kPayloadWidth + 1);
  const auto erased_width = GetPackedSumBitCount(empty.element_type());
  ASSERT_FALSE(erased_width.ok());
  EXPECT_THAT(
      erased_width.status().message(),
      ::testing::HasSubstr("shared sum bit count exceeds 4294967295 bits"));
  auto* payload_type = package_.GetBitsType(kPayloadWidth);
  auto* packed_element = package_.GetTupleType(
      {package_.GetBitsType(1), package_.GetTupleType({payload_type})});
  xls::Type* element_type = package_.GetArrayType(2, packed_element);
  EXPECT_EQ(element_type->leaf_count(), 4);
  int64_t resolver_calls = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, empty, builder_.Literal(UBits(0, 0)),
                             [&](const Type& type) {
                               EXPECT_EQ(&type, &empty.element_type());
                               ++resolver_calls;
                               return element_type;
                             },
                             {}));
  EXPECT_EQ(resolver_calls, 1);
  EXPECT_EQ(unpacked.GetType(), package_.GetArrayType(0, element_type));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           builder_.BuildWithReturnValue(unpacked));
  EXPECT_EQ(function->node_count(), 2);
}

TEST_F(PackedSumIrTest, NestedArraysResolveEveryConstructedArray) {
  ArrayType outer(
      std::make_unique<ArrayType>(BitsType::MakeU8(), TypeDim::CreateU32(2)),
      TypeDim::CreateU32(2));
  const Type& inner = outer.element_type();
  const Type& bits = inner.AsArray().element_type();
  xls::Type* inner_element_type = package_.GetBitsType(8);
  xls::Type* outer_element_type = package_.GetArrayType(2, inner_element_type);
  std::vector<const Type*> resolved;
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(
          builder_, outer, builder_.Literal(UBits(0x04030201, 32)),
          [&](const Type& type) {
            resolved.push_back(&type);
            return &type == &inner ? outer_element_type : inner_element_type;
          },
          {}));
  EXPECT_THAT(resolved, ::testing::UnorderedElementsAre(&bits, &bits, &inner));
  XLS_ASSERT_OK_AND_ASSIGN(Value result, Run(unpacked));
  EXPECT_EQ(result,
            Value::ArrayOrDie(
                {Value::ArrayOrDie({Value(UBits(1, 8)), Value(UBits(2, 8))}),
                 Value::ArrayOrDie({Value(UBits(3, 8)), Value(UBits(4, 8))})}));
}

TEST_F(PackedSumIrTest, NonemptyArrayChecksValuesAgainstResolvedElementType) {
  ArrayType array(BitsType::MakeU8(), TypeDim::CreateU32(1));
  xls::Type* mismatched_type = package_.GetBitsType(4);
  XLS_ASSERT_OK_AND_ASSIGN(
      BValue unpacked,
      UnpackPackedSumPayload(builder_, array, builder_.Literal(UBits(0x12, 8)),
                             [&](const Type& type) {
                               EXPECT_EQ(&type, &array.element_type());
                               return mismatched_type;
                             },
                             {}));
  EXPECT_FALSE(unpacked.valid());
  EXPECT_THAT(
      builder_.GetError().message(),
      ::testing::HasSubstr(
          "Element type bits[8] does not match expected type: bits[4]"));
}

TEST_F(PackedSumIrTest, PropagatesArrayElementResolverError) {
  ArrayType array(BitsType::MakeU8(), TypeDim::CreateU32(1));
  ArrayType oversized(std::make_unique<BitsType>(false, 1'000'000),
                      TypeDim::CreateU32(5'000));
  const auto error = GetPackedSumBitCount(oversized);
  ASSERT_FALSE(error.ok());
  int64_t resolver_calls = 0;
  const auto unpacked = UnpackPackedSumPayload(
      builder_, array, builder_.Literal(UBits(0x12, 8)),
      [&](const Type& type) -> absl::StatusOr<xls::Type*> {
        EXPECT_EQ(&type, &array.element_type());
        ++resolver_calls;
        return error.status();
      },
      {});
  EXPECT_EQ(resolver_calls, 1);
  EXPECT_EQ(unpacked.status(), error.status());
  XLS_EXPECT_OK(builder_.GetError());
}

}  // namespace
}  // namespace xls::dslx::internal
