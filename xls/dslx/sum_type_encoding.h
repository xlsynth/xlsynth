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

#ifndef XLS_DSLX_SUM_TYPE_ENCODING_H_
#define XLS_DSLX_SUM_TYPE_ENCODING_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {

// Public, temporary Phase 1 view of the semantic-sum storage encoding.
//
// This deliberately exposes the current `(tag, payload_slots)` lowering layout
// so Phase 1 interpreter, bytecode, exhaustiveness, and IR conversion code can
// share one encoding rule. Do not treat the class name, slot order, or
// tag/payload representation as a stable semantic API: later sum-type phases are
// expected to replace this storage contract when boundary decoding and tagged
// union lowering are implemented.
class Phase1SumTypeEncoding {
 public:
  struct VariantInfo {
    int64_t variant_index;
    const SumTypeVariant* variant;
    // First payload slot for this variant in the flattened Phase 1 payload
    // tuple.
    int64_t payload_start;

    int64_t payload_size() const { return variant->size(); }
    int64_t payload_end() const { return payload_start + payload_size(); }
  };

  class StoredLeafInfo {
   public:
    static StoredLeafInfo MakeDenseTag(BitsType tag_type,
                                       int64_t dense_max_value) {
      return StoredLeafInfo(StoredType(tag_type), dense_max_value);
    }

    static StoredLeafInfo MakePayload(const Type& type) {
      return StoredLeafInfo(StoredType(std::cref(type)), std::nullopt);
    }

    const Type& type() const {
      if (std::holds_alternative<BitsType>(type_)) {
        return std::get<BitsType>(type_);
      }
      return std::get<std::reference_wrapper<const Type>>(type_).get();
    }

    std::optional<int64_t> dense_max_value() const { return dense_max_value_; }

   private:
    using StoredType =
        std::variant<BitsType, std::reference_wrapper<const Type>>;

    StoredLeafInfo(StoredType type, std::optional<int64_t> dense_max_value)
        : type_(type), dense_max_value_(dense_max_value) {}

    StoredType type_;
    std::optional<int64_t> dense_max_value_;
  };

  explicit Phase1SumTypeEncoding(const SumType& type);

  int64_t payload_slot_count() const { return payload_slot_types_.size(); }
  absl::StatusOr<int64_t> tag_bit_count() const;

  absl::StatusOr<VariantInfo> GetVariant(std::string_view variant_name) const;
  absl::Status ForEachVariant(
      absl::FunctionRef<absl::Status(const VariantInfo& variant)> visitor)
      const;
  // Visits the stored leaves for the encoded sum value: one dense tag leaf
  // first, followed by payload slots in canonical storage order. The dense tag
  // leaf is carried by value inside `StoredLeafInfo`; payload leaves borrow the
  // underlying stored payload types.
  absl::Status ForEachStoredLeafType(
      absl::FunctionRef<absl::Status(const StoredLeafInfo& leaf)> visitor)
      const;
  // Visits stored payload slot types in canonical Phase 1 storage order.
  absl::Status ForEachPayloadType(
      absl::FunctionRef<absl::Status(const Type& type)> visitor) const;
  // Visits only the active payload members for one variant, providing the
  // canonical storage slot index and the payload index within the variant.
  absl::Status ForEachActivePayloadSlot(
      const VariantInfo& variant,
      absl::FunctionRef<absl::Status(int64_t slot_index, int64_t active_index,
                                     const Type& type)>
          visitor) const;
  // Replays the canonical Phase 1 payload storage order for one variant
  // without exposing raw slot metadata to callers.
  absl::Status VisitPayloadAssemblyOrder(
      const VariantInfo& variant,
      absl::FunctionRef<absl::Status(int64_t active_index)> active_visitor,
      absl::FunctionRef<absl::Status(const Type& inactive_type)>
          inactive_visitor) const;

 private:
  struct StoredVariant {
    int64_t variant_index;
    const SumTypeVariant* variant;
    int64_t payload_start;
  };

  static VariantInfo ToVariantInfo(const StoredVariant& variant);
  absl::Status ValidateVariantInfo(const VariantInfo& variant) const;
  absl::StatusOr<const StoredVariant*> FindVariant(
      std::string_view variant_name) const;

  const SumType& type_;
  std::vector<const Type*> payload_slot_types_;
  std::vector<StoredVariant> variants_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_SUM_TYPE_ENCODING_H_
