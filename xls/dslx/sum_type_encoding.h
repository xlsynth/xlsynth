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
#include <utility>
#include <variant>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"

namespace xls::dslx {

// Shared semantic-sum storage encoding.
//
// The runtime representation is one semantic-discriminant tag plus one shared
// low-bit-aligned payload bit slot sized to the widest flattened variant
// payload.
class Phase1SumTypeEncoding {
 public:
  struct VariantInfo {
    int64_t variant_index;
    const SumTypeVariant* variant;
    const InterpValue* discriminant;

    int64_t payload_size() const { return variant->size(); }
    absl::StatusOr<int64_t> payload_bit_count() const;
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

    static StoredLeafInfo MakePayloadBits(BitsType payload_type) {
      return StoredLeafInfo(StoredType(std::move(payload_type)), std::nullopt);
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

  absl::StatusOr<int64_t> payload_slot_bit_count() const;
  absl::StatusOr<int64_t> tag_bit_count() const;

  absl::StatusOr<VariantInfo> GetVariant(std::string_view variant_name) const;
  absl::StatusOr<VariantInfo> GetVariantByTagBits(const Bits& tag_bits) const;
  absl::Status ForEachVariant(
      absl::FunctionRef<absl::Status(const VariantInfo& variant)> visitor)
      const;
  // Visits the stored leaves for the encoded sum value: one dense tag leaf
  // first, followed by the shared payload-bit slot. Both leaves are carried by
  // value inside `StoredLeafInfo`.
  absl::Status ForEachStoredLeafType(
      absl::FunctionRef<absl::Status(const StoredLeafInfo& leaf)> visitor)
      const;
  // Visits the semantic payload members for one variant in declaration order.
  absl::Status ForEachPayloadMember(
      const VariantInfo& variant,
      absl::FunctionRef<absl::Status(int64_t active_index, const Type& type)>
          visitor) const;

 private:
  struct StoredVariant {
    int64_t variant_index;
    const SumTypeVariant* variant;
    const InterpValue* discriminant;
  };

  static VariantInfo ToVariantInfo(const StoredVariant& variant);
  absl::Status ValidateVariantInfo(const VariantInfo& variant) const;
  absl::StatusOr<const StoredVariant*> FindVariant(
      std::string_view variant_name) const;
  absl::StatusOr<const StoredVariant*> FindVariantByTagBits(
      const Bits& tag_bits) const;

  const SumType& type_;
  std::vector<StoredVariant> variants_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_SUM_TYPE_ENCODING_H_
