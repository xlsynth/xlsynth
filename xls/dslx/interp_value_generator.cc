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

#include "xls/dslx/interp_value_generator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

absl::StatusOr<InterpValue> GenerateUniformBits(
    absl::BitGenRef bit_gen, const BitsLikeProperties& bits_like,
    absl::Span<const InterpValue> prior) {
  XLS_ASSIGN_OR_RETURN(int64_t bit_count, bits_like.size.GetAsInt64());
  XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like.is_signed.GetAsBool());
  InlineBitmap bitmap(bit_count);
  for (int64_t word = 0; word < bitmap.word_count(); ++word) {
    bitmap.SetWord(word, absl::Uniform<uint64_t>(bit_gen));
  }
  return InterpValue::MakeBits(is_signed, Bits::FromBitmap(std::move(bitmap)));
}

}  // namespace

absl::StatusOr<InterpValue> InterpValueGenerator::GenerateSum(
    absl::BitGenRef bit_gen, const SumType& sum_type,
    absl::Span<const InterpValue> prior,
    InterpValueBitsGenerator bits_generator) {
  auto cached = sum_generation_info_.find(&sum_type);
  if (cached == sum_generation_info_.end()) {
    std::vector<int64_t> inhabited_variant_indices;
    inhabited_variant_indices.reserve(sum_type.variants().size());
    for (int64_t i = 0; i < sum_type.variants().size(); ++i) {
      XLS_ASSIGN_OR_RETURN(bool variant_is_inhabited,
                           SumVariantIsInhabited(sum_type.variants().at(i)));
      if (variant_is_inhabited) {
        inhabited_variant_indices.push_back(i);
      }
    }
    if (inhabited_variant_indices.empty()) {
      return absl::InvalidArgumentError(
          sum_type.variants().empty()
              ? "Cannot generate an InterpValue for an empty sum type."
              : "Cannot generate an InterpValue for an uninhabited sum type.");
    }
    XLS_ASSIGN_OR_RETURN(TypeDim payload_slot_bit_count,
                         sum_type.GetMaxPayloadBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t concrete_payload_slot_bit_count,
                         payload_slot_bit_count.GetAsInt64());
    cached = sum_generation_info_
                 .emplace(&sum_type,
                          SumGenerationInfo{
                              .inhabited_variant_indices =
                                  std::move(inhabited_variant_indices),
                              .payload_slot_bit_count =
                                  concrete_payload_slot_bit_count,
                          })
                 .first;
  }
  const std::vector<int64_t>& inhabited_variant_indices =
      cached->second.inhabited_variant_indices;
  const int64_t variant_index = inhabited_variant_indices.at(
      absl::Uniform(bit_gen, size_t{0}, inhabited_variant_indices.size()));
  // Recursive generation may insert another sum type and invalidate `cached`.
  const int64_t payload_slot_bit_count = cached->second.payload_slot_bit_count;
  const SumTypeVariant& variant = sum_type.variants().at(variant_index);
  std::vector<InterpValue> payload_values;
  payload_values.reserve(variant.size());
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue member_value,
        Generate(bit_gen, variant.GetMemberType(i), prior, bits_generator));
    payload_values.push_back(std::move(member_value));
  }
  return internal::CreateSumValueFromValidatedGeneratedPayload(
      sum_type, variant_index, payload_slot_bit_count, payload_values);
}

absl::StatusOr<InterpValue> InterpValueGenerator::Generate(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior,
    InterpValueBitsGenerator bits_generator) {
  XLS_RET_CHECK(!type.IsMeta()) << type.ToString();
  XLS_RET_CHECK(dynamic_cast<const BitsConstructorType*>(&type) == nullptr)
      << "`BitsConstructorType`s are not valid InterpValue types.";

  if (type.IsToken()) {
    return InterpValue::MakeToken();
  } else if (auto* channel_type = dynamic_cast<const ChannelType*>(&type)) {
    return Generate(bit_gen, channel_type->payload_type(), prior,
                    bits_generator);
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    std::vector<InterpValue> members;
    members.reserve(tuple_type->size());
    for (const std::unique_ptr<Type>& member_type : tuple_type->members()) {
      XLS_ASSIGN_OR_RETURN(InterpValue member, Generate(bit_gen, *member_type,
                                                        prior, bits_generator));
      members.push_back(std::move(member));
    }
    return InterpValue::MakeTuple(std::move(members));
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    std::vector<InterpValue> members;
    members.reserve(struct_type->size());
    for (const std::unique_ptr<Type>& member_type : struct_type->members()) {
      XLS_ASSIGN_OR_RETURN(InterpValue member, Generate(bit_gen, *member_type,
                                                        prior, bits_generator));
      members.push_back(std::move(member));
    }
    return InterpValue::MakeTuple(std::move(members));
  } else if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    return GenerateSum(bit_gen, *sum_type, prior, bits_generator);
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    if (enum_type->members().empty()) {
      return absl::InvalidArgumentError(
          "Cannot generate an InterpValue for an empty enum type.");
    }
    const size_t member_index =
        absl::Uniform(bit_gen, size_t{0}, enum_type->members().size());
    return CastBitsToEnum(enum_type->members().at(member_index), *enum_type);
  } else if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
             bits_like.has_value()) {
    // Arrays of bits constructors are bits-like and must be handled before
    // ordinary source-language arrays.
    return bits_generator(bit_gen, *bits_like, prior);
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type->size().GetAsInt64());
    std::vector<InterpValue> elements;
    elements.reserve(array_size);
    for (int64_t i = 0; i < array_size; ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue element,
          Generate(bit_gen, array_type->element_type(), prior, bits_generator));
      elements.push_back(std::move(element));
    }
    return InterpValue::MakeArray(std::move(elements));
  } else {
    return absl::UnimplementedError("Unsupported type for GenerateInterpValue");
  }
}

absl::StatusOr<InterpValue> InterpValueGenerator::Generate(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior) {
  return Generate(bit_gen, type, prior, GenerateUniformBits);
}

absl::StatusOr<std::vector<InterpValue>> InterpValueGenerator::GenerateValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types,
    InterpValueBitsGenerator bits_generator) {
  std::vector<InterpValue> values;
  values.reserve(types.size());
  for (const Type* type : types) {
    XLS_RET_CHECK(type != nullptr);
    XLS_ASSIGN_OR_RETURN(InterpValue value,
                         Generate(bit_gen, *type, values, bits_generator));
    values.push_back(std::move(value));
  }
  return values;
}

absl::StatusOr<std::vector<InterpValue>> InterpValueGenerator::GenerateValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types) {
  return GenerateValues(bit_gen, types, GenerateUniformBits);
}

absl::StatusOr<InterpValue> GenerateInterpValue(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior,
    InterpValueBitsGenerator bits_generator) {
  InterpValueGenerator generator;
  return generator.Generate(bit_gen, type, prior, bits_generator);
}

absl::StatusOr<InterpValue> GenerateInterpValue(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior) {
  InterpValueGenerator generator;
  return generator.Generate(bit_gen, type, prior);
}

absl::StatusOr<std::vector<InterpValue>> GenerateInterpValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types,
    InterpValueBitsGenerator bits_generator) {
  InterpValueGenerator generator;
  return generator.GenerateValues(bit_gen, types, bits_generator);
}

absl::StatusOr<std::vector<InterpValue>> GenerateInterpValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types) {
  InterpValueGenerator generator;
  return generator.GenerateValues(bit_gen, types);
}

}  // namespace xls::dslx
