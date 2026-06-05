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

#include "xls/dslx/interp_value_internal_utils.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"

namespace xls::dslx::internal {

InterpValue CreateEncodedSumTuple(InterpValue tag,
                                  std::vector<InterpValue> payload_slots) {
  return InterpValue::MakeTuple(
      {std::move(tag), InterpValue::MakeTuple(std::move(payload_slots))});
}

namespace {

absl::StatusOr<InterpValue> CreateInternalPlaceholderValueForSum(
    const SumType& type) {
  const Phase1SumTypeEncoding encoding(type);
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  if (type.variant_count() == 0) {
    return CreateEncodedSumTuple(InterpValue::MakeUBits(tag_bit_count, 0), {});
  }

  const SumTypeVariant& first_variant = type.variants().front();
  XLS_ASSIGN_OR_RETURN(
      Phase1SumTypeEncoding::VariantInfo variant,
      encoding.GetVariant(first_variant.variant().identifier()));

  std::vector<InterpValue> payload_slots;
  payload_slots.reserve(encoding.payload_slot_count());
  XLS_RETURN_IF_ERROR(encoding.VisitPayloadAssemblyOrder(
      variant,
      [&](int64_t active_index) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(
            InterpValue placeholder,
            CreateInternalPlaceholderValueFromType(
                variant.variant->GetMemberType(active_index)));
        payload_slots.push_back(std::move(placeholder));
        return absl::OkStatus();
      },
      [&](const Type& inactive_type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(
            InterpValue placeholder,
            CreateInternalPlaceholderValueFromType(inactive_type));
        payload_slots.push_back(std::move(placeholder));
        return absl::OkStatus();
      }));

  return CreateEncodedSumTuple(
      InterpValue::MakeUBits(tag_bit_count, variant.variant_index),
      std::move(payload_slots));
}

}  // namespace

absl::StatusOr<InterpValue> CreateInternalPlaceholderValueFromType(
    const Type& type) {
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
    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(tuple_type->size());
    for (int64_t i = 0; i < tuple_type->size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateInternalPlaceholderValueFromType(tuple_type->GetMemberType(i)));
      zero_elements.push_back(std::move(zero_element));
    }
    return InterpValue::MakeTuple(std::move(zero_elements));
  }

  if (auto* struct_type = dynamic_cast<const StructType*>(&type)) {
    std::vector<InterpValue> zero_elements;
    zero_elements.reserve(struct_type->size());
    for (int64_t i = 0; i < struct_type->size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue zero_element,
          CreateInternalPlaceholderValueFromType(
              struct_type->GetMemberType(i)));
      zero_elements.push_back(std::move(zero_element));
    }
    return InterpValue::MakeTuple(std::move(zero_elements));
  }

  if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type->size().GetAsInt64());
    if (array_size == 0) {
      return InterpValue::MakeArray({});
    }

    XLS_ASSIGN_OR_RETURN(
        InterpValue zero_element,
        CreateInternalPlaceholderValueFromType(array_type->element_type()));
    std::vector<InterpValue> zero_elements(array_size, zero_element);
    return InterpValue::MakeArray(std::move(zero_elements));
  }

  if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    if (!enum_type->members().empty()) {
      return enum_type->members().at(0);
    }
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, enum_type->size().GetAsInt64());
    return InterpValue::MakeEnum(Bits(bit_count), enum_type->is_signed(),
                                 &enum_type->nominal_type());
  }

  if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    return CreateInternalPlaceholderValueForSum(*sum_type);
  }

  return absl::UnimplementedError(
      absl::StrCat("Cannot create internal placeholder value for type: ",
                   type.ToString()));
}

}  // namespace xls::dslx::internal
