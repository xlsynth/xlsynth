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

#include "xls/dslx/sum_type_encoding.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/status_macros.h"

namespace xls::dslx {

SumTypeEncoding::SumTypeEncoding(const SumType& type) : type_(type) {
  int64_t payload_start = 0;
  variants_.reserve(type_.variants().size());
  for (int64_t variant_index = 0; variant_index < type_.variants().size();
       ++variant_index) {
    const SumTypeVariant& variant = type_.variants().at(variant_index);
    variants_.push_back(StoredVariant{
        .variant_index = variant_index,
        .variant = &variant,
        .payload_start = payload_start,
    });
    for (const std::unique_ptr<Type>& member : variant.payload_members()) {
      payload_slot_types_.push_back(member.get());
    }
    payload_start += variant.size();
  }
}

absl::StatusOr<int64_t> SumTypeEncoding::tag_bit_count() const {
  return type_.tag_bit_count().GetAsInt64();
}

absl::StatusOr<SumTypeEncoding::VariantInfo> SumTypeEncoding::GetVariant(
    std::string_view variant_name) const {
  XLS_ASSIGN_OR_RETURN(const StoredVariant * variant, FindVariant(variant_name));
  return ToVariantInfo(*variant);
}

absl::Status SumTypeEncoding::ForEachVariant(
    absl::FunctionRef<absl::Status(const VariantInfo& variant)> visitor) const {
  for (const StoredVariant& variant : variants_) {
    absl::Status status = visitor(ToVariantInfo(variant));
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status SumTypeEncoding::ForEachStoredLeafType(
    absl::FunctionRef<absl::Status(const StoredLeafInfo& leaf)> visitor) const {
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, this->tag_bit_count());
  BitsType tag_type(/*is_signed=*/false, tag_bit_count);
  XLS_RETURN_IF_ERROR(visitor(StoredLeafInfo{
      .type = &tag_type,
      .dense_max_value = type_.variant_count() - 1,
  }));
  return ForEachPayloadType([&](const Type& type) -> absl::Status {
    return visitor(StoredLeafInfo{
        .type = &type,
        .dense_max_value = std::nullopt,
    });
  });
}

absl::Status SumTypeEncoding::ForEachPayloadType(
    absl::FunctionRef<absl::Status(const Type& type)> visitor) const {
  for (const Type* payload_type : payload_slot_types_) {
    absl::Status status = visitor(*payload_type);
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status SumTypeEncoding::VisitPayloadAssemblyOrder(
    const VariantInfo& variant,
    absl::FunctionRef<absl::Status(int64_t active_index)> active_visitor,
    absl::FunctionRef<absl::Status(const Type& inactive_type)>
        inactive_visitor) const {
  XLS_ASSIGN_OR_RETURN(
      const StoredVariant * stored_variant,
      FindVariant(variant.variant->variant().identifier()));

  int64_t active_index = 0;
  for (int64_t slot_index = 0; slot_index < payload_slot_types_.size();
       ++slot_index) {
    const bool is_active =
        slot_index >= stored_variant->payload_start &&
        slot_index < stored_variant->payload_start + variant.payload_size();
    absl::Status status =
        is_active ? active_visitor(active_index++)
                  : inactive_visitor(*payload_slot_types_.at(slot_index));
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status SumTypeEncoding::ForEachActivePayloadSlot(
    const VariantInfo& variant,
    absl::FunctionRef<absl::Status(int64_t slot_index, int64_t active_index,
                                   const Type& type)> visitor) const {
  XLS_ASSIGN_OR_RETURN(
      const StoredVariant * stored_variant,
      FindVariant(variant.variant->variant().identifier()));

  int64_t active_index = 0;
  for (int64_t slot_index = stored_variant->payload_start;
       slot_index < stored_variant->payload_start + variant.payload_size();
       ++slot_index) {
    absl::Status status =
        visitor(slot_index, active_index++, *payload_slot_types_.at(slot_index));
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

SumTypeEncoding::VariantInfo SumTypeEncoding::ToVariantInfo(
    const StoredVariant& variant) {
  return VariantInfo{
      .variant_index = variant.variant_index,
      .variant = variant.variant,
  };
}

absl::StatusOr<const SumTypeEncoding::StoredVariant*> SumTypeEncoding::FindVariant(
    std::string_view variant_name) const {
  for (const StoredVariant& variant : variants_) {
    if (variant.variant->variant().identifier() == variant_name) {
      return &variant;
    }
  }
  return absl::NotFoundError(
      absl::StrCat("No variant `", variant_name, "` in sum `",
                   type_.nominal_type().identifier(), "`."));
}

}  // namespace xls::dslx
