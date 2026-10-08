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

#include "xls/dslx/sum_type_encoding.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/status_macros.h"

namespace xls::dslx {

SumTypeEncoding::SumTypeEncoding(const SumType& type) : type_(type) {}

absl::StatusOr<int64_t> SumTypeEncoding::VariantInfo::payload_bit_count()
    const {
  XLS_ASSIGN_OR_RETURN(TypeDim payload_bit_count,
                       internal::GetBitCountWithSharedSumPayload(*variant));
  return payload_bit_count.GetAsInt64();
}

absl::StatusOr<int64_t> SumTypeEncoding::payload_slot_bit_count() const {
  XLS_ASSIGN_OR_RETURN(TypeDim payload_bit_count,
                       type_.GetMaxPayloadBitCount());
  return payload_bit_count.GetAsInt64();
}

absl::StatusOr<int64_t> SumTypeEncoding::tag_bit_count() const {
  return type_.tag_bit_count().GetAsInt64();
}

absl::StatusOr<SumTypeEncoding::VariantInfo> SumTypeEncoding::GetVariant(
    std::string_view variant_name) const {
  for (int64_t i = 0; i < type_.variant_count(); ++i) {
    if (type_.variants().at(i).variant().identifier() == variant_name) {
      return GetVariantInfo(i);
    }
  }
  return absl::NotFoundError(
      absl::StrCat("No variant `", variant_name, "` in sum `",
                   type_.nominal_type().identifier(), "`."));
}

absl::StatusOr<SumTypeEncoding::VariantInfo>
SumTypeEncoding::GetVariantByTagBits(const Bits& tag_bits) const {
  for (int64_t i = 0; i < type_.variant_count(); ++i) {
    if (type_.GetDiscriminant(i).GetBitsOrDie() == tag_bits) {
      return GetVariantInfo(i);
    }
  }
  return absl::NotFoundError(
      absl::StrCat("No variant with tag bits `", tag_bits.ToDebugString(),
                   "` in sum `", type_.nominal_type().identifier(), "`."));
}

absl::Status SumTypeEncoding::ForEachVariant(
    absl::FunctionRef<absl::Status(const VariantInfo& variant)> visitor) const {
  for (int64_t i = 0; i < type_.variant_count(); ++i) {
    XLS_RETURN_IF_ERROR(visitor(GetVariantInfo(i)));
  }
  return absl::OkStatus();
}

absl::Status SumTypeEncoding::ForEachPayloadMember(
    const VariantInfo& variant,
    absl::FunctionRef<absl::Status(int64_t active_index, const Type& type)>
        visitor) const {
  XLS_RETURN_IF_ERROR(ValidateVariantInfo(variant));

  for (int64_t active_index = 0; active_index < variant.payload_size();
       ++active_index) {
    XLS_RETURN_IF_ERROR(
        visitor(active_index, variant.variant->GetMemberType(active_index)));
  }
  return absl::OkStatus();
}

SumTypeEncoding::VariantInfo SumTypeEncoding::GetVariantInfo(
    int64_t variant_index) const {
  return VariantInfo(variant_index, type_.variants().at(variant_index),
                     type_.GetDiscriminant(variant_index));
}

absl::Status SumTypeEncoding::ValidateVariantInfo(
    const VariantInfo& variant) const {
  const int64_t variant_count = type_.variant_count();
  if (variant.variant == nullptr) {
    return absl::InvalidArgumentError("VariantInfo has a null variant.");
  } else if (variant.discriminant == nullptr) {
    return absl::InvalidArgumentError("VariantInfo has a null discriminant.");
  } else if (variant.variant_index < 0 ||
             variant.variant_index >= variant_count) {
    return absl::OutOfRangeError(absl::StrCat(
        "Variant index ", variant.variant_index, " is out of range for sum `",
        type_.nominal_type().identifier(), "` with ", variant_count,
        " variants."));
  } else if (&type_.variants().at(variant.variant_index) != variant.variant) {
    return absl::InvalidArgumentError(absl::StrCat(
        "VariantInfo for `", variant.variant->variant().identifier(),
        "` does not belong to sum `", type_.nominal_type().identifier(),
        "` at variant index ", variant.variant_index, "."));
  } else if (type_.GetDiscriminant(variant.variant_index).GetBitsOrDie() !=
             variant.discriminant->GetBitsOrDie()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "VariantInfo for `", variant.variant->variant().identifier(),
        "` does not match discriminant bits for variant index ",
        variant.variant_index, " in sum `", type_.nominal_type().identifier(),
        "`."));
  } else {
    return absl::OkStatus();
  }
}

}  // namespace xls::dslx
