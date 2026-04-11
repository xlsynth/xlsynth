// Copyright 2024 The XLS Authors
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

#include "xls/dslx/value_format_descriptor.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {

ValueFormatSumVariantDescriptor ValueFormatSumVariantDescriptor::MakeUnit(
    std::string name) {
  return ValueFormatSumVariantDescriptor(
      std::move(name), ValueFormatSumVariantKind::kUnit,
      /*field_names=*/{}, /*payload_formats=*/{});
}

ValueFormatSumVariantDescriptor ValueFormatSumVariantDescriptor::MakeTuple(
    std::string name, std::vector<ValueFormatDescriptor> payload_formats) {
  return ValueFormatSumVariantDescriptor(
      std::move(name), ValueFormatSumVariantKind::kTuple,
      /*field_names=*/{}, std::move(payload_formats));
}

ValueFormatSumVariantDescriptor ValueFormatSumVariantDescriptor::MakeStruct(
    std::string name, std::vector<std::string> field_names,
    std::vector<ValueFormatDescriptor> payload_formats) {
  CHECK_EQ(field_names.size(), payload_formats.size());
  return ValueFormatSumVariantDescriptor(std::move(name),
                                         ValueFormatSumVariantKind::kStruct,
                                         std::move(field_names),
                                         std::move(payload_formats));
}

ValueFormatDescriptor ValueFormatDescriptor::MakeLeafValue(
    FormatPreference format) {
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kLeafValue);
  vfd.format_ = format;
  return vfd;
}

ValueFormatDescriptor ValueFormatDescriptor::MakeEnum(
    std::string_view enum_name,
    absl::flat_hash_map<Bits, std::string> value_to_name) {
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kEnum);
  vfd.enum_name_ = enum_name;
  vfd.value_to_name_ = std::move(value_to_name);
  return vfd;
}

ValueFormatDescriptor ValueFormatDescriptor::MakeArray(
    const ValueFormatDescriptor& element_format, size_t size) {
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kArray);
  vfd.children_ = {element_format};
  vfd.size_ = size;
  return vfd;
}

ValueFormatDescriptor ValueFormatDescriptor::MakeTuple(
    absl::Span<const ValueFormatDescriptor> elements) {
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kTuple);
  vfd.children_ =
      std::vector<ValueFormatDescriptor>(elements.begin(), elements.end());
  vfd.size_ = elements.size();
  return vfd;
}

ValueFormatDescriptor ValueFormatDescriptor::MakeStruct(
    std::string_view struct_name, absl::Span<const std::string> field_names,
    absl::Span<const ValueFormatDescriptor> field_formats) {
  CHECK_EQ(field_names.size(), field_formats.size());
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kStruct);
  vfd.struct_name_ = struct_name;
  vfd.children_ = std::vector<ValueFormatDescriptor>(field_formats.begin(),
                                                     field_formats.end());
  vfd.size_ = field_names.size();
  vfd.struct_field_names_ =
      std::vector<std::string>(field_names.begin(), field_names.end());
  return vfd;
}

ValueFormatDescriptor ValueFormatDescriptor::MakeSum(
    std::string_view sum_name,
    absl::Span<const ValueFormatSumVariantDescriptor> variants,
    FormatPreference tag_format) {
  ValueFormatDescriptor vfd(ValueFormatDescriptorKind::kSum);
  vfd.sum_name_ = sum_name;
  vfd.sum_tag_format_ = tag_format;

  vfd.sum_variants_.reserve(variants.size());
  for (const ValueFormatSumVariantDescriptor& variant : variants) {
    vfd.sum_variants_.emplace_back(
        std::string(variant.name()), variant.kind(),
        std::vector<std::string>(variant.field_names().begin(),
                                 variant.field_names().end()),
        std::vector<ValueFormatDescriptor>(variant.payload_formats().begin(),
                                           variant.payload_formats().end()));
  }
  return vfd;
}

ValueFormatSumVariantView ValueFormatDescriptor::sum_variant(size_t i) const {
  CHECK(IsSum());
  const SumVariantFormat& variant = sum_variants_.at(i);
  return ValueFormatSumVariantView(
      variant.name, variant.kind, absl::MakeConstSpan(variant.field_names),
      absl::MakeConstSpan(variant.payload_formats));
}

absl::Status ValueFormatDescriptor::Accept(ValueFormatVisitor& v) const {
  switch (kind()) {
    case ValueFormatDescriptorKind::kLeafValue:
      return v.HandleLeafValue(*this);
    case ValueFormatDescriptorKind::kEnum:
      return v.HandleEnum(*this);
    case ValueFormatDescriptorKind::kArray:
      return v.HandleArray(*this);
    case ValueFormatDescriptorKind::kTuple:
      return v.HandleTuple(*this);
    case ValueFormatDescriptorKind::kStruct:
      return v.HandleStruct(*this);
    case ValueFormatDescriptorKind::kSum:
      return v.HandleSum(*this);
  }
  return absl::InvalidArgumentError(absl::StrCat(
      "Out of bounds ValueFormatDescriptorKind: ", static_cast<int>(kind())));
}

}  // namespace xls::dslx
