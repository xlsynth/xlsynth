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
#include <tuple>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/package.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"

namespace xls::dslx::internal {
namespace {

constexpr int64_t kUsizeBits = 32;

absl::Span<const std::unique_ptr<Type>> AggregateMembers(const Type& type) {
  if (type.IsTuple()) {
    return type.AsTuple().members();
  } else {
    return dynamic_cast<const StructTypeBase&>(type).members();
  }
}

BValue ConcatOrZero(BuilderBase& builder, absl::Span<const BValue> pieces,
                    const SourceInfo& loc) {
  if (pieces.empty()) {
    return builder.Literal(UBits(0, 0), loc);
  } else {
    return builder.Concat(pieces, loc);
  }
}

absl::StatusOr<BValue> Pack(BuilderBase& builder, const Type& type,
                            BValue value, const SourceInfo& loc) {
  if (type.IsSum()) {
    return builder.Concat(
        {builder.TupleIndex(value, 0, loc),
         builder.TupleIndex(builder.TupleIndex(value, 1, loc), 0, loc)},
        loc);
  } else if (type.IsTuple() || dynamic_cast<const StructTypeBase*>(&type)) {
    std::vector<BValue> pieces;
    auto members = AggregateMembers(type);
    for (int64_t i = 0; i < members.size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          BValue piece,
          Pack(builder, *members[i], builder.TupleIndex(value, i, loc), loc));
      pieces.push_back(piece);
    }
    return ConcatOrZero(builder, pieces, loc);
  } else if (type.IsEnum() || GetBitsLike(type).has_value()) {
    return value;
  } else if (type.IsArray()) {
    const ArrayType& array = type.AsArray();
    XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
    std::vector<BValue> pieces;
    for (int64_t i = size - 1; i >= 0; --i) {
      BValue index = builder.Literal(UBits(i, kUsizeBits), loc);
      XLS_ASSIGN_OR_RETURN(BValue piece,
                           Pack(builder, array.element_type(),
                                builder.ArrayIndex(value, {index}, loc), loc));
      pieces.push_back(piece);
    }
    return ConcatOrZero(builder, pieces, loc);
  } else {
    return absl::UnimplementedError(
        absl::StrCat("Cannot pack sum payload type: ", type.ToString()));
  }
}

// The value is present exactly when input bits were supplied. One traversal
// owns both the type projection and reconstruction, including empty arrays.
struct PackedProjection {
  xls::Type* type;
  BValue value;
};

absl::StatusOr<PackedProjection> Project(Package& package, const Type& type,
                                         std::optional<BValue> bits,
                                         const SourceInfo& loc) {
  BuilderBase* builder = bits.has_value() ? bits->builder() : nullptr;
  if (type.IsSum()) {
    const SumTypeEncoding encoding(type.AsSum());
    XLS_RETURN_IF_ERROR(GetPackedSumBitCount(type).status());
    XLS_ASSIGN_OR_RETURN(int64_t slot_width, encoding.payload_slot_bit_count());
    XLS_ASSIGN_OR_RETURN(int64_t tag_width, encoding.tag_bit_count());
    xls::Type* result = package.GetTupleType(
        {package.GetBitsType(tag_width),
         package.GetTupleType({package.GetBitsType(slot_width)})});
    BValue value;
    if (bits.has_value()) {
      BValue tag = builder->BitSlice(*bits, slot_width, tag_width, loc);
      BValue slot = builder->BitSlice(*bits, 0, slot_width, loc);
      value = builder->Tuple({tag, builder->Tuple({slot}, loc)}, loc);
    }
    return PackedProjection{result, value};
  } else if (type.IsTuple() || dynamic_cast<const StructTypeBase*>(&type)) {
    int64_t offset = 0;
    if (bits.has_value()) {
      XLS_ASSIGN_OR_RETURN(offset, GetPackedSumBitCount(type));
    }
    std::vector<xls::Type*> member_types;
    std::vector<BValue> members;
    for (const std::unique_ptr<Type>& member : AggregateMembers(type)) {
      std::optional<BValue> member_bits;
      if (bits.has_value()) {
        XLS_ASSIGN_OR_RETURN(int64_t width, GetPackedSumBitCount(*member));
        offset -= width;
        member_bits = builder->BitSlice(*bits, offset, width, loc);
      }
      XLS_ASSIGN_OR_RETURN(PackedProjection child,
                           Project(package, *member, member_bits, loc));
      member_types.push_back(child.type);
      if (bits.has_value()) {
        members.push_back(child.value);
      }
    }
    return PackedProjection{
        package.GetTupleType(member_types),
        bits.has_value() ? builder->Tuple(members, loc) : BValue()};
  } else if (type.IsEnum() || GetBitsLike(type).has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t width, GetPackedSumBitCount(type));
    return PackedProjection{package.GetBitsType(width),
                            bits.value_or(BValue())};
  } else if (type.IsArray()) {
    const ArrayType& array = type.AsArray();
    XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
    xls::Type* element_type;
    std::vector<BValue> elements;
    if (bits.has_value() && size != 0) {
      XLS_ASSIGN_OR_RETURN(int64_t width,
                           GetPackedSumBitCount(array.element_type()));
      for (int64_t i = 0; i < size; ++i) {
        XLS_ASSIGN_OR_RETURN(
            PackedProjection child,
            Project(package, array.element_type(),
                    builder->BitSlice(*bits, i * width, width, loc), loc));
        elements.push_back(child.value);
      }
      element_type = elements.front().GetType();
    } else {
      XLS_ASSIGN_OR_RETURN(
          PackedProjection child,
          Project(package, array.element_type(), std::nullopt, loc));
      element_type = child.type;
    }
    return PackedProjection{package.GetArrayType(size, element_type),
                            bits.has_value()
                                ? builder->Array(elements, element_type, loc)
                                : BValue()};
  } else {
    return absl::UnimplementedError(
        absl::StrCat("Cannot unpack sum payload type: ", type.ToString()));
  }
}

// Keep original roots and absolute offsets in the equality memo: slicing or
// rebuilding tuples would give a shared child a new identity on each path.
class EqualityBuilder {
 public:
  EqualityBuilder(BuilderBase& builder, const SourceInfo& loc)
      : builder_(builder), loc_(loc) {}

  absl::StatusOr<BValue> Compare(const Type& type, BValue lhs, BValue rhs) {
    if (type.IsSum() && type.AsSum().variant_count() != 0) {
      return CompareSum(
          type.AsSum(), builder_.TupleIndex(lhs, 0, loc_),
          builder_.TupleIndex(rhs, 0, loc_),
          {builder_.TupleIndex(builder_.TupleIndex(lhs, 1, loc_), 0, loc_),
           builder_.TupleIndex(builder_.TupleIndex(rhs, 1, loc_), 0, loc_)});
    } else if (!TypeContainsSemanticSum(type)) {
      return builder_.Eq(lhs, rhs, loc_);
    } else if (type.IsTuple() || dynamic_cast<const StructTypeBase*>(&type)) {
      std::vector<BValue> equal;
      auto members = AggregateMembers(type);
      for (int64_t i = 0; i < members.size(); ++i) {
        XLS_ASSIGN_OR_RETURN(
            BValue member,
            Compare(*members[i], builder_.TupleIndex(lhs, i, loc_),
                    builder_.TupleIndex(rhs, i, loc_)));
        equal.push_back(member);
      }
      return All(equal);
    } else if (type.IsArray()) {
      const ArrayType& array = type.AsArray();
      XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
      std::vector<BValue> equal;
      for (int64_t i = 0; i < size; ++i) {
        BValue index = builder_.Literal(UBits(i, kUsizeBits), loc_);
        XLS_ASSIGN_OR_RETURN(BValue element,
                             Compare(array.element_type(),
                                     builder_.ArrayIndex(lhs, {index}, loc_),
                                     builder_.ArrayIndex(rhs, {index}, loc_)));
        equal.push_back(element);
      }
      return All(equal);
    } else {
      return builder_.Eq(lhs, rhs, loc_);
    }
  }

 private:
  struct PackedPair {
    BValue lhs;
    BValue rhs;
    int64_t lhs_offset = 0;
    int64_t rhs_offset = 0;
    PackedPair At(int64_t offset) const {
      return {lhs, rhs, lhs_offset + offset, rhs_offset + offset};
    }
  };

  BValue All(absl::Span<const BValue> values) {
    if (values.empty()) {
      return builder_.Literal(UBits(1, 1), loc_);
    } else if (values.size() == 1) {
      return values.front();
    } else {
      return builder_.And(values, loc_);
    }
  }

  absl::StatusOr<BValue> ComparePacked(const Type& type, PackedPair values) {
    const bool contains_sum = TypeContainsSemanticSum(type);
    if (type.IsSum() && type.AsSum().variant_count() != 0) {
      const SumType& sum = type.AsSum();
      const PackedEqKey key{&sum.variants(), values.lhs.node(),
                            values.rhs.node(), values.lhs_offset,
                            values.rhs_offset};
      if (auto it = packed_equalities_.find(key);
          it != packed_equalities_.end()) {
        return it->second;
      } else {
        const SumTypeEncoding encoding(sum);
        XLS_ASSIGN_OR_RETURN(int64_t slot_width,
                             encoding.payload_slot_bit_count());
        XLS_ASSIGN_OR_RETURN(int64_t tag_width, encoding.tag_bit_count());
        BValue lhs_tag = builder_.BitSlice(
            values.lhs, values.lhs_offset + slot_width, tag_width, loc_);
        BValue rhs_tag = builder_.BitSlice(
            values.rhs, values.rhs_offset + slot_width, tag_width, loc_);
        XLS_ASSIGN_OR_RETURN(BValue equal,
                             CompareSum(sum, lhs_tag, rhs_tag, values));
        packed_equalities_.emplace(key, equal);
        return equal;
      }
    } else if (contains_sum &&
               (type.IsTuple() || dynamic_cast<const StructTypeBase*>(&type))) {
      XLS_ASSIGN_OR_RETURN(int64_t offset, GetPackedSumBitCount(type));
      std::vector<BValue> equal;
      for (const std::unique_ptr<Type>& member : AggregateMembers(type)) {
        XLS_ASSIGN_OR_RETURN(int64_t width, GetPackedSumBitCount(*member));
        offset -= width;
        XLS_ASSIGN_OR_RETURN(BValue result,
                             ComparePacked(*member, values.At(offset)));
        equal.push_back(result);
      }
      return All(equal);
    } else if (contains_sum && type.IsArray()) {
      const ArrayType& array = type.AsArray();
      XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
      XLS_ASSIGN_OR_RETURN(int64_t width,
                           GetPackedSumBitCount(array.element_type()));
      std::vector<BValue> equal;
      for (int64_t i = 0; i < size; ++i) {
        XLS_ASSIGN_OR_RETURN(
            BValue result,
            ComparePacked(array.element_type(), values.At(i * width)));
        equal.push_back(result);
      }
      return All(equal);
    } else {
      XLS_ASSIGN_OR_RETURN(int64_t width, GetPackedSumBitCount(type));
      return builder_.Eq(
          builder_.BitSlice(values.lhs, values.lhs_offset, width, loc_),
          builder_.BitSlice(values.rhs, values.rhs_offset, width, loc_), loc_);
    }
  }

  absl::StatusOr<BValue> CompareSum(const SumType& sum, BValue lhs_tag,
                                    BValue rhs_tag, PackedPair payloads) {
    const SumTypeEncoding encoding(sum);
    std::vector<BValue> cases;
    XLS_RETURN_IF_ERROR(encoding.ForEachVariant(
        [&](const SumTypeEncoding::VariantInfo& variant) -> absl::Status {
          XLS_ASSIGN_OR_RETURN(int64_t offset, variant.payload_bit_count());
          std::vector<BValue> equal;
          for (int64_t i = 0; i < variant.payload_size(); ++i) {
            const Type& member = variant.variant->GetMemberType(i);
            XLS_ASSIGN_OR_RETURN(int64_t width, GetPackedSumBitCount(member));
            offset -= width;
            XLS_ASSIGN_OR_RETURN(BValue result,
                                 ComparePacked(member, payloads.At(offset)));
            equal.push_back(result);
          }
          cases.push_back(All(equal));
          return absl::OkStatus();
        }));
    BValue payload_equal = cases.back();
    for (int64_t i = sum.variant_count() - 2; i >= 0; --i) {
      XLS_ASSIGN_OR_RETURN(BValue tag,
                           BuildPackedSumDiscriminant(builder_, sum, i, loc_));
      payload_equal =
          builder_.Select(builder_.Eq(lhs_tag, tag, loc_),
                          {payload_equal, cases[i]}, std::nullopt, loc_);
    }
    return builder_.And(builder_.Eq(lhs_tag, rhs_tag, loc_), payload_equal,
                        loc_);
  }

  using PackedEqKey = std::tuple<const std::vector<SumTypeVariant>*, xls::Node*,
                                 xls::Node*, int64_t, int64_t>;
  BuilderBase& builder_;
  const SourceInfo& loc_;
  absl::flat_hash_map<PackedEqKey, BValue> packed_equalities_;
};

}  // namespace

absl::StatusOr<int64_t> GetPackedSumBitCount(const Type& type) {
  XLS_ASSIGN_OR_RETURN(TypeDim width, GetBitCountWithSharedSumPayload(type));
  return width.GetAsInt64();
}

absl::StatusOr<xls::Type*> GetPackedSumIrType(Package& package,
                                              const Type& type) {
  XLS_ASSIGN_OR_RETURN(PackedProjection result,
                       Project(package, type, std::nullopt, SourceInfo()));
  return result.type;
}

absl::StatusOr<BValue> BuildPackedSumDiscriminant(BuilderBase& builder,
                                                  const SumType& sum,
                                                  int64_t variant_index,
                                                  const SourceInfo& loc) {
  XLS_ASSIGN_OR_RETURN(Value value,
                       sum.GetDiscriminant(variant_index).ConvertToIr());
  return builder.Literal(value, loc);
}

absl::StatusOr<BValue> BuildPackedSumValue(
    BuilderBase& builder, const SumType& sum, const SumTypeEncoding& encoding,
    const SumTypeEncoding::VariantInfo& variant,
    absl::Span<const BValue> members, const SourceInfo& loc) {
  XLS_RET_CHECK_EQ(members.size(), variant.payload_size());
  XLS_RETURN_IF_ERROR(GetPackedSumBitCount(sum).status());
  XLS_ASSIGN_OR_RETURN(int64_t active_width, variant.payload_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t slot_width, encoding.payload_slot_bit_count());
  std::vector<BValue> pieces;
  XLS_RETURN_IF_ERROR(encoding.ForEachPayloadMember(
      variant, [&](int64_t i, const Type& type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(BValue piece,
                             Pack(builder, type, members[i], loc));
        pieces.push_back(piece);
        return absl::OkStatus();
      }));
  BValue slot = ConcatOrZero(builder, pieces, loc);
  if (active_width != slot_width) {
    slot = builder.ZeroExtend(slot, slot_width, loc);
  }
  XLS_ASSIGN_OR_RETURN(
      BValue tag,
      BuildPackedSumDiscriminant(builder, sum, variant.variant_index, loc));
  return builder.Tuple({tag, builder.Tuple({slot}, loc)}, loc);
}

absl::StatusOr<BValue> UnpackPackedSumPayload(BuilderBase& builder,
                                              const Type& type, BValue bits,
                                              const SourceInfo& loc) {
  XLS_ASSIGN_OR_RETURN(PackedProjection result,
                       Project(*builder.package(), type, bits, loc));
  return result.value;
}

absl::StatusOr<BValue> BuildPackedSumEquality(BuilderBase& builder,
                                              const Type& type, BValue lhs,
                                              BValue rhs,
                                              const SourceInfo& loc) {
  return EqualityBuilder(builder, loc).Compare(type, lhs, rhs);
}

}  // namespace xls::dslx::internal
