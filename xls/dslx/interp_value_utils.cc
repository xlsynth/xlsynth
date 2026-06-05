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
#include "xls/dslx/interp_value_internal_utils.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/value.h"

namespace xls::dslx {

namespace {

absl::StatusOr<InterpValue> InterpValueFromString(std::string_view s) {
  XLS_ASSIGN_OR_RETURN(Value value, Parser::ParseTypedValue(s));
  return dslx::ValueToInterpValue(value);
}

absl::Status ValidateBitsLikeValue(const InterpValue& value, const Type& type,
                                   const BitsLikeProperties& bits_like);

absl::Status ValidateEnumValue(const InterpValue& value,
                               const EnumType& enum_type);

absl::StatusOr<int64_t> GetFlattenedBitCount(const Type& type) {
  XLS_ASSIGN_OR_RETURN(TypeDim bit_count, type.GetTotalBitCount());
  return bit_count.GetAsInt64();
}

absl::StatusOr<Bits> FlattenValueForType(const Type& type,
                                         const InterpValue& value);

absl::StatusOr<InterpValue> UnflattenValueForType(const Type& type,
                                                  const Bits& bits);

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
        UnflattenValueForType(*member, bits.Slice(bit_offset, member_bit_count)));
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
          "Expected encoded sum tag and shared payload slot to be unsigned bits.");
    }
    return bits_ops::Concat(
        {sum_view.tag.GetBitsOrDie(), sum_view.payload_slot.GetBitsOrDie()});
  }
  if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    if (!value.IsTuple()) {
      return absl::InvalidArgumentError("Expected tuple value while flattening.");
    }
    return FlattenAggregateMembers(tuple_type->members(),
                                   value.GetValuesOrDie());
  }
  if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    if (!value.IsTuple()) {
      return absl::InvalidArgumentError("Expected struct value while flattening.");
    }
    return FlattenAggregateMembers(struct_type->members(),
                                   value.GetValuesOrDie());
  }
  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    if (!value.IsArray()) {
      return absl::InvalidArgumentError("Expected array value while flattening.");
    }
    std::vector<Bits> flattened_elements;
    flattened_elements.reserve(value.GetValuesOrDie().size());
    for (int64_t i = static_cast<int64_t>(value.GetValuesOrDie().size()) - 1;
         i >= 0; --i) {
      XLS_ASSIGN_OR_RETURN(
          Bits flattened_element,
          FlattenValueForType(array_type->element_type(),
                              value.GetValuesOrDie().at(i)));
      flattened_elements.push_back(std::move(flattened_element));
    }
    return bits_ops::Concat(flattened_elements);
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    XLS_RETURN_IF_ERROR(ValidateEnumValue(value, *enum_type));
    return value.GetBitsOrDie();
  }
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    XLS_RETURN_IF_ERROR(ValidateBitsLikeValue(value, type, *bits_like));
    return value.GetBitsOrDie();
  }
  return absl::UnimplementedError(
      absl::StrCat("Cannot flatten InterpValue for type: ", type.ToString()));
}

absl::StatusOr<InterpValue> UnflattenValueForType(const Type& type,
                                                  const Bits& bits) {
  XLS_ASSIGN_OR_RETURN(int64_t expected_bit_count, GetFlattenedBitCount(type));
  if (bits.bit_count() != expected_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Cannot unflatten `%s`: expected %d bits; got %d.", type.ToString(),
        expected_bit_count, bits.bit_count()));
  }

  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    const Phase1SumTypeEncoding encoding(*sum_type);
    XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                         encoding.payload_slot_bit_count());
    XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
    return DecodeRawSumValue(
        *sum_type,
        InterpValue::MakeUnsigned(
            bits.Slice(payload_slot_bit_count, tag_bit_count)),
        InterpValue::MakeUnsigned(bits.Slice(0, payload_slot_bit_count)));
  }
  if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> members,
                         UnflattenAggregateMembers(tuple_type->members(), bits));
    return InterpValue::MakeTuple(std::move(members));
  }
  if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> members,
                         UnflattenAggregateMembers(struct_type->members(), bits));
    return InterpValue::MakeTuple(std::move(members));
  }
  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type->size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t element_bit_count,
                         GetFlattenedBitCount(array_type->element_type()));
    std::vector<InterpValue> elements;
    elements.reserve(array_size);
    for (int64_t i = 0; i < array_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue element,
          UnflattenValueForType(array_type->element_type(),
                                bits.Slice(i * element_bit_count,
                                           element_bit_count)));
      elements.push_back(std::move(element));
    }
    return InterpValue::MakeArray(std::move(elements));
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return InterpValue::MakeEnum(bits, enum_type->is_signed(),
                                 &enum_type->nominal_type());
  }
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());
    return InterpValue::MakeBits(is_signed, bits);
  }
  return absl::UnimplementedError(
      absl::StrCat("Cannot unflatten InterpValue for type: ", type.ToString()));
}

absl::StatusOr<std::vector<InterpValue>> DecodeVariantPayloadValues(
    const Phase1SumTypeEncoding::VariantInfo& variant,
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
    XLS_ASSIGN_OR_RETURN(
        InterpValue payload_value,
        UnflattenValueForType(
            member_type, active_payload_bits.Slice(bit_offset, member_bit_count)));
    payload_values.push_back(std::move(payload_value));
  }
  return payload_values;
}

absl::Status ValidateEncodedSumShape(const InterpValue& value,
                                     const SumType& sum_type) {
  XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                       internal::GetEncodedSumView(value));
  const Phase1SumTypeEncoding encoding(sum_type);

  if (!sum_view.tag.IsUBits()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Expected sum tag for `%s` to be unsigned bits; got `%s`.",
        sum_type.ToString(), sum_view.tag.ToString()));
  }
  XLS_ASSIGN_OR_RETURN(int64_t expected_tag_bit_count, encoding.tag_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t actual_tag_bit_count, sum_view.tag.GetBitCount());
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
  XLS_ASSIGN_OR_RETURN(int64_t expected_payload_bit_count,
                       encoding.payload_slot_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t actual_payload_bit_count,
                       sum_view.payload_slot.GetBitCount());
  if (actual_payload_bit_count != expected_payload_bit_count) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum `%s` expected a %d-bit payload slot; got %d bits.",
        sum_type.ToString(), expected_payload_bit_count,
        actual_payload_bit_count));
  }
  return absl::OkStatus();
}

absl::StatusOr<InterpValue> DecodeRawSumValue(const SumType& sum_type,
                                              const InterpValue& tag,
                                              const InterpValue& payload_slot) {
  InterpValue raw_value =
      internal::CreateEncodedSumTuple(tag, payload_slot);
  XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(raw_value, sum_type));

  const Phase1SumTypeEncoding encoding(sum_type);
  absl::StatusOr<Phase1SumTypeEncoding::VariantInfo> variant =
      encoding.GetVariantByTagBits(tag.GetBitsOrDie());
  if (variant.status().code() == absl::StatusCode::kNotFound) {
    return raw_value;
  }
  XLS_ASSIGN_OR_RETURN(Phase1SumTypeEncoding::VariantInfo valid_variant,
                       std::move(variant));
  XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> payload_values,
                       DecodeVariantPayloadValues(valid_variant, payload_slot));
  return CreateSumValue(sum_type, valid_variant.variant->variant().identifier(),
                        payload_values);
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

absl::Status ValidateEnumValue(const InterpValue& value,
                               const EnumType& enum_type) {
  if (!value.IsEnum()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected enum-typed value for `%s`; got `%s`.",
                        enum_type.ToString(), value.ToString()));
  }
  InterpValue::EnumData enum_data = value.GetEnumData().value();
  if (enum_data.def != &enum_type.nominal_type()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Value `%s` does not match enum `%s`.", value.ToString(),
        enum_type.ToString()));
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
  XLS_ASSIGN_OR_RETURN(std::vector<Bits> declared_members,
                       GetDeclaredEnumMemberBits(enum_type));
  for (const Bits& member_bits : declared_members) {
    if (enum_data.value == member_bits) {
      return absl::OkStatus();
    }
  }
  return absl::InvalidArgumentError(absl::StrFormat(
      "Value `%s` does not match enum `%s`: expected a declared member.",
      value.ToString(), enum_type.ToString()));
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
    XLS_RETURN_IF_ERROR(
        ValidateInterpValueMatchesType(elements.at(i), tuple_type.GetMemberType(i)));
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

absl::Status ValidateSumValue(const InterpValue& value, const SumType& sum_type) {
  XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                       internal::GetEncodedSumView(value));
  const Phase1SumTypeEncoding encoding(sum_type);
  XLS_RETURN_IF_ERROR(ValidateEncodedSumShape(value, sum_type));

  XLS_ASSIGN_OR_RETURN(
      Phase1SumTypeEncoding::VariantInfo variant,
      encoding.GetVariantByTagBits(sum_view.tag.GetBitsOrDie()));
  const SumTypeVariant& variant_def = *variant.variant;
  XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> active_payload_values,
                       DecodeVariantPayloadValues(variant, sum_view.payload_slot));
  XLS_ASSIGN_OR_RETURN(InterpValue canonical,
                       CreateSumValue(sum_type, variant_def.variant().identifier(),
                                      active_payload_values));
  if (value.Ne(canonical)) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum `%s` is not in canonical encoded form for variant `%s`.",
        sum_type.ToString(), variant_def.variant().identifier()));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status ValidateInterpValueMatchesType(const InterpValue& value,
                                            const Type& type) {
  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    return ValidateSumValue(value, *sum_type);
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    return ValidateTupleValue(value, *tuple_type);
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    return ValidateStructValue(value, *struct_type);
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    return ValidateArrayValue(value, *array_type);
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return ValidateEnumValue(value, *enum_type);
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    return ValidateBitsLikeValue(value, type, *bits_like);
  }
  return absl::UnimplementedError(
      absl::StrCat("Cannot validate InterpValue against type: ",
                   type.ToString()));
}

absl::StatusOr<std::vector<Bits>> GetDeclaredEnumMemberBits(
    const EnumType& enum_type) {
  std::vector<Bits> members;
  members.reserve(enum_type.members().size());
  for (const InterpValue& member : enum_type.members()) {
    members.push_back(member.GetBitsOrDie());
  }
  return members;
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
  return InterpValue::MakeEnum(bits_value.GetBitsOrDie(), bits_value.IsSigned(),
                               &enum_def);
}

absl::StatusOr<InterpValue> CreateZeroValueFromType(const Type& type) {
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, bits_like->size.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());

    if (is_signed) {
      return InterpValue::MakeSBits(bit_count, /*value=*/0);
    }

    return InterpValue::MakeUBits(bit_count, /*value=*/0);
  }

  if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    const int64_t tuple_size = tuple_type->size();

    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(tuple_size);

    for (int64_t i = 0; i < tuple_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateZeroValueFromType(tuple_type->GetMemberType(i)));
      zero_elements.push_back(zero_element);
    }

    return InterpValue::MakeTuple(zero_elements);
  }

  if (auto* struct_type = dynamic_cast<const StructType*>(&type)) {
    const int64_t struct_size = struct_type->size();

    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(struct_size);

    for (int64_t i = 0; i < struct_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateZeroValueFromType(struct_type->GetMemberType(i)));
      zero_elements.push_back(zero_element);
    }

    return InterpValue::MakeTuple(zero_elements);
  }

  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(const int64_t array_size,
                         array_type->size().GetAsInt64());

    if (array_size == 0) {
      return InterpValue::MakeArray({});
    }

    XLS_ASSIGN_OR_RETURN(InterpValue zero_element,
                         CreateZeroValueFromType(array_type->element_type()));
    std::vector<InterpValue> zero_elements(array_size, zero_element);
    return InterpValue::MakeArray(zero_elements);
  }

  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    if (!enum_type->members().empty()) {
      return enum_type->members().at(0);
    }
  }

  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Cannot create zero value for semantic sum type `",
                     sum_type->nominal_type().identifier(), "`."));
  }

  return absl::UnimplementedError("Cannot create zero value for type type: " +
                                  type.ToString());
}

absl::StatusOr<InterpValue> CreateSumValue(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values) {
  const Phase1SumTypeEncoding encoding(type);
  XLS_ASSIGN_OR_RETURN(Phase1SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(variant_name));
  if (payload_values.size() != variant.payload_size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Sum constructor `%s` expected %d payload values; got %d.",
        variant_name, variant.payload_size(), payload_values.size()));
  }
  for (int64_t active_index = 0; active_index < variant.payload_size();
       ++active_index) {
    XLS_RETURN_IF_ERROR(ValidateInterpValueMatchesType(
        payload_values.at(active_index),
        variant.variant->GetMemberType(active_index)));
  }

  std::vector<Bits> flattened_members;
  flattened_members.reserve(payload_values.size());
  XLS_RETURN_IF_ERROR(encoding.ForEachPayloadMember(
      variant, [&](int64_t active_index, const Type& member_type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(
            Bits flattened_member,
            FlattenValueForType(member_type, payload_values.at(active_index)));
        flattened_members.push_back(std::move(flattened_member));
        return absl::OkStatus();
      }));
  Bits payload_bits = bits_ops::Concat(flattened_members);
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  return internal::CreateEncodedSumTuple(
      InterpValue::MakeUnsigned(variant.discriminant->GetBitsOrDie()),
      InterpValue::MakeUnsigned(
          bits_ops::ZeroExtend(std::move(payload_bits), payload_slot_bit_count)));
}

absl::StatusOr<std::vector<InterpValue>> GetSumPayloadValues(
    const SumType& type, const InterpValue& value) {
  XLS_RETURN_IF_ERROR(ValidateSumValue(value, type));
  XLS_ASSIGN_OR_RETURN(internal::EncodedSumView sum_view,
                       internal::GetEncodedSumView(value));
  const Phase1SumTypeEncoding encoding(type);
  XLS_ASSIGN_OR_RETURN(
      Phase1SumTypeEncoding::VariantInfo variant,
      encoding.GetVariantByTagBits(sum_view.tag.GetBitsOrDie()));
  return DecodeVariantPayloadValues(variant, sum_view.payload_slot);
}

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
      XLS_RETURN_IF_ERROR(ValidateEnumValue(value, *enum_type));
      return value;
    }
    XLS_RET_CHECK(value.IsBits()) << value.ToString();
    return CastBitsToEnum(value, *enum_type);
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

absl::StatusOr<InterpValue> ValueToInterpValue(const Value& v,
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
                         ValueToInterpValue(v.elements().at(0), nullptr));
    XLS_ASSIGN_OR_RETURN(
        InterpValue payload_slot,
        ValueToInterpValue(v.elements().at(1).elements().at(0), nullptr));
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
        // Tuple values can either come from tuples or structs. Check for
        // structs first.
        if (auto* struct_type = dynamic_cast<const StructType*>(type)) {
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
                             ValueToInterpValue(e, get_type(i)));
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
    ChannelDirection direction, const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator) {
  if (auto* array_type = dynamic_cast<const ArrayType*>(type)) {
    XLS_ASSIGN_OR_RETURN(int dim_int, array_type->size().GetAsInt64());
    std::vector<InterpValue> elements;
    elements.reserve(dim_int);
    for (int i = 0; i < dim_int; i++) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue element,
          CreateChannelReference(direction, &array_type->element_type(),
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
  return InterpValue::MakeChannelReference(direction, channel_instance_id);
}

absl::StatusOr<std::pair<InterpValue, InterpValue>> CreateChannelReferencePair(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator) {
  if (auto* array_type = dynamic_cast<const ArrayType*>(type)) {
    XLS_ASSIGN_OR_RETURN(int dim_int, array_type->size().GetAsInt64());
    std::vector<InterpValue> lhs_elements;
    std::vector<InterpValue> rhs_elements;
    lhs_elements.reserve(dim_int);
    rhs_elements.reserve(dim_int);
    for (int i = 0; i < dim_int; i++) {
      XLS_ASSIGN_OR_RETURN(
          auto lhs_rhs, CreateChannelReferencePair(&array_type->element_type(),
                                                   channel_instance_allocator));
      lhs_elements.push_back(lhs_rhs.first);
      rhs_elements.push_back(lhs_rhs.second);
    }
    XLS_ASSIGN_OR_RETURN(InterpValue lhs, InterpValue::MakeArray(lhs_elements));
    XLS_ASSIGN_OR_RETURN(InterpValue rhs, InterpValue::MakeArray(rhs_elements));
    return std::make_pair(lhs, rhs);
  }

  // `type` must be either an array or ChannelType.
  const ChannelType* ct = dynamic_cast<const ChannelType*>(type);
  XLS_RET_CHECK_NE(ct, nullptr);
  std::optional<int64_t> channel_instance_id =
      channel_instance_allocator.has_value()
          ? std::make_optional((*channel_instance_allocator)())
          : std::nullopt;
  return std::make_pair(InterpValue::MakeChannelReference(
                            ChannelDirection::kOut, channel_instance_id),
                        InterpValue::MakeChannelReference(ChannelDirection::kIn,
                                                          channel_instance_id));
}

absl::StatusOr<std::string> FormatInterpValue(const InterpValue& value,
                                              FormatPreference preference) {
  if (value.IsBits()) {
    return BitsToString(value.GetBitsOrDie(), preference);
  }
  return value.ToString();
}

}  // namespace xls::dslx
