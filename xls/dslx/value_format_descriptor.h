// Copyright 2023 The XLS Authors
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

#ifndef XLS_DSLX_VALUE_FORMAT_DESCRIPTOR_H_
#define XLS_DSLX_VALUE_FORMAT_DESCRIPTOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {

class ValueFormatDescriptor;

struct ValueFormatSumVariantDescriptor;
class ValueFormatSumVariantView;

enum class ValueFormatSumVariantKind : int8_t {
  kUnit,
  kTuple,
  kStruct,
};

// Visits concrete types in the ValueFormatDescriptor hierarchy.
class ValueFormatVisitor {
 public:
  virtual ~ValueFormatVisitor() = default;

  virtual absl::Status HandleArray(const ValueFormatDescriptor& d) = 0;
  virtual absl::Status HandleEnum(const ValueFormatDescriptor& d) = 0;
  virtual absl::Status HandleLeafValue(const ValueFormatDescriptor& d) = 0;
  virtual absl::Status HandleSum(const ValueFormatDescriptor& d) = 0;
  virtual absl::Status HandleStruct(const ValueFormatDescriptor& d) = 0;
  virtual absl::Status HandleTuple(const ValueFormatDescriptor& d) = 0;
};

enum class ValueFormatDescriptorKind : int8_t {
  kLeafValue,
  kEnum,
  kArray,
  kTuple,
  kStruct,
  kSum,
};

// Class for the description of how to format values (according to the structure
// of the type as determined after type-inferencing time).
//
// These are generally static summaries of information determined by the type
// inference process so they can be used after IR conversion or in bytecode
// interpretation, where the types are fully concrete and we only need limited
// metadata in order to print them out properly. This data structure can be one
// of several kinds (enum, tuple, array, struct, sum, or leaf) corresponding to
// the respective DSLX type.
//
// Sum descriptors preserve canonical variant order and enough constructor
// metadata to recover unit, tuple, and struct spellings when formatting the
// internal runtime representation of a semantic sum value.
class ValueFormatDescriptor {
 public:
  ValueFormatDescriptor() : kind_(ValueFormatDescriptorKind::kLeafValue) {}

  static ValueFormatDescriptor MakeLeafValue(FormatPreference format);
  static ValueFormatDescriptor MakeEnum(
      std::string_view enum_name,
      absl::flat_hash_map<Bits, std::string> value_to_name);
  static ValueFormatDescriptor MakeArray(
      const ValueFormatDescriptor& element_format, size_t size);
  static ValueFormatDescriptor MakeTuple(
      absl::Span<const ValueFormatDescriptor> elements);
  static ValueFormatDescriptor MakeStruct(
      std::string_view struct_name, absl::Span<const std::string> field_names,
      absl::Span<const ValueFormatDescriptor> field_formats);
  static ValueFormatDescriptor MakeSum(
      std::string_view sum_name,
      absl::Span<const ValueFormatSumVariantDescriptor> variants,
      FormatPreference tag_format);

  ValueFormatDescriptorKind kind() const { return kind_; }

  bool IsLeafValue() const {
    return kind() == ValueFormatDescriptorKind::kLeafValue;
  };
  bool IsArray() const { return kind() == ValueFormatDescriptorKind::kArray; };
  bool IsTuple() const { return kind() == ValueFormatDescriptorKind::kTuple; };
  bool IsStruct() const {
    return kind() == ValueFormatDescriptorKind::kStruct;
  };
  bool IsEnum() const { return kind() == ValueFormatDescriptorKind::kEnum; };
  bool IsSum() const { return kind() == ValueFormatDescriptorKind::kSum; }

  // Leaf methods.
  FormatPreference leaf_format() const {
    CHECK(IsLeafValue());
    return format_;
  }

  // Enum methods.
  std::string_view enum_name() const {
    CHECK(IsEnum());
    return enum_name_;
  }
  const absl::flat_hash_map<Bits, std::string>& value_to_name() const {
    CHECK(IsEnum());
    return value_to_name_;
  }

  // Array methods.
  const ValueFormatDescriptor& array_element_format() const {
    CHECK(IsArray());
    return children_.front();
  }
  // Struct methods.
  std::string_view struct_name() const {
    CHECK(IsStruct());
    return struct_name_;
  }
  absl::Span<const std::string> struct_field_names() const {
    CHECK(IsStruct());
    return struct_field_names_;
  }
  absl::Span<const ValueFormatDescriptor> struct_elements() const {
    CHECK(IsStruct());
    return children_;
  }

  // Tuple methods.
  absl::Span<const ValueFormatDescriptor> tuple_elements() const {
    CHECK(IsTuple());
    return children_;
  }

  // Sum methods.
  std::string_view sum_name() const {
    CHECK(IsSum());
    return sum_name_;
  }
  size_t sum_variant_count() const {
    CHECK(IsSum());
    return sum_variant_names_.size();
  }
  ValueFormatSumVariantView sum_variant(size_t i) const;
  FormatPreference sum_tag_format() const {
    CHECK(IsSum());
    return sum_tag_format_;
  }

  // Methods for aggregate kinds.
  size_t size() const {
    CHECK(IsTuple() || IsArray() || IsStruct());
    return size_;
  }

  absl::Status Accept(ValueFormatVisitor& v) const;

 private:
  explicit ValueFormatDescriptor(ValueFormatDescriptorKind kind)
      : kind_(kind) {}

  ValueFormatDescriptorKind kind_;
  std::vector<ValueFormatDescriptor> children_;

  // Leaf data members;
  FormatPreference format_ = FormatPreference::kDefault;

  // Enum data members.
  std::string enum_name_;
  absl::flat_hash_map<Bits, std::string> value_to_name_;

  // Size of array or tuple.
  size_t size_ = 0;

  std::string struct_name_;
  std::vector<std::string> struct_field_names_;

  // Sum data members.
  std::string sum_name_;
  std::vector<std::string> sum_variant_names_;
  std::vector<ValueFormatSumVariantKind> sum_variant_kinds_;
  std::vector<size_t> sum_variant_payload_starts_;
  std::vector<size_t> sum_variant_payload_sizes_;
  std::vector<std::vector<std::string>> sum_variant_field_names_;
  FormatPreference sum_tag_format_ = FormatPreference::kDefault;
};

// Describes one constructor inside a sum formatting descriptor.
//
// Callers describe each variant in canonical sum order and attach the payload
// formats owned by that variant. `ValueFormatDescriptor` flattens those payload
// descriptors internally into the canonical `(tag, payload_slots...)` layout
// used by runtime sum values.
struct ValueFormatSumVariantDescriptor {
  std::string name;
  ValueFormatSumVariantKind kind;
  std::vector<std::string> field_names;
  std::vector<ValueFormatDescriptor> payload_formats;
};

// Read-only view of one constructor inside a sum formatting descriptor.
//
// The payload formats let callers reason about one variant at a time without
// exposing the broader flattened storage layout used internally.
class ValueFormatSumVariantView {
 public:
  std::string_view name() const { return name_; }
  ValueFormatSumVariantKind kind() const { return kind_; }
  size_t payload_slot_count() const { return payload_formats_.size(); }
  absl::Span<const std::string> field_names() const { return field_names_; }
  absl::Span<const ValueFormatDescriptor> payload_formats() const {
    return payload_formats_;
  }

 private:
  friend class ValueFormatDescriptor;

  ValueFormatSumVariantView(
      std::string_view name, ValueFormatSumVariantKind kind,
      absl::Span<const std::string> field_names,
      absl::Span<const ValueFormatDescriptor> payload_formats)
      : name_(name),
        kind_(kind),
        field_names_(field_names),
        payload_formats_(payload_formats) {}

  std::string_view name_;
  ValueFormatSumVariantKind kind_;
  absl::Span<const std::string> field_names_;
  absl::Span<const ValueFormatDescriptor> payload_formats_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_VALUE_FORMAT_DESCRIPTOR_H_
