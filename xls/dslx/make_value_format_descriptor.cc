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

#include "xls/dslx/make_value_format_descriptor.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {

// Bridges semantic layout ownership to formatting without exposing raw
// payload offsets in the public descriptor-construction API. One construction
// context preserves shared sums even when reached through other aggregates.
class ValueFormatDescriptorBuilder {
 public:
  enum class ChannelFormatPolicy { kReject, kOpaque };

  explicit ValueFormatDescriptorBuilder(
      FormatPreference field_preference,
      ChannelFormatPolicy channel_format_policy = ChannelFormatPolicy::kReject)
      : field_preference_(field_preference),
        channel_format_policy_(channel_format_policy) {}

  absl::StatusOr<ValueFormatDescriptor> Build(const Type& type);

 private:
  absl::StatusOr<ValueFormatDescriptor> BuildStruct(
      const StructTypeBase& struct_type);
  absl::StatusOr<ValueFormatDescriptor> BuildTuple(const TupleType& tuple_type);
  absl::StatusOr<ValueFormatDescriptor> BuildArray(const ArrayType& type);
  absl::StatusOr<ValueFormatDescriptor> BuildSum(const SumType& type);

  const FormatPreference field_preference_;
  const ChannelFormatPolicy channel_format_policy_;
  // The vector object's address identifies the complete immutable sum data,
  // including for empty sums. Keys are borrowed only for this synchronous
  // construction; completed descriptors own their strings and packed metadata.
  absl::flat_hash_map<const std::vector<SumTypeVariant>*, ValueFormatDescriptor>
      sum_descriptors_;
};

absl::StatusOr<ValueFormatDescriptor> ValueFormatDescriptorBuilder::BuildStruct(
    const StructTypeBase& struct_type) {
  std::vector<std::string> field_names;
  std::vector<ValueFormatDescriptor> field_formats;
  field_names.reserve(struct_type.size());
  field_formats.reserve(struct_type.size());
  for (size_t i = 0; i < struct_type.size(); ++i) {
    const Type& member_type = struct_type.GetMemberType(i);
    field_names.push_back(std::string{struct_type.GetMemberName(i)});
    XLS_ASSIGN_OR_RETURN(auto desc, Build(member_type));
    field_formats.push_back(std::move(desc));
  }
  return ValueFormatDescriptor::MakeStruct(
      struct_type.struct_def_base().identifier(), field_names, field_formats);
}

absl::StatusOr<ValueFormatDescriptor> ValueFormatDescriptorBuilder::BuildTuple(
    const TupleType& tuple_type) {
  std::vector<ValueFormatDescriptor> elements;
  for (size_t i = 0; i < tuple_type.size(); ++i) {
    const Type& member_type = tuple_type.GetMemberType(i);
    XLS_ASSIGN_OR_RETURN(auto vfd, Build(member_type));
    elements.push_back(std::move(vfd));
  }
  return ValueFormatDescriptor::MakeTuple(elements);
}

absl::StatusOr<ValueFormatDescriptor> ValueFormatDescriptorBuilder::BuildArray(
    const ArrayType& type) {
  XLS_ASSIGN_OR_RETURN(int64_t size, type.size().GetAsInt64());
  XLS_ASSIGN_OR_RETURN(ValueFormatDescriptor element_type_descriptor,
                       Build(type.element_type()));
  return ValueFormatDescriptor::MakeArray(element_type_descriptor, size);
}

namespace {

absl::StatusOr<ValueFormatDescriptor> MakeEnumFormatDescriptor(
    const EnumType& type, FormatPreference field_preference) {
  absl::flat_hash_map<Bits, std::string> value_to_name;
  const EnumDef& enum_def = type.nominal_type();
  for (size_t i = 0; i < enum_def.values().size(); ++i) {
    const std::string& s = enum_def.GetMemberName(i);
    const InterpValue& v = type.members().at(i);
    XLS_RET_CHECK(v.IsEnum());
    value_to_name[v.GetBitsOrDie()] = s;
  }
  XLS_ASSIGN_OR_RETURN(int64_t bit_count, type.size().GetAsInt64());
  return ValueFormatDescriptor::MakeEnum(enum_def.identifier(),
                                         std::move(value_to_name), bit_count,
                                         type.is_signed());
}

}  // namespace

absl::StatusOr<ValueFormatDescriptor> ValueFormatDescriptorBuilder::BuildSum(
    const SumType& type) {
  const SumTypeEncoding encoding(type);
  std::vector<ValueFormatSumVariantDescriptor> variants;
  std::vector<Bits> variant_tag_bits;
  variants.reserve(type.variant_count());
  variant_tag_bits.reserve(type.variant_count());
  XLS_RETURN_IF_ERROR(encoding.ForEachVariant(
      [&](const SumTypeEncoding::VariantInfo& info) -> absl::Status {
        variant_tag_bits.push_back(info.discriminant->GetBitsOrDie());
        const SumTypeVariant& variant = *info.variant;
        std::vector<ValueFormatDescriptor> payload_formats;
        payload_formats.reserve(variant.size());
        for (int64_t i = 0; i < variant.size(); ++i) {
          XLS_ASSIGN_OR_RETURN(ValueFormatDescriptor payload_format,
                               Build(variant.GetMemberType(i)));
          payload_formats.push_back(std::move(payload_format));
        }
        if (variant.is_unit()) {
          variants.push_back(ValueFormatSumVariantDescriptor::MakeUnit(
              std::string(variant.variant().identifier())));
        } else if (variant.is_tuple()) {
          variants.push_back(ValueFormatSumVariantDescriptor::MakeTuple(
              std::string(variant.variant().identifier()),
              std::move(payload_formats)));
        } else {
          std::vector<std::string> field_names;
          field_names.reserve(variant.size());
          for (int64_t i = 0; i < variant.size(); ++i) {
            field_names.push_back(std::string(variant.GetMemberName(i)));
          }
          variants.push_back(ValueFormatSumVariantDescriptor::MakeStruct(
              std::string(variant.variant().identifier()),
              std::move(field_names), std::move(payload_formats)));
        }
        return absl::OkStatus();
      }));
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  return internal::MakePackedSumValueFormatDescriptor(
      type.nominal_type().identifier(), variants, tag_bit_count,
      payload_slot_bit_count, variant_tag_bits);
}

absl::StatusOr<ValueFormatDescriptor> ValueFormatDescriptorBuilder::Build(
    const Type& type) {
  class Visitor : public TypeVisitor {
   public:
    explicit Visitor(ValueFormatDescriptorBuilder& builder)
        : builder_(builder) {}

    absl::Status HandleArray(const ArrayType& t) override {
      if (IsBitsLike(t)) {
        std::optional<BitsLikeProperties> bits_like = GetBitsLike(t);
        XLS_RET_CHECK(bits_like.has_value());
        XLS_ASSIGN_OR_RETURN(int64_t bit_count, bits_like->size.GetAsInt64());
        XLS_ASSIGN_OR_RETURN(bool is_signed, bits_like->is_signed.GetAsBool());
        result_ = ValueFormatDescriptor::MakeLeafValue(
            builder_.field_preference_, bit_count, is_signed);
      } else {
        XLS_ASSIGN_OR_RETURN(result_, builder_.BuildArray(t));
      }
      return absl::OkStatus();
    }
    absl::Status HandleStruct(const StructType& t) override {
      XLS_ASSIGN_OR_RETURN(result_, builder_.BuildStruct(t));
      return absl::OkStatus();
    }
    absl::Status HandleSum(const SumType& t) override {
      const auto* identity = &t.variants();
      if (auto it = builder_.sum_descriptors_.find(identity);
          it != builder_.sum_descriptors_.end()) {
        result_ = it->second;
      } else {
        XLS_ASSIGN_OR_RETURN(result_, builder_.BuildSum(t));
        builder_.sum_descriptors_.emplace(identity, result_);
      }
      return absl::OkStatus();
    }
    absl::Status HandleProc(const ProcType& t) override {
      XLS_ASSIGN_OR_RETURN(result_, builder_.BuildStruct(t));
      return absl::OkStatus();
    }
    absl::Status HandleTuple(const TupleType& t) override {
      XLS_ASSIGN_OR_RETURN(result_, builder_.BuildTuple(t));
      return absl::OkStatus();
    }
    absl::Status HandleEnum(const EnumType& t) override {
      XLS_ASSIGN_OR_RETURN(
          result_, MakeEnumFormatDescriptor(t, builder_.field_preference_));
      return absl::OkStatus();
    }
    absl::Status HandleBits(const BitsType& t) override {
      XLS_ASSIGN_OR_RETURN(int64_t bit_count, t.size().GetAsInt64());
      result_ = ValueFormatDescriptor::MakeLeafValue(builder_.field_preference_,
                                                     bit_count, t.is_signed());
      return absl::OkStatus();
    }
    absl::Status HandleFunction(const FunctionType& t) override {
      return absl::InvalidArgumentError("Cannot format a function type; got: " +
                                        t.ToString());
    }
    absl::Status HandleToken(const TokenType& t) override {
      result_ =
          ValueFormatDescriptor::MakeLeafValue(builder_.field_preference_);
      return absl::OkStatus();
    }
    absl::Status HandleChannel(const ChannelType& t) override {
      return absl::InvalidArgumentError("Cannot format a channel type; got: " +
                                        t.ToString());
    }
    absl::Status HandleMeta(const MetaType& t) override {
      return absl::InvalidArgumentError("Cannot format a metatype; got: " +
                                        t.ToString());
    }
    absl::Status HandleBitsConstructor(const BitsConstructorType& t) override {
      return absl::InvalidArgumentError(
          "Cannot format a bits constructor; got: " + t.ToString());
    }
    absl::Status HandleModule(const ModuleType& t) override {
      return absl::InvalidArgumentError("Cannot format a module type; got: " +
                                        t.ToString());
    }

    ValueFormatDescriptor& result() { return result_; }

   private:
    ValueFormatDescriptorBuilder& builder_;
    ValueFormatDescriptor result_;
  };

  if (channel_format_policy_ == ChannelFormatPolicy::kOpaque &&
      type.GetDirectOrElementChannelType().has_value()) {
    // Channel arrays carry handles and must not use an array value descriptor.
    return ValueFormatDescriptor::MakeLeafValue(field_preference_);
  } else {
    Visitor v(*this);
    XLS_RETURN_IF_ERROR(type.Accept(v));
    return std::move(v.result());
  }
}

absl::StatusOr<ValueFormatDescriptor> MakeValueFormatDescriptor(
    const Type& type, FormatPreference field_preference) {
  return ValueFormatDescriptorBuilder(field_preference).Build(type);
}

absl::StatusOr<ValueFormatDescriptor> MakeTraceCallFormatDescriptor(
    const Type& type, FormatPreference field_preference) {
  return ValueFormatDescriptorBuilder(
             field_preference,
             ValueFormatDescriptorBuilder::ChannelFormatPolicy::kOpaque)
      .Build(type);
}

}  // namespace xls::dslx
