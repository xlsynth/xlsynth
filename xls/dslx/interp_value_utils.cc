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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/value.h"

namespace xls::dslx {

namespace {

// Transport and outer construction require shape, not observation of nested
// tags. Source values and equality require every active constructor to be
// valid.
enum class SumValidation { kRepresentation, kDeclaredConstructors };

// Shares ordinary type checks while making sum-observation depth explicit.
absl::Status ValidateValue(const InterpValue& value, const Type& type,
                           SumValidation sum_validation);

absl::StatusOr<InterpValue> InterpValueFromString(std::string_view s) {
  XLS_ASSIGN_OR_RETURN(Value value, Parser::ParseTypedValue(s));
  return dslx::ValueToInterpValue(value);
}

void CollectLeafChannelReferences(const InterpValue& channel_or_array,
                                  std::vector<InterpValue>& leaves) {
  if (channel_or_array.IsChannelArray()) {
    for (const InterpValue& elem :
         channel_or_array.GetChannelArrayOrDie().elements()) {
      CollectLeafChannelReferences(elem, leaves);
    }
  } else if (channel_or_array.IsChannelReference()) {
    leaves.push_back(channel_or_array);
  }
}

absl::Status ValidateBitsLikeValue(const InterpValue& value, const Type& type,
                                   const BitsLikeProperties& bits_like);

absl::Status ValidateEnumValue(const InterpValue& value,
                               const EnumType& enum_type);

absl::StatusOr<int64_t> GetFlattenedBitCount(const Type& type) {
  XLS_ASSIGN_OR_RETURN(TypeDim bit_count,
                       internal::GetBitCountWithSharedSumPayload(type));
  return bit_count.GetAsInt64();
}

absl::StatusOr<Bits> FlattenValueForType(const Type& type,
                                         const InterpValue& value);

absl::Status ValidateEncodedSumShape(const InterpValue& value,
                                     const SumType& sum_type);

absl::StatusOr<InterpValue> DecodeRawSumValue(const SumType& sum_type,
                                              const InterpValue& tag,
                                              const InterpValue& payload_slot);

absl::StatusOr<Bits> FlattenAggregateMembers(
    absl::Span<const std::unique_ptr<Type>> members,
    absl::Span<const InterpValue> values) {
  XLS_RET_CHECK_EQ(members.size(), values.size());
  std::vector<Bits> flattened_members;
  flattened_members.reserve(members.size());
  for (int64_t i = 0; i < members.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(Bits flattened_member,
                         FlattenValueForType(*members.at(i), values.at(i)));
    flattened_members.push_back(std::move(flattened_member));
  }
  return bits_ops::Concat(flattened_members);
}

absl::StatusOr<std::vector<InterpValue>> UnflattenAggregateMembers(
    absl::Span<const std::unique_ptr<Type>> members, const Bits& bits) {
  std::vector<InterpValue> values;
  values.reserve(members.size());
  int64_t bit_offset = bits.bit_count();
  for (const std::unique_ptr<Type>& member : members) {
    XLS_ASSIGN_OR_RETURN(int64_t member_bit_count,
                         GetFlattenedBitCount(*member));
    bit_offset -= member_bit_count;
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        internal::UnflattenValueForType(
            *member, bits.Slice(bit_offset, member_bit_count)));
    values.push_back(std::move(value));
  }
  return values;
}

absl::StatusOr<Bits> FlattenValueForType(const Type& type,
                                         const InterpValue& value) {
  if (dynamic_cast<const SumType*>(&type) != nullptr) {
    XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                         internal::GetEncodedSumView(value));
    if (!sum_view.tag.IsUBits() || !sum_view.payload_slot.IsUBits()) {
      return absl::InvalidArgumentError(
          "Expected encoded sum tag and shared payload slot to be unsigned "
          "bits.");
    }
    return bits_ops::Concat(
        {sum_view.tag.GetBitsOrDie(), sum_view.payload_slot.GetBitsOrDie()});
  } else if (dynamic_cast<const TokenType*>(&type) != nullptr) {
    if (!value.IsToken()) {
      return absl::InvalidArgumentError(
          "Expected token value while flattening.");
    }
    return Bits(/*bit_count=*/0);
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    XLS_RETURN_IF_ERROR(ValidateBitsLikeValue(value, type, *bits_like));
    return value.GetBitsOrDie();
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    if (!value.IsTuple()) {
      return absl::InvalidArgumentError(
          "Expected tuple value while flattening.");
    }
    return FlattenAggregateMembers(tuple_type->members(),
                                   value.GetValuesOrDie());
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    if (!value.IsTuple()) {
      return absl::InvalidArgumentError(
          "Expected struct value while flattening.");
    }
    return FlattenAggregateMembers(struct_type->members(),
                                   value.GetValuesOrDie());
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    if (!value.IsArray()) {
      return absl::InvalidArgumentError(
          "Expected array value while flattening.");
    }
    std::vector<Bits> flattened_elements;
    flattened_elements.reserve(value.GetValuesOrDie().size());
    for (int64_t i = static_cast<int64_t>(value.GetValuesOrDie().size()) - 1;
         i >= 0; --i) {
      XLS_ASSIGN_OR_RETURN(Bits flattened_element,
                           FlattenValueForType(array_type->element_type(),
                                               value.GetValuesOrDie().at(i)));
      flattened_elements.push_back(std::move(flattened_element));
    }
    return bits_ops::Concat(flattened_elements);
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    XLS_RETURN_IF_ERROR(ValidateEnumValue(value, *enum_type));
    return value.GetBitsOrDie();
  } else {
    return absl::UnimplementedError(
        absl::StrCat("Cannot flatten InterpValue for type: ", type.ToString()));
  }
}

}  // namespace

namespace internal {

absl::StatusOr<InterpValue> UnflattenValueForType(const Type& type,
                                                  const Bits& bits) {
  XLS_ASSIGN_OR_RETURN(int64_t expected_bit_count, GetFlattenedBitCount(type));
  if (bits.bit_count() != expected_bit_count) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Cannot unflatten `%s`: expected %d bits; got %d.",
                        type.ToString(), expected_bit_count, bits.bit_count()));
  }

  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(TypeDim payload_bit_count,
                         sum_type->GetMaxPayloadBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                         payload_bit_count.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count,
                         sum_type->tag_bit_count().GetAsInt64());
    // Extracting a payload transports an existing nested image. Its tag is
    // checked only by an observer or an explicit source-domain validator.
    return internal::CreateEncodedSumTuple(
        InterpValue::MakeUnsigned(
            bits.Slice(payload_slot_bit_count, tag_bit_count)),
        InterpValue::MakeUnsigned(bits.Slice(0, payload_slot_bit_count)));
  } else if (dynamic_cast<const TokenType*>(&type) != nullptr) {
    return InterpValue::MakeToken();
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());
    return InterpValue::MakeBits(is_signed, bits);
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(
        std::vector<InterpValue> members,
        UnflattenAggregateMembers(tuple_type->members(), bits));
    return InterpValue::MakeTuple(std::move(members));
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    XLS_ASSIGN_OR_RETURN(
        std::vector<InterpValue> members,
        UnflattenAggregateMembers(struct_type->members(), bits));
    return InterpValue::MakeTuple(std::move(members));
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type->size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t element_bit_count,
                         GetFlattenedBitCount(array_type->element_type()));
    std::vector<InterpValue> elements;
    elements.reserve(array_size);
    for (int64_t i = 0; i < array_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue element,
          UnflattenValueForType(
              array_type->element_type(),
              bits.Slice(i * element_bit_count, element_bit_count)));
      elements.push_back(std::move(element));
    }
    return InterpValue::MakeArray(std::move(elements));
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return InterpValue::MakeEnum(bits, enum_type->is_signed(),
                                 &enum_type->nominal_type());
  } else {
    return absl::UnimplementedError(absl::StrCat(
        "Cannot unflatten InterpValue for type: ", type.ToString()));
  }
}

}  // namespace internal

namespace {

absl::StatusOr<std::vector<InterpValue>> DecodeVariantPayloadValues(
    const SumTypeEncoding::VariantInfo& variant,
    const InterpValue& payload_slot) {
  if (!payload_slot.IsUBits()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Expected shared sum payload slot to be unsigned bits; got `%s`.",
        payload_slot.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t active_payload_bit_count,
                       variant.payload_bit_count());
  Bits active_payload_bits =
      payload_slot.GetBitsOrDie().Slice(0, active_payload_bit_count);
  std::vector<InterpValue> payload_values;
  payload_values.reserve(variant.payload_size());
  int64_t bit_offset = active_payload_bit_count;
  for (int64_t active_index = 0; active_index < variant.payload_size();
       ++active_index) {
    const Type& member_type = variant.variant->GetMemberType(active_index);
    XLS_ASSIGN_OR_RETURN(int64_t member_bit_count,
                         GetFlattenedBitCount(member_type));
    bit_offset -= member_bit_count;
    XLS_ASSIGN_OR_RETURN(InterpValue payload_value,
                         internal::UnflattenValueForType(
                             member_type, active_payload_bits.Slice(
                                              bit_offset, member_bit_count)));
    payload_values.push_back(std::move(payload_value));
  }
  return payload_values;
}

absl::Status ValidateEncodedSumShape(const InterpValue& value,
                                     const SumType& sum_type) {
  XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                       internal::GetEncodedSumView(value));

  if (!sum_view.tag.IsUBits()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Expected sum tag for `%s` to be unsigned bits; got `%s`.",
        sum_type.ToString(), sum_view.tag.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_tag_bit_count,
                       sum_type.tag_bit_count().GetAsInt64());
  XLS_ASSIGN_OR_RETURN(int64_t actual_tag_bit_count,
                       sum_view.tag.GetBitCount());
  if (actual_tag_bit_count != expected_tag_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum `%s` expected a %d-bit tag; got %d bits.", sum_type.ToString(),
        expected_tag_bit_count, actual_tag_bit_count));
  }

  if (!sum_view.payload_slot.IsUBits()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Expected sum payload slot for `%s` to be unsigned bits; got `%s`.",
        sum_type.ToString(), sum_view.payload_slot.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(TypeDim payload_width, sum_type.GetMaxPayloadBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t expected_payload_bit_count,
                       payload_width.GetAsInt64());
  XLS_RETURN_IF_ERROR(
      internal::GetBitCountWithSharedSumPayload(sum_type).status());
  XLS_ASSIGN_OR_RETURN(int64_t actual_payload_bit_count,
                       sum_view.payload_slot.GetBitCount());
  if (actual_payload_bit_count != expected_payload_bit_count) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Sum `%s` expected a %d-bit payload slot; got %d bits.",
                        sum_type.ToString(), expected_payload_bit_count,
                        actual_payload_bit_count));
  }
  return absl::OkStatus();
}

absl::Status ValidateTupleValue(const InterpValue& value,
                                const TupleType& tuple_type) {
  if (!value.IsTuple()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected tuple-typed value `%s`; got `%s`.",
                        tuple_type.ToString(), value.ToString()));
  }
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != tuple_type.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d members; got %d.",
        value.ToString(), tuple_type.ToString(), tuple_type.size(),
        static_cast<int64_t>(elements.size())));
  }
  for (int64_t i = 0; i < tuple_type.size(); ++i) {
    XLS_RETURN_IF_ERROR(ValidateInterpValueMatchesType(
        elements.at(i), tuple_type.GetMemberType(i)));
  }
  return absl::OkStatus();
}

absl::Status ValidateStructValue(const InterpValue& value,
                                 const StructTypeBase& struct_type) {
  if (!value.IsTuple()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected struct-typed value `%s`; got `%s`.",
                        struct_type.ToString(), value.ToString()));
  }
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != struct_type.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d members; got %d.",
        value.ToString(), struct_type.ToString(), struct_type.size(),
        static_cast<int64_t>(elements.size())));
  }
  for (int64_t i = 0; i < struct_type.size(); ++i) {
    XLS_RETURN_IF_ERROR(ValidateInterpValueMatchesType(
        elements.at(i), struct_type.GetMemberType(i)));
  }
  return absl::OkStatus();
}

absl::Status ValidateArrayValue(const InterpValue& value,
                                const ArrayType& array_type) {
  if (!value.IsArray()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected array-typed value for `%s`; got `%s`.",
                        array_type.ToString(), value.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_size, array_type.size().GetAsInt64());
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != expected_size) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d elements; got %d.",
        value.ToString(), array_type.ToString(), expected_size,
        static_cast<int64_t>(elements.size())));
  }
  for (const InterpValue& element : elements) {
    XLS_RETURN_IF_ERROR(
        ValidateInterpValueMatchesType(element, array_type.element_type()));
  }
  return absl::OkStatus();
}

absl::StatusOr<InterpValue> DecodeRawSumValue(const SumType& sum_type,
                                              const InterpValue& tag,
                                              const InterpValue& payload_slot) {
  InterpValue raw_value = internal::CreateEncodedSumTuple(tag, payload_slot);
  XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(raw_value, sum_type));

  const SumTypeEncoding encoding(sum_type);
  absl::StatusOr<SumTypeEncoding::VariantInfo> variant =
      encoding.GetVariantByTagBits(tag.GetBitsOrDie());
  if (variant.status().code() == absl::StatusCode::kNotFound) {
    return raw_value;
  }
  XLS_ASSIGN_OR_RETURN(SumTypeEncoding::VariantInfo valid_variant,
                       std::move(variant));
  XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> payload_values,
                       DecodeVariantPayloadValues(valid_variant, payload_slot));

  for (int64_t index = 0; index < payload_values.size(); ++index) {
    const Type& member_type = valid_variant.variant->GetMemberType(index);
    const InterpValue& payload_value = payload_values.at(index);
    XLS_RETURN_IF_ERROR(
        ValidateInterpValueMatchesType(payload_value, member_type));
  }
  return raw_value;
}

absl::Status ValidateBitsLikeValue(const InterpValue& value, const Type& type,
                                   const BitsLikeProperties& bits_like) {
  if (!value.IsBits()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected bits-typed value for `%s`; got `%s`.",
                        type.ToString(), value.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_bit_count, bits_like.size.GetAsInt64());
  XLS_ASSIGN_OR_RETURN(int64_t actual_bit_count, value.GetBitCount());
  if (actual_bit_count != expected_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d bits; got %d.",
        value.ToString(), type.ToString(), expected_bit_count,
        actual_bit_count));
  }
  XLS_ASSIGN_OR_RETURN(bool expected_signed, bits_like.is_signed.GetAsBool());
  if (value.IsSigned() != expected_signed) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %s bits.", value.ToString(),
        type.ToString(), expected_signed ? "signed" : "unsigned"));
  }
  return absl::OkStatus();
}

absl::Status ValidateEnumIdentity(const InterpValue& value,
                                  const EnumType& enum_type) {
  if (!value.IsEnum()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected enum-typed value for `%s`; got `%s`.",
                        enum_type.ToString(), value.ToString()));
  }
  InterpValue::EnumData enum_data = value.GetEnumData().value();
  if (enum_data.def != &enum_type.nominal_type()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Value `%s` does not match enum `%s`.",
                        value.ToString(), enum_type.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_bit_count,
                       enum_type.size().GetAsInt64());
  if (enum_data.value.bit_count() != expected_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match enum `%s`: expected %d bits; got %d.",
        value.ToString(), enum_type.ToString(), expected_bit_count,
        enum_data.value.bit_count()));
  }
  if (enum_data.is_signed != enum_type.is_signed()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match enum `%s`: expected %s enum value.",
        value.ToString(), enum_type.ToString(),
        enum_type.is_signed() ? "signed" : "unsigned"));
  }
  return absl::OkStatus();
}

absl::Status ValidateEnumValue(const InterpValue& value,
                               const EnumType& enum_type) {
  XLS_RETURN_IF_ERROR(ValidateEnumIdentity(value, enum_type));
  const InterpValue::EnumData enum_data = value.GetEnumData().value();
  const Bits& value_bits = enum_data.value;
  for (const InterpValue& member : enum_type.members()) {
    if (value_bits == member.GetBitsOrDie()) {
      return absl::OkStatus();
    }
  }
  return absl::InvalidArgumentError(absl::StrFormat(
      "Value `%s` does not match enum `%s`: expected a declared member.",
      value.ToString(), enum_type.ToString()));
}

}  // namespace

namespace internal {

// One validator/comparator serves ordinary calls and match-local reuse. A null
// observation owner keeps ordinary validation free of path/map allocation.
class ValueTraversal {
 public:
  explicit ValueTraversal(SumValidation sum_validation,
                          MatchValueObservation* observation = nullptr,
                          MatchValueObservation::Path path = {})
      : sum_validation_(sum_validation),
        observation_(observation),
        path_(std::move(path)) {}

  // Checks according to sum_validation_, reusing successful complete subtrees
  // only when this traversal belongs to a match observation owner.
  absl::Status Validate(const InterpValue& value, const Type& type);
  // Both operands must already be fully validated. Only RHS storage is reused;
  // constants and their comparison results are never retained in the owner.
  absl::StatusOr<bool> Compare(const InterpValue& lhs, const InterpValue& rhs,
                               const Type& type);
  // Checks one constructor and ordinary payload validity, not nested sum tags.
  absl::StatusOr<const std::vector<InterpValue>*> ObserveSum(
      const SumType& type, const InterpValue& value,
      std::vector<InterpValue>& transient_payload);

 private:
  const MatchValueObservation::Observation* FindObservation() const;
  absl::Status ValidateUncached(const InterpValue& value, const Type& type);
  absl::Status ValidateTupleValue(const InterpValue& value,
                                  const TupleType& type);
  absl::Status ValidateStructValue(const InterpValue& value,
                                   const StructTypeBase& type);
  absl::Status ValidateArrayValue(const InterpValue& value,
                                  const ArrayType& type);
  absl::Status ValidateSumValue(const InterpValue& value, const SumType& type);
  absl::Status ValidateChild(int64_t index, const InterpValue& value,
                             const Type& type);
  absl::StatusOr<bool> CompareChild(int64_t index, const InterpValue& lhs,
                                    const InterpValue& rhs, const Type& type);
  // Returns owner-backed storage for matches, or uses transient_payload for
  // ordinary calls. Either storage must outlive the returned pointer's use.
  absl::StatusOr<const std::vector<InterpValue>*> DecodePayload(
      const SumTypeEncoding::VariantInfo& variant,
      const InterpValue& payload_slot,
      std::vector<InterpValue>& transient_payload);

  SumValidation sum_validation_;
  MatchValueObservation* observation_;
  MatchValueObservation::Path path_;
};

const MatchValueObservation::Observation* ValueTraversal::FindObservation()
    const {
  if (observation_ != nullptr) {
    auto it = observation_->observations_.find(path_);
    return it == observation_->observations_.end() ? nullptr : &it->second;
  } else {
    return nullptr;
  }
}

absl::Status ValueTraversal::Validate(const InterpValue& value,
                                      const Type& type) {
  if (observation_ == nullptr || !type.IsAggregate()) {
    // Scalar leaves have no decoded payload or recursive work to retain.
    return ValidateUncached(value, type);
  } else if (const auto* existing = FindObservation();
             existing != nullptr &&
             existing->validation ==
                 MatchValueObservation::Validation::kComplete) {
    return absl::OkStatus();
  } else {
    XLS_RETURN_IF_ERROR(ValidateUncached(value, type));
    if (sum_validation_ == SumValidation::kDeclaredConstructors) {
      observation_->observations_[path_].validation =
          MatchValueObservation::Validation::kComplete;
    }
    return absl::OkStatus();
  }
}

absl::Status ValueTraversal::ValidateChild(int64_t index,
                                           const InterpValue& value,
                                           const Type& type) {
  if (observation_ != nullptr) {
    path_.push_back(index);
  }
  absl::Status status = Validate(value, type);
  if (observation_ != nullptr) {
    path_.pop_back();
  }
  return status;
}

absl::StatusOr<bool> ValueTraversal::CompareChild(int64_t index,
                                                  const InterpValue& lhs,
                                                  const InterpValue& rhs,
                                                  const Type& type) {
  if (observation_ != nullptr) {
    path_.push_back(index);
  }
  absl::StatusOr<bool> equal = Compare(lhs, rhs, type);
  if (observation_ != nullptr) {
    path_.pop_back();
  }
  return equal;
}

absl::StatusOr<const std::vector<InterpValue>*> ValueTraversal::DecodePayload(
    const SumTypeEncoding::VariantInfo& variant,
    const InterpValue& payload_slot,
    std::vector<InterpValue>& transient_payload) {
  if (observation_ == nullptr) {
    XLS_ASSIGN_OR_RETURN(transient_payload,
                         DecodeVariantPayloadValues(variant, payload_slot));
    return &transient_payload;
  } else {
    auto& entry = observation_->observations_[path_];
    if (!entry.payload.has_value()) {
      XLS_ASSIGN_OR_RETURN(entry.payload,
                           DecodeVariantPayloadValues(variant, payload_slot));
    }
    return &*entry.payload;
  }
}

absl::Status ValueTraversal::ValidateTupleValue(const InterpValue& value,
                                                const TupleType& tuple_type) {
  if (!value.IsTuple()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected tuple-typed value `%s`; got `%s`.",
                        tuple_type.ToString(), value.ToString()));
  }
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != tuple_type.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d members; got %d.",
        value.ToString(), tuple_type.ToString(), tuple_type.size(),
        static_cast<int64_t>(elements.size())));
  }
  for (int64_t i = 0; i < tuple_type.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        ValidateChild(i, elements.at(i), tuple_type.GetMemberType(i)));
  }
  return absl::OkStatus();
}

absl::Status ValueTraversal::ValidateStructValue(
    const InterpValue& value, const StructTypeBase& struct_type) {
  if (!value.IsTuple()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected struct-typed value `%s`; got `%s`.",
                        struct_type.ToString(), value.ToString()));
  }
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != struct_type.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d members; got %d.",
        value.ToString(), struct_type.ToString(), struct_type.size(),
        static_cast<int64_t>(elements.size())));
  }
  for (int64_t i = 0; i < struct_type.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        ValidateChild(i, elements.at(i), struct_type.GetMemberType(i)));
  }
  return absl::OkStatus();
}

absl::Status ValueTraversal::ValidateArrayValue(const InterpValue& value,
                                                const ArrayType& array_type) {
  if (!value.IsArray()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected array-typed value for `%s`; got `%s`.",
                        array_type.ToString(), value.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_size, array_type.size().GetAsInt64());
  const std::vector<InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != expected_size) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match `%s`: expected %d elements; got %d.",
        value.ToString(), array_type.ToString(), expected_size,
        static_cast<int64_t>(elements.size())));
  }
  for (int64_t i = 0; i < elements.size(); ++i) {
    XLS_RETURN_IF_ERROR(
        ValidateChild(i, elements.at(i), array_type.element_type()));
  }
  return absl::OkStatus();
}

absl::Status ValueTraversal::ValidateSumValue(const InterpValue& value,
                                              const SumType& sum_type) {
  XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                       internal::GetEncodedSumView(value));
  const SumTypeEncoding encoding(sum_type);
  XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(value, sum_type));

  XLS_ASSIGN_OR_RETURN(
      SumTypeEncoding::VariantInfo variant,
      encoding.GetVariantByTagBits(sum_view.tag.GetBitsOrDie()));
  const SumTypeVariant& variant_def = *variant.variant;
  std::vector<InterpValue> transient_payload;
  XLS_ASSIGN_OR_RETURN(
      const std::vector<InterpValue>* active_payload_values,
      DecodePayload(variant, sum_view.payload_slot, transient_payload));
  for (int64_t i = 0; i < active_payload_values->size(); ++i) {
    XLS_RETURN_IF_ERROR(ValidateChild(i, active_payload_values->at(i),
                                      variant_def.GetMemberType(i)));
  }
  return absl::OkStatus();
}

absl::Status ValueTraversal::ValidateUncached(const InterpValue& value,
                                              const Type& type) {
  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    if (sum_validation_ == SumValidation::kRepresentation) {
      return ValidateEncodedSumShape(value, *sum_type);
    } else {
      return ValidateSumValue(value, *sum_type);
    }
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    return ValidateTupleValue(value, *tuple_type);
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    return ValidateStructValue(value, *struct_type);
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return ValidateEnumValue(value, *enum_type);
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    return ValidateBitsLikeValue(value, type, *bits_like);
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    return ValidateArrayValue(value, *array_type);
  } else if (dynamic_cast<const TokenType*>(&type) != nullptr) {
    if (!value.IsToken()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Expected token-typed value; got `%s`.", value.ToString()));
    }
    return absl::OkStatus();
  } else {
    return absl::UnimplementedError(absl::StrCat(
        "Cannot validate InterpValue against type: ", type.ToString()));
  }
}

// The caller has validated both complete source values. Comparison may stop at
// the first unequal member without hiding a later malformed active constructor.
absl::StatusOr<bool> ValueTraversal::Compare(const InterpValue& lhs,
                                             const InterpValue& rhs,
                                             const Type& type) {
  auto compare_members = [&](absl::Span<const std::unique_ptr<Type>> members)
      -> absl::StatusOr<bool> {
    for (int64_t i = 0; i < members.size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          bool equal, CompareChild(i, lhs.GetValuesOrDie().at(i),
                                   rhs.GetValuesOrDie().at(i), *members.at(i)));
      if (!equal) {
        return false;
      }
    }
    return true;
  };
  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(internal::EncodedSumView lhs_view,
                         internal::GetEncodedSumView(lhs));
    XLS_ASSIGN_OR_RETURN(internal::EncodedSumView rhs_view,
                         internal::GetEncodedSumView(rhs));
    const SumTypeEncoding encoding(*sum_type);
    XLS_ASSIGN_OR_RETURN(auto variant, encoding.GetVariantByTagBits(
                                           lhs_view.tag.GetBitsOrDie()));
    XLS_RETURN_IF_ERROR(
        encoding.GetVariantByTagBits(rhs_view.tag.GetBitsOrDie()).status());
    if (lhs_view.tag.Ne(rhs_view.tag)) {
      return false;
    }
    XLS_ASSIGN_OR_RETURN(
        std::vector<InterpValue> lhs_payload,
        DecodeVariantPayloadValues(variant, lhs_view.payload_slot));
    std::vector<InterpValue> transient_payload;
    XLS_ASSIGN_OR_RETURN(
        const std::vector<InterpValue>* rhs_payload,
        DecodePayload(variant, rhs_view.payload_slot, transient_payload));
    for (int64_t i = 0; i < variant.payload_size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          bool equal, CompareChild(i, lhs_payload.at(i), rhs_payload->at(i),
                                   variant.variant->GetMemberType(i)));
      if (!equal) {
        return false;
      }
    }
    return true;
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    return compare_members(tuple_type->members());
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    return compare_members(struct_type->members());
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type);
             array_type != nullptr && !GetBitsLike(type).has_value()) {
    for (int64_t i = 0; i < lhs.GetValuesOrDie().size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          bool equal,
          CompareChild(i, lhs.GetValuesOrDie().at(i),
                       rhs.GetValuesOrDie().at(i), array_type->element_type()));
      if (!equal) {
        return false;
      }
    }
    return true;
  } else {
    return lhs.Eq(rhs);
  }
}

absl::StatusOr<const std::vector<InterpValue>*> ValueTraversal::ObserveSum(
    const SumType& type, const InterpValue& value,
    std::vector<InterpValue>& transient_payload) {
  if (const auto* existing = FindObservation();
      existing != nullptr &&
      existing->validation != MatchValueObservation::Validation::kNone) {
    return &*existing->payload;
  } else {
    XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(value, type));
    XLS_ASSIGN_OR_RETURN(EncodedSumView view,
                         internal::GetEncodedSumView(value));
    XLS_ASSIGN_OR_RETURN(
        auto variant,
        SumTypeEncoding(type).GetVariantByTagBits(view.tag.GetBitsOrDie()));
    XLS_ASSIGN_OR_RETURN(
        const std::vector<InterpValue>* payload,
        DecodePayload(variant, view.payload_slot, transient_payload));
    // Shallow observation checks ordinary enums, but not active nested sum
    // tags. Full validation must instead recurse in member order; doing this
    // sweep first could report a later enum error before an earlier malformed
    // sum.
    ValueTraversal shallow(SumValidation::kRepresentation);
    for (int64_t i = 0; i < payload->size(); ++i) {
      XLS_RETURN_IF_ERROR(
          shallow.Validate(payload->at(i), variant.variant->GetMemberType(i)));
    }
    if (observation_ != nullptr) {
      observation_->observations_[path_].validation =
          MatchValueObservation::Validation::kShallow;
    }
    return payload;
  }
}

absl::StatusOr<const std::vector<InterpValue>*>
MatchValueObservation::GetSumPayloadValues(const SumType& type,
                                           const InterpValue& value,
                                           const Path& path) {
  std::vector<InterpValue> unused;
  return ValueTraversal(SumValidation::kRepresentation, this, path)
      .ObserveSum(type, value, unused);
}

absl::StatusOr<bool> MatchValueObservation::EqualsConstant(
    const InterpValue& constant, const InterpValue& value, const Type& type,
    const Path& path) {
  XLS_RETURN_IF_ERROR(ValueTraversal(SumValidation::kDeclaredConstructors)
                          .Validate(constant, type));
  ValueTraversal traversal(SumValidation::kDeclaredConstructors, this, path);
  XLS_RETURN_IF_ERROR(traversal.Validate(value, type));
  return traversal.Compare(constant, value, type);
}

absl::StatusOr<bool> PackedValuesEqual(const InterpValue& lhs,
                                       const InterpValue& rhs,
                                       const Type& type) {
  ValueTraversal traversal(SumValidation::kDeclaredConstructors);
  XLS_RETURN_IF_ERROR(traversal.Validate(lhs, type));
  XLS_RETURN_IF_ERROR(traversal.Validate(rhs, type));
  return traversal.Compare(lhs, rhs, type);
}

}  // namespace internal

namespace {

absl::Status ValidateValue(const InterpValue& value, const Type& type,
                           SumValidation sum_validation) {
  return internal::ValueTraversal(sum_validation).Validate(value, type);
}

}  // namespace

absl::Status ValidateInterpValueMatchesType(const InterpValue& value,
                                            const Type& type) {
  if (dynamic_cast<const SumType*>(&type) != nullptr) {
    return ValidateValue(value, type, SumValidation::kDeclaredConstructors);
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    return ValidateTupleValue(value, *tuple_type);
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    return ValidateStructValue(value, *struct_type);
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return ValidateEnumValue(value, *enum_type);
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    return ValidateBitsLikeValue(value, type, *bits_like);
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    return ValidateArrayValue(value, *array_type);
  } else if (dynamic_cast<const TokenType*>(&type) != nullptr) {
    if (!value.IsToken()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Expected token-typed value; got `%s`.", value.ToString()));
    }
    return absl::OkStatus();
  } else {
    return absl::UnimplementedError(absl::StrCat(
        "Cannot validate InterpValue against type: ", type.ToString()));
  }
}

absl::StatusOr<bool> SemanticValuesEqual(const InterpValue& lhs,
                                         const InterpValue& rhs,
                                         const Type& type) {
  return internal::PackedValuesEqual(lhs, rhs, type);
}

absl::StatusOr<InterpValue> CastBitsToArray(const InterpValue& bits_value,
                                            const ArrayType& array_type) {
  XLS_ASSIGN_OR_RETURN(TypeDim element_bit_count,
                       array_type.element_type().GetTotalBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t bits_per_element,
                       element_bit_count.GetAsInt64());
  XLS_ASSIGN_OR_RETURN(Bits bits, bits_value.GetBits());

  auto bit_slice_value_at_index = [&](int64_t i) -> InterpValue {
    int64_t lo = i * bits_per_element;
    Bits rev = bits_ops::Reverse(bits);
    Bits slice = rev.Slice(lo, bits_per_element);
    Bits result = bits_ops::Reverse(slice);
    return InterpValue::MakeBits(InterpValueTag::kUBits, result).value();
  };

  std::vector<InterpValue> values;
  XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type.size().GetAsInt64());
  values.reserve(array_size);
  for (int64_t i = 0; i < array_size; ++i) {
    values.push_back(bit_slice_value_at_index(i));
  }

  return InterpValue::MakeArray(values);
}

absl::StatusOr<InterpValue> CastBitsToEnum(const InterpValue& bits_value,
                                           const EnumType& enum_type) {
  const EnumDef& enum_def = enum_type.nominal_type();
  bool found = false;
  for (const InterpValue& member_value : enum_type.members()) {
    if (bits_value.GetBitsOrDie() == member_value.GetBitsOrDie()) {
      found = true;
      break;
    }
  }

  if (!found) {
    return absl::InternalError(
        absl::StrFormat("FailureError: Value is not valid for enum %s: %s",
                        enum_def.identifier(), bits_value.ToString()));
  }
  return InterpValue::MakeEnum(bits_value.GetBitsOrDie(), enum_type.is_signed(),
                               &enum_def);
}

namespace {

enum class TypeValuePolicy { kZero, kInternalPlaceholder };

absl::StatusOr<InterpValue> CreateValueFromType(const Type& type,
                                                TypeValuePolicy policy);

absl::StatusOr<InterpValue> CreatePlaceholderForSum(const SumType& type) {
  const SumTypeEncoding encoding(type);
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  InterpValue tag = type.variant_count() == 0
                        ? InterpValue::MakeUBits(tag_bit_count, 0)
                        : type.GetDiscriminant(0);
  return internal::CreateEncodedSumTuple(
      std::move(tag), InterpValue::MakeUBits(payload_slot_bit_count, 0));
}

absl::StatusOr<InterpValue> CreateValueFromType(const Type& type,
                                                TypeValuePolicy policy) {
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, bits_like->size.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());

    if (is_signed) {
      return InterpValue::MakeSBits(bit_count, /*value=*/0);
    }

    return InterpValue::MakeUBits(bit_count, /*value=*/0);
  }

  if (dynamic_cast<const TokenType*>(&type) != nullptr &&
      policy == TypeValuePolicy::kInternalPlaceholder) {
    // Inactive tokens do not represent an operation, so every placeholder
    // must share the same identity for independently constructed sums to be
    // equal.
    static const InterpValue placeholder = InterpValue::MakeToken();
    return placeholder;
  }

  if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    const int64_t tuple_size = tuple_type->size();

    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(tuple_size);

    for (int64_t i = 0; i < tuple_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateValueFromType(tuple_type->GetMemberType(i), policy));
      zero_elements.push_back(std::move(zero_element));
    }

    return InterpValue::MakeTuple(std::move(zero_elements));
  }

  if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type);
      struct_type != nullptr &&
      (policy == TypeValuePolicy::kInternalPlaceholder ||
       dynamic_cast<const StructType*>(struct_type) != nullptr)) {
    const int64_t struct_size = struct_type->size();

    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(struct_size);

    for (int64_t i = 0; i < struct_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateValueFromType(struct_type->GetMemberType(i), policy));
      zero_elements.push_back(std::move(zero_element));
    }

    return InterpValue::MakeTuple(std::move(zero_elements));
  }

  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(const int64_t array_size,
                         array_type->size().GetAsInt64());

    if (array_size == 0) {
      return InterpValue::MakeArray({});
    }

    XLS_ASSIGN_OR_RETURN(
        InterpValue zero_element,
        CreateValueFromType(array_type->element_type(), policy));
    std::vector<InterpValue> zero_elements(array_size, zero_element);
    return InterpValue::MakeArray(std::move(zero_elements));
  }

  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    if (!enum_type->members().empty()) {
      return enum_type->members().at(0);
    } else if (policy == TypeValuePolicy::kInternalPlaceholder) {
      XLS_ASSIGN_OR_RETURN(int64_t bit_count, enum_type->size().GetAsInt64());
      return InterpValue::MakeEnum(Bits(bit_count), enum_type->is_signed(),
                                   &enum_type->nominal_type());
    }
  }

  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    if (policy == TypeValuePolicy::kInternalPlaceholder) {
      return CreatePlaceholderForSum(*sum_type);
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Cannot create zero value for semantic sum type `",
                       sum_type->nominal_type().identifier(), "`."));
    }
  }

  if (policy == TypeValuePolicy::kInternalPlaceholder) {
    return absl::UnimplementedError(
        absl::StrCat("Cannot create internal placeholder value for type: ",
                     type.ToString()));
  } else {
    return absl::UnimplementedError("Cannot create zero value for type type: " +
                                    type.ToString());
  }
}

}  // namespace

namespace internal {

absl::StatusOr<InterpValue> CreateInternalPlaceholderValueFromType(
    const Type& type) {
  return CreateValueFromType(type, TypeValuePolicy::kInternalPlaceholder);
}

}  // namespace internal

absl::StatusOr<InterpValue> CreateZeroValueFromType(const Type& type) {
  return CreateValueFromType(type, TypeValuePolicy::kZero);
}

namespace {

enum class SumPayloadValidation { kValidate, kTrusted };

absl::StatusOr<InterpValue> AssembleKnownSumValue(
    const SumType& type, int64_t variant_index, int64_t payload_slot_bit_count,
    absl::Span<const InterpValue> payload_values,
    SumPayloadValidation validation) {
  if (variant_index < 0 || variant_index >= type.variant_count()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Sum `%s` has no constructor at index %d.",
                        type.nominal_type().identifier(), variant_index));
  }
  XLS_RETURN_IF_ERROR(internal::GetBitCountWithSharedSumPayload(type).status());
  const SumTypeVariant& variant = type.variants().at(variant_index);
  const std::string_view variant_name = variant.variant().identifier();
  if (payload_values.size() != variant.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum constructor `%s` expected %d payload values; got %d.",
        variant_name, variant.size(), payload_values.size()));
  }
  if (validation == SumPayloadValidation::kValidate) {
    for (int64_t active_index = 0; active_index < variant.size();
         ++active_index) {
      XLS_RETURN_IF_ERROR(ValidateValue(payload_values.at(active_index),
                                        variant.GetMemberType(active_index),
                                        SumValidation::kRepresentation));
    }
  }

  std::vector<Bits> flattened_members;
  flattened_members.reserve(payload_values.size());
  for (int64_t active_index = 0; active_index < variant.size();
       ++active_index) {
    XLS_ASSIGN_OR_RETURN(
        Bits flattened_member,
        FlattenValueForType(variant.GetMemberType(active_index),
                            payload_values.at(active_index)));
    flattened_members.push_back(std::move(flattened_member));
  }
  Bits payload_bits = bits_ops::Concat(flattened_members);
  if (payload_bits.bit_count() > payload_slot_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum constructor `%s` has a %d-bit payload but its slot has %d bits.",
        variant_name, payload_bits.bit_count(), payload_slot_bit_count));
  }
  return internal::CreateEncodedSumTuple(
      InterpValue::MakeUnsigned(
          type.GetDiscriminant(variant_index).GetBitsOrDie()),
      InterpValue::MakeUnsigned(bits_ops::ZeroExtend(std::move(payload_bits),
                                                     payload_slot_bit_count)));
}

absl::StatusOr<InterpValue> AssembleSumValue(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values,
    SumPayloadValidation validation) {
  const SumTypeEncoding encoding(type);
  XLS_ASSIGN_OR_RETURN(SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(variant_name));
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  return AssembleKnownSumValue(type, variant.variant_index,
                               payload_slot_bit_count, payload_values,
                               validation);
}

}  // namespace

absl::StatusOr<std::vector<InterpValue>> GetSumPayloadValues(
    const SumType& type, const InterpValue& value) {
  std::vector<InterpValue> payload;
  XLS_RETURN_IF_ERROR(internal::ValueTraversal(SumValidation::kRepresentation)
                          .ObserveSum(type, value, payload)
                          .status());
  return payload;
}

absl::StatusOr<InterpValue> CreateSumValue(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values) {
  return AssembleSumValue(type, variant_name, payload_values,
                          SumPayloadValidation::kValidate);
}

absl::StatusOr<InterpValue> CreateSumValue(
    const SumType& type, int64_t variant_index,
    absl::Span<const InterpValue> payload_values) {
  XLS_ASSIGN_OR_RETURN(TypeDim payload_width, type.GetMaxPayloadBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       payload_width.GetAsInt64());
  return AssembleKnownSumValue(type, variant_index, payload_slot_bit_count,
                               payload_values, SumPayloadValidation::kValidate);
}

namespace internal {

absl::StatusOr<InterpValue> CreateSumValueFromValidatedZeroPayload(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values) {
  return AssembleSumValue(type, variant_name, payload_values,
                          SumPayloadValidation::kTrusted);
}

absl::StatusOr<InterpValue> CreateSumValueFromValidatedGeneratedPayload(
    const SumType& type, int64_t variant_index, int64_t payload_slot_bit_count,
    absl::Span<const InterpValue> payload_values) {
  return AssembleKnownSumValue(type, variant_index, payload_slot_bit_count,
                               payload_values, SumPayloadValidation::kTrusted);
}

}  // namespace internal

absl::StatusOr<InterpValue> CreateZeroValue(const InterpValue& value) {
  switch (value.tag()) {
    case InterpValueTag::kSBits: {
      XLS_ASSIGN_OR_RETURN(int64_t bit_count, value.GetBitCount());
      return InterpValue::MakeSBits(bit_count, /*value=*/0);
    }
    case InterpValueTag::kUBits: {
      XLS_ASSIGN_OR_RETURN(int64_t bit_count, value.GetBitCount());
      return InterpValue::MakeUBits(bit_count, /*value=*/0);
    }
    case InterpValueTag::kTuple: {
      XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                           value.GetValues());
      std::vector<InterpValue> zero_elements;
      zero_elements.reserve(elements->size());
      for (const auto& element : *elements) {
        XLS_ASSIGN_OR_RETURN(InterpValue zero_element,
                             CreateZeroValue(element));
        zero_elements.push_back(zero_element);
      }
      return InterpValue::MakeTuple(zero_elements);
    }
    case InterpValueTag::kArray: {
      XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                           value.GetValues());
      if (elements->empty()) {
        return InterpValue::MakeArray({});
      }
      XLS_ASSIGN_OR_RETURN(InterpValue zero_element,
                           CreateZeroValue(elements->at(0)));
      std::vector<InterpValue> zero_elements(elements->size(), zero_element);
      return InterpValue::MakeArray(zero_elements);
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid InterpValueTag for zero-value generation: ",
                       TagToString(value.tag())));
  }
}

absl::StatusOr<std::optional<int64_t>> FindFirstDifferingIndex(
    absl::Span<const InterpValue> lhs, absl::Span<const InterpValue> rhs) {
  if (lhs.size() != rhs.size()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("LHS and RHS must have the same size: %d vs. %d.",
                        lhs.size(), rhs.size()));
  }

  for (int64_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].Ne(rhs[i])) {
      return i;
    }
  }

  return std::nullopt;
}

absl::StatusOr<InterpValue> SignConvertValue(const Type& type,
                                             const InterpValue& value) {
  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(value, *sum_type));
    return value;
  }

  if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    XLS_RET_CHECK(value.IsTuple()) << value.ToString();
    const int64_t tuple_size = value.GetValuesOrDie().size();
    std::vector<InterpValue> results;
    for (int64_t i = 0; i < tuple_size; ++i) {
      const InterpValue& e = value.GetValuesOrDie()[i];
      const Type& t = tuple_type->GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(InterpValue converted, SignConvertValue(t, e));
      results.push_back(converted);
    }
    return InterpValue::MakeTuple(std::move(results));
  }

  // Note: we have to test for BitsLike before ArrayType because
  // array-of-bits-constructor looks like an array but is actually bits-like.
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    XLS_RET_CHECK(value.IsBits()) << value.ToString();
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());
    if (is_signed) {
      return InterpValue::MakeBits(InterpValueTag::kSBits,
                                   value.GetBitsOrDie());
    }
    return value;
  }

  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_RET_CHECK(value.IsArray()) << value.ToString();
    const Type& t = array_type->element_type();
    int64_t array_size = value.GetValuesOrDie().size();
    std::vector<InterpValue> results;
    for (int64_t i = 0; i < array_size; ++i) {
      const InterpValue& e = value.GetValuesOrDie()[i];
      XLS_ASSIGN_OR_RETURN(InterpValue converted, SignConvertValue(t, e));
      results.push_back(converted);
    }
    return InterpValue::MakeArray(std::move(results));
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    if (value.IsEnum()) {
      XLS_RETURN_IF_ERROR(ValidateEnumIdentity(value, *enum_type));
      return value;
    }
    XLS_RET_CHECK(value.IsBits()) << value.ToString();
    XLS_ASSIGN_OR_RETURN(int64_t expected_bit_count,
                         enum_type->size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t actual_bit_count, value.GetBitCount());
    if (actual_bit_count != expected_bit_count) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Value `%s` does not match enum `%s`: expected %d bits; got %d.",
          value.ToString(), enum_type->ToString(), expected_bit_count,
          actual_bit_count));
    }
    return InterpValue::MakeEnum(value.GetBitsOrDie(), enum_type->is_signed(),
                                 &enum_type->nominal_type());
  }
  return absl::UnimplementedError("Cannot sign convert type: " +
                                  type.ToString());
}

absl::StatusOr<std::vector<InterpValue>> SignConvertArgs(
    const FunctionType& fn_type, absl::Span<const InterpValue> args) {
  absl::Span<const std::unique_ptr<Type>> params = fn_type.params();
  XLS_RET_CHECK_EQ(params.size(), args.size());
  std::vector<InterpValue> converted;
  converted.reserve(args.size());
  for (int64_t i = 0; i < args.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(InterpValue value,
                         SignConvertValue(*params[i], args[i]));
    converted.push_back(value);
  }
  return converted;
}

namespace {

// Nested sums are validated by their outermost owning sum after restoration;
// validating every intermediate subtree would make a linear chain quadratic.
absl::StatusOr<InterpValue> ValueToInterpValueImpl(const Value& v,
                                                   const Type* type) {
  if (type != nullptr && type->IsSum()) {
    const SumType& sum_type = type->AsSum();
    if (v.kind() != ValueKind::kTuple || v.elements().size() != 2 ||
        v.elements().at(1).kind() != ValueKind::kTuple) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Raw value for semantic sum `%s` must be a tuple containing a tag "
          "and a payload tuple.",
          sum_type.nominal_type().identifier()));
    }
    if (v.elements().at(1).elements().size() != 1) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Raw value for semantic sum `%s` must contain %d payload slots; got "
          "%d.",
          sum_type.nominal_type().identifier(), 1,
          v.elements().at(1).elements().size()));
    }
    XLS_ASSIGN_OR_RETURN(InterpValue tag,
                         ValueToInterpValueImpl(v.elements().at(0), nullptr));
    XLS_ASSIGN_OR_RETURN(
        InterpValue payload_slot,
        ValueToInterpValueImpl(v.elements().at(1).elements().at(0), nullptr));
    if (!tag.IsUBits() || !payload_slot.IsUBits()) {
      return absl::InvalidArgumentError(
          "Expected raw sum tag and payload slot to be unsigned bits.");
    }
    return DecodeRawSumValue(sum_type, tag, payload_slot);
  }

  switch (v.kind()) {
    case ValueKind::kToken:
      return InterpValue::MakeToken();
    case ValueKind::kBits: {
      InterpValueTag tag = InterpValueTag::kUBits;
      if (type != nullptr) {
        if (type->IsEnum()) {
          const EnumType& enum_type = type->AsEnum();
          return InterpValue::MakeEnum(v.bits(), enum_type.is_signed(),
                                       &enum_type.nominal_type());
        }
        std::optional<BitsLikeProperties> bits_like = GetBitsLike(*type);
        XLS_RET_CHECK(bits_like.has_value())
            << "IR value: " << v
            << " kind is bits but type is not bits-like: " << type->ToString();
        XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());
        tag = is_signed ? InterpValueTag::kSBits : InterpValueTag::kUBits;
      }
      return InterpValue::MakeBits(tag, v.bits());
    }
    case ValueKind::kArray:
    case ValueKind::kTuple: {
      if (type != nullptr) {
        if (v.kind() == ValueKind::kArray) {
          const auto* array_type = dynamic_cast<const ArrayType*>(type);
          if (array_type == nullptr) {
            return absl::InvalidArgumentError(absl::StrFormat(
                "Raw array value does not match expected type `%s`.",
                type->ToString()));
          }
          XLS_ASSIGN_OR_RETURN(int64_t expected_size,
                               array_type->size().GetAsInt64());
          if (v.elements().size() != expected_size) {
            return absl::InvalidArgumentError(absl::StrFormat(
                "Raw array for `%s` expected %d elements; got %d.",
                type->ToString(), expected_size, v.elements().size()));
          }
        } else {
          int64_t expected_size;
          if (const auto* struct_type =
                  dynamic_cast<const StructTypeBase*>(type)) {
            expected_size = struct_type->size();
          } else if (const auto* tuple_type =
                         dynamic_cast<const TupleType*>(type)) {
            expected_size = tuple_type->size();
          } else {
            return absl::InvalidArgumentError(absl::StrFormat(
                "Raw tuple value does not match expected type `%s`.",
                type->ToString()));
          }
          if (v.elements().size() != expected_size) {
            return absl::InvalidArgumentError(absl::StrFormat(
                "Raw tuple for `%s` expected %d elements; got %d.",
                type->ToString(), expected_size, v.elements().size()));
          }
        }
      }
      auto get_type = [&](int64_t i) -> const Type* {
        if (type == nullptr) {
          return nullptr;
        }
        if (v.kind() == ValueKind::kArray) {
          auto* array_type = dynamic_cast<const ArrayType*>(type);
          CHECK(array_type != nullptr);
          return &array_type->element_type();
        }
        CHECK(v.kind() == ValueKind::kTuple);
        // Tuple values can come from tuples, structs, or struct-like procs.
        if (auto* struct_type = dynamic_cast<const StructTypeBase*>(type)) {
          return &struct_type->GetMemberType(i);
        }
        auto* tuple_type = dynamic_cast<const TupleType*>(type);
        CHECK(tuple_type != nullptr);
        return &tuple_type->GetMemberType(i);
      };
      std::vector<InterpValue> members;
      for (int64_t i = 0; i < v.elements().size(); ++i) {
        const Value& e = v.elements()[i];
        XLS_ASSIGN_OR_RETURN(InterpValue iv,
                             ValueToInterpValueImpl(e, get_type(i)));
        members.push_back(iv);
      }
      if (v.kind() == ValueKind::kTuple) {
        return InterpValue::MakeTuple(std::move(members));
      }
      return InterpValue::MakeArray(std::move(members));
    }
    default:
      return absl::InvalidArgumentError(
          "Cannot convert IR value to interpreter value: " + v.ToString());
  }
}

}  // namespace

absl::StatusOr<InterpValue> ValueToInterpValue(const Value& v,
                                               const Type* type) {
  return ValueToInterpValueImpl(v, type);
}

absl::StatusOr<std::vector<InterpValue>> ParseArgs(std::string_view args_text) {
  args_text = absl::StripAsciiWhitespace(args_text);
  std::vector<InterpValue> args;
  if (args_text.empty()) {
    return args;
  }
  for (std::string_view piece : absl::StrSplit(args_text, ';')) {
    piece = absl::StripAsciiWhitespace(piece);
    XLS_ASSIGN_OR_RETURN(InterpValue value, InterpValueFromString(piece));
    args.push_back(value);
  }
  return args;
}

absl::StatusOr<std::vector<std::vector<InterpValue>>> ParseArgsBatch(
    std::string_view args_text) {
  args_text = absl::StripAsciiWhitespace(args_text);
  std::vector<std::vector<InterpValue>> args_batch;
  if (args_text.empty()) {
    return args_batch;
  }
  for (std::string_view line : absl::StrSplit(args_text, '\n')) {
    XLS_ASSIGN_OR_RETURN(auto args, ParseArgs(line));
    args_batch.push_back(std::move(args));
  }
  return args_batch;
}

absl::StatusOr<std::string> InterpValueAsString(const InterpValue& v) {
  if (!v.IsArray()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "InterpValue must be an array of u8s, got %s", v.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements, v.GetValues());
  std::string result;
  result.reserve(elements->size() + 1);
  for (const InterpValue& element : *elements) {
    XLS_RET_CHECK(element.IsBits() && element.FitsInNBitsUnsigned(8))
        << "Array elements must be u8.";
    XLS_ASSIGN_OR_RETURN(int64_t element_byte,
                         element.GetBitsOrDie().ToInt64());
    result.push_back(static_cast<uint8_t>(element_byte));
  }
  return result;
}

absl::StatusOr<InterpValue> CreateChannelReference(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator) {
  if (auto* array_type = dynamic_cast<const ArrayType*>(type)) {
    XLS_ASSIGN_OR_RETURN(int dim_int, array_type->size().GetAsInt64());
    std::vector<InterpValue> elements;
    elements.reserve(dim_int);
    for (int i = 0; i < dim_int; i++) {
      XLS_ASSIGN_OR_RETURN(InterpValue element,
                           CreateChannelReference(&array_type->element_type(),
                                                  channel_instance_allocator));
      elements.push_back(element);
    }
    return InterpValue::MakeArray(elements);
  }

  // `type` must be either an array or ChannelType.
  const ChannelType* ct = dynamic_cast<const ChannelType*>(type);
  XLS_RET_CHECK_NE(ct, nullptr);
  std::optional<int64_t> channel_instance_id =
      channel_instance_allocator.has_value()
          ? std::make_optional((*channel_instance_allocator)())
          : std::nullopt;
  return InterpValue::MakeChannelReference(ct->direction(),
                                           channel_instance_id);
}

absl::StatusOr<std::pair<InterpValue, InterpValue>> CreateChannelReferencePair(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator,
    std::optional<const AstNode*> definer) {
  if (auto* array_type = dynamic_cast<const ArrayType*>(type)) {
    XLS_ASSIGN_OR_RETURN(int dim_int, array_type->size().GetAsInt64());
    std::vector<InterpValue> lhs_elements;
    std::vector<InterpValue> rhs_elements;
    lhs_elements.reserve(dim_int);
    rhs_elements.reserve(dim_int);
    for (int i = 0; i < dim_int; i++) {
      XLS_ASSIGN_OR_RETURN(
          auto lhs_rhs,
          CreateChannelReferencePair(&array_type->element_type(),
                                     channel_instance_allocator, definer));
      lhs_elements.push_back(lhs_rhs.first);
      rhs_elements.push_back(lhs_rhs.second);
    }
    int64_t array_id = (*channel_instance_allocator)();
    return std::make_pair(
        InterpValue::MakeChannelArray(ChannelDirection::kOut, array_id,
                                      definer.has_value() ? *definer : nullptr,
                                      lhs_elements),
        InterpValue::MakeChannelArray(ChannelDirection::kIn, array_id,
                                      definer.has_value() ? *definer : nullptr,
                                      rhs_elements));
  }

  // `type` must be either an array or ChannelType.
  const ChannelType* ct = dynamic_cast<const ChannelType*>(type);
  XLS_RET_CHECK_NE(ct, nullptr)
      << "Expected channel type but got: " << type->ToString();
  std::optional<int64_t> channel_instance_id =
      channel_instance_allocator.has_value()
          ? std::make_optional((*channel_instance_allocator)())
          : std::nullopt;
  return std::make_pair(
      InterpValue::MakeChannelReference(ChannelDirection::kOut,
                                        channel_instance_id, definer),
      InterpValue::MakeChannelReference(ChannelDirection::kIn,
                                        channel_instance_id, definer));
}

absl::StatusOr<InterpValue> CreateChannelReferenceOrArray(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator,
    std::optional<const AstNode*> definer) {
  if (type->IsArray()) {
    const ArrayType& array_type = type->AsArray();
    XLS_ASSIGN_OR_RETURN(int size, array_type.size().GetAsInt64());
    std::vector<InterpValue> elements;
    elements.reserve(size);
    for (int i = 0; i < size; i++) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue element,
          CreateChannelReferenceOrArray(&array_type.element_type(),
                                        channel_instance_allocator, definer));
      elements.push_back(element);
    }
    int64_t array_id = channel_instance_allocator.has_value()
                           ? (*channel_instance_allocator)()
                           : 0;
    std::optional<const ChannelType*> ct =
        array_type.GetDirectOrElementChannelType();
    ChannelDirection direction =
        ct.has_value() ? (*ct)->direction() : ChannelDirection::kIn;
    return InterpValue::MakeChannelArray(
        direction, array_id, definer.has_value() ? *definer : nullptr,
        elements);
  }

  XLS_RET_CHECK(type->IsChannel())
      << "Expected channel type but got: " << type->ToString();
  const ChannelType& ct = type->AsChannel();
  std::optional<int64_t> channel_instance_id =
      channel_instance_allocator.has_value()
          ? std::make_optional((*channel_instance_allocator)())
          : std::nullopt;
  return InterpValue::MakeChannelReference(ct.direction(), channel_instance_id,
                                           definer);
}

const AstNode* GetChannelOrArrayDefiner(const InterpValue& channel_or_array) {
  return channel_or_array.IsChannelArray()
             ? channel_or_array.GetChannelArrayOrDie().definer()
             : channel_or_array.GetChannelReferenceOrDie()
                   .GetDefiner()
                   .value_or(nullptr);
}

int64_t GetChannelOrArrayId(const InterpValue& channel_or_array) {
  return channel_or_array.IsChannelArray()
             ? channel_or_array.GetChannelArrayOrDie().channel_array_id()
             : *channel_or_array.GetChannelReferenceOrDie().GetChannelId();
}

ChannelDirection GetChannelOrArrayDirection(
    const InterpValue& channel_or_array) {
  return channel_or_array.IsChannelArray()
             ? channel_or_array.GetChannelArrayOrDie().direction()
             : channel_or_array.GetChannelReferenceOrDie().GetDirection();
}

std::vector<InterpValue> GetLeafChannelReferences(
    const InterpValue& channel_or_array) {
  std::vector<InterpValue> leaves;
  CollectLeafChannelReferences(channel_or_array, leaves);
  return leaves;
}

absl::StatusOr<std::string> FormatInterpValue(const InterpValue& value,
                                              FormatPreference preference) {
  if (value.IsBits()) {
    return BitsToString(value.GetBitsOrDie(), preference);
  }
  return value.ToString();
}

}  // namespace xls::dslx
