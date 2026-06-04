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

#include "xls/dslx/ir_convert/convert_format_macro.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/make_value_format_descriptor.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/format_strings.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/node_util.h"

namespace xls::dslx {
namespace {

struct ConvertContext {
  BuilderBase& fn_builder;
  bool saw_sum = false;
};

struct FormatFragment {
  BValue predicate;
  std::vector<FormatStep> fmt_steps;
  std::vector<BValue> ir_args;
};

BValue TruePredicate(ConvertContext& ctx) {
  return ctx.fn_builder.Literal(UBits(/*value=*/1, /*bit_count=*/1));
}

BValue FalsePredicate(ConvertContext& ctx) {
  return ctx.fn_builder.Literal(UBits(/*value=*/0, /*bit_count=*/1));
}

FormatFragment EmptyFragment(ConvertContext& ctx) {
  return FormatFragment{.predicate = TruePredicate(ctx)};
}

BValue AndPredicates(const BValue& lhs, const BValue& rhs,
                     ConvertContext& ctx) {
  if (IsLiteralUnsignedOne(lhs.node())) {
    return rhs;
  } else if (IsLiteralUnsignedOne(rhs.node())) {
    return lhs;
  }
  return ctx.fn_builder.And(lhs, rhs);
}

void AppendStep(std::vector<FormatFragment>& fragments, FormatStep step) {
  for (FormatFragment& fragment : fragments) {
    fragment.fmt_steps.push_back(step);
  }
}

std::vector<FormatFragment> CrossProduct(
    absl::Span<const FormatFragment> lhs,
    absl::Span<const FormatFragment> rhs, ConvertContext& ctx) {
  std::vector<FormatFragment> result;
  result.reserve(lhs.size() * rhs.size());
  for (const FormatFragment& left : lhs) {
    for (const FormatFragment& right : rhs) {
      FormatFragment merged{
          .predicate = AndPredicates(left.predicate, right.predicate, ctx),
      };
      merged.fmt_steps.reserve(left.fmt_steps.size() + right.fmt_steps.size());
      merged.fmt_steps.insert(merged.fmt_steps.end(), left.fmt_steps.begin(),
                              left.fmt_steps.end());
      merged.fmt_steps.insert(merged.fmt_steps.end(), right.fmt_steps.begin(),
                              right.fmt_steps.end());
      merged.ir_args.reserve(left.ir_args.size() + right.ir_args.size());
      merged.ir_args.insert(merged.ir_args.end(), left.ir_args.begin(),
                            left.ir_args.end());
      merged.ir_args.insert(merged.ir_args.end(), right.ir_args.begin(),
                            right.ir_args.end());
      result.push_back(std::move(merged));
    }
  }
  return result;
}

absl::StatusOr<int64_t> RequireFlatBitCount(
    const ValueFormatDescriptor& fmt_desc) {
  if (!fmt_desc.flat_bit_count().has_value()) {
    return absl::InvalidArgumentError(
        "Cannot lower semantic sum formatting without concrete bit-count "
        "metadata.");
  }
  return fmt_desc.flat_bit_count().value();
}

// Forward decls for recursion.
absl::StatusOr<std::vector<FormatFragment>> Flatten(
    const ValueFormatDescriptor& vfd, const BValue& v, ConvertContext& ctx);
absl::StatusOr<std::vector<FormatFragment>> FlattenFromBits(
    const ValueFormatDescriptor& vfd, const BValue& bits, ConvertContext& ctx);

absl::StatusOr<std::vector<FormatFragment>> FlattenTuple(
    const ValueFormatDescriptor& tfd, const BValue& v, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, "(");
  for (size_t i = 0; i < tfd.size(); ++i) {
    BValue item = ctx.fn_builder.TupleIndex(v, i);
    XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> child_fragments,
                         Flatten(tfd.tuple_elements()[i], item, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
    if (i + 1 != tfd.size()) {
      AppendStep(fragments, ", ");
    }
  }
  if (tfd.size() == 1) {
    AppendStep(fragments, ",");
  }
  AppendStep(fragments, ")");
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenTupleFromBits(
    const ValueFormatDescriptor& tfd, const BValue& bits, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, "(");
  XLS_ASSIGN_OR_RETURN(int64_t bit_offset, RequireFlatBitCount(tfd));
  for (size_t i = 0; i < tfd.size(); ++i) {
    const ValueFormatDescriptor& child = tfd.tuple_elements()[i];
    XLS_ASSIGN_OR_RETURN(int64_t child_bit_count, RequireFlatBitCount(child));
    bit_offset -= child_bit_count;
    BValue child_bits =
        ctx.fn_builder.BitSlice(bits, bit_offset, child_bit_count);
    XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> child_fragments,
                         FlattenFromBits(child, child_bits, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
    if (i + 1 != tfd.size()) {
      AppendStep(fragments, ", ");
    }
  }
  if (tfd.size() == 1) {
    AppendStep(fragments, ",");
  }
  AppendStep(fragments, ")");
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenStruct(
    const ValueFormatDescriptor& sfd, const BValue& v, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, absl::StrCat(sfd.struct_name(), "{{"));
  for (size_t i = 0; i < sfd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragments, ", ");
    }
    AppendStep(fragments, absl::StrCat(sfd.struct_field_names()[i], ": "));
    BValue field_value = ctx.fn_builder.TupleIndex(v, i);
    XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> child_fragments,
                         Flatten(sfd.struct_elements()[i], field_value, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
  }
  AppendStep(fragments, "}}");
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenStructFromBits(
    const ValueFormatDescriptor& sfd, const BValue& bits, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, absl::StrCat(sfd.struct_name(), "{{"));
  XLS_ASSIGN_OR_RETURN(int64_t bit_offset, RequireFlatBitCount(sfd));
  for (size_t i = 0; i < sfd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragments, ", ");
    }
    AppendStep(fragments, absl::StrCat(sfd.struct_field_names()[i], ": "));
    const ValueFormatDescriptor& child = sfd.struct_elements()[i];
    XLS_ASSIGN_OR_RETURN(int64_t child_bit_count, RequireFlatBitCount(child));
    bit_offset -= child_bit_count;
    BValue child_bits =
        ctx.fn_builder.BitSlice(bits, bit_offset, child_bit_count);
    XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> child_fragments,
                         FlattenFromBits(child, child_bits, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
  }
  AppendStep(fragments, "}}");
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenArray(
    const ValueFormatDescriptor& afd, const BValue& v, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, "[");
  for (int64_t i = 0; i < afd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragments, ", ");
    }
    BValue index = ctx.fn_builder.Literal(UBits(i, /*bit_count=*/32));
    BValue elem = ctx.fn_builder.ArrayIndex(v, {index});
    XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> child_fragments,
                         Flatten(afd.array_element_format(), elem, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
  }
  AppendStep(fragments, "]");
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenArrayFromBits(
    const ValueFormatDescriptor& afd, const BValue& bits, ConvertContext& ctx) {
  std::vector<FormatFragment> fragments = {EmptyFragment(ctx)};
  AppendStep(fragments, "[");
  XLS_ASSIGN_OR_RETURN(int64_t element_bit_count,
                       RequireFlatBitCount(afd.array_element_format()));
  for (int64_t i = 0; i < afd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragments, ", ");
    }
    BValue elem_bits =
        ctx.fn_builder.BitSlice(bits, i * element_bit_count, element_bit_count);
    XLS_ASSIGN_OR_RETURN(
        std::vector<FormatFragment> child_fragments,
        FlattenFromBits(afd.array_element_format(), elem_bits, ctx));
    fragments = CrossProduct(fragments, child_fragments, ctx);
  }
  AppendStep(fragments, "]");
  return fragments;
}

std::vector<FormatFragment> FlattenEnum(const ValueFormatDescriptor& efd,
                                        const BValue& v, ConvertContext& ctx) {
  FormatFragment fragment = EmptyFragment(ctx);
  // IR tracing cannot carry the value-to-name lookup table, so enums continue
  // to print their raw value after the nominal type prefix.
  fragment.fmt_steps.push_back(absl::StrCat(efd.enum_name(), "::"));
  fragment.fmt_steps.push_back(FormatPreference::kDefault);
  fragment.ir_args.push_back(v);
  return {std::move(fragment)};
}

std::vector<FormatFragment> FlattenLeaf(const ValueFormatDescriptor& lfd,
                                        const BValue& v, ConvertContext& ctx) {
  FormatFragment fragment = EmptyFragment(ctx);
  fragment.fmt_steps.push_back(lfd.leaf_format());
  fragment.ir_args.push_back(v);
  return {std::move(fragment)};
}

absl::StatusOr<std::vector<FormatFragment>> FlattenSumFromParts(
    const ValueFormatDescriptor& sfd, const BValue& tag,
    const BValue& payload_slot, ConvertContext& ctx) {
  ctx.saw_sum = true;
  if (!sfd.sum_tag_bit_count().has_value() ||
      !sfd.sum_payload_slot_bit_count().has_value()) {
    return absl::InvalidArgumentError(
        "Cannot lower semantic sum formatting without shared-slot metadata.");
  }
  std::vector<FormatFragment> fragments;
  for (size_t i = 0; i < sfd.sum_variant_count(); ++i) {
    const ValueFormatSumVariantView variant = sfd.sum_variant(i);
    XLS_ASSIGN_OR_RETURN(Bits variant_tag_bits, sfd.sum_variant_tag_bits(i));
    BValue tag_matches = ctx.fn_builder.Eq(
        tag, ctx.fn_builder.Literal(std::move(variant_tag_bits)));
    std::vector<FormatFragment> variant_fragments = {
        FormatFragment{.predicate = tag_matches}};
    AppendStep(variant_fragments,
               absl::StrCat(sfd.sum_name(), "::", variant.name()));

    const absl::Span<const ValueFormatDescriptor> payload_formats =
        variant.payload_formats();
    int64_t active_payload_bit_count = 0;
    for (const ValueFormatDescriptor& payload_format : payload_formats) {
      XLS_ASSIGN_OR_RETURN(int64_t payload_bit_count,
                           RequireFlatBitCount(payload_format));
      active_payload_bit_count += payload_bit_count;
    }
    if (active_payload_bit_count >
        sfd.sum_payload_slot_bit_count().value()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Variant payload does not fit in semantic sum shared slot for ",
          sfd.sum_name(), "::", variant.name()));
    }

    switch (variant.kind()) {
      case ValueFormatSumVariantKind::kUnit:
        break;
      case ValueFormatSumVariantKind::kTuple: {
        AppendStep(variant_fragments, "(");
        int64_t bit_offset = active_payload_bit_count;
        for (size_t j = 0; j < payload_formats.size(); ++j) {
          const ValueFormatDescriptor& payload_format = payload_formats[j];
          XLS_ASSIGN_OR_RETURN(int64_t payload_bit_count,
                               RequireFlatBitCount(payload_format));
          bit_offset -= payload_bit_count;
          BValue payload_bits =
              ctx.fn_builder.BitSlice(payload_slot, bit_offset,
                                      payload_bit_count);
          XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> payload_fragments,
                               FlattenFromBits(payload_format, payload_bits,
                                               ctx));
          variant_fragments =
              CrossProduct(variant_fragments, payload_fragments, ctx);
          if (j + 1 != payload_formats.size()) {
            AppendStep(variant_fragments, ", ");
          }
        }
        AppendStep(variant_fragments, ")");
        break;
      }
      case ValueFormatSumVariantKind::kStruct: {
        AppendStep(variant_fragments, " {{");
        int64_t bit_offset = active_payload_bit_count;
        for (size_t j = 0; j < payload_formats.size(); ++j) {
          if (j != 0) {
            AppendStep(variant_fragments, ", ");
          }
          AppendStep(variant_fragments,
                     absl::StrCat(variant.field_names()[j], ": "));
          const ValueFormatDescriptor& payload_format = payload_formats[j];
          XLS_ASSIGN_OR_RETURN(int64_t payload_bit_count,
                               RequireFlatBitCount(payload_format));
          bit_offset -= payload_bit_count;
          BValue payload_bits =
              ctx.fn_builder.BitSlice(payload_slot, bit_offset,
                                      payload_bit_count);
          XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> payload_fragments,
                               FlattenFromBits(payload_format, payload_bits,
                                               ctx));
          variant_fragments =
              CrossProduct(variant_fragments, payload_fragments, ctx);
        }
        AppendStep(variant_fragments, " }}");
        break;
      }
    }
    fragments.insert(fragments.end(),
                     std::make_move_iterator(variant_fragments.begin()),
                     std::make_move_iterator(variant_fragments.end()));
  }
  return fragments;
}

absl::StatusOr<std::vector<FormatFragment>> FlattenSum(
    const ValueFormatDescriptor& sfd, const BValue& v, ConvertContext& ctx) {
  BValue tag = ctx.fn_builder.TupleIndex(v, 0);
  BValue payload_tuple = ctx.fn_builder.TupleIndex(v, 1);
  BValue payload_slot = ctx.fn_builder.TupleIndex(payload_tuple, 0);
  return FlattenSumFromParts(sfd, tag, payload_slot, ctx);
}

class FlattenVisitor : public ValueFormatVisitor {
 public:
  FlattenVisitor(BValue ir_value, ConvertContext& ctx)
      : ir_value_(ir_value), ctx_(ctx) {}

  ~FlattenVisitor() override = default;

  absl::Status HandleArray(const ValueFormatDescriptor& d) override {
    XLS_ASSIGN_OR_RETURN(result_, FlattenArray(d, ir_value_, ctx_));
    return absl::OkStatus();
  }
  absl::Status HandleStruct(const ValueFormatDescriptor& d) override {
    XLS_ASSIGN_OR_RETURN(result_, FlattenStruct(d, ir_value_, ctx_));
    return absl::OkStatus();
  }
  absl::Status HandleEnum(const ValueFormatDescriptor& d) override {
    result_ = FlattenEnum(d, ir_value_, ctx_);
    return absl::OkStatus();
  }
  absl::Status HandleSum(const ValueFormatDescriptor& d) override {
    XLS_ASSIGN_OR_RETURN(result_, FlattenSum(d, ir_value_, ctx_));
    return absl::OkStatus();
  }
  absl::Status HandleTuple(const ValueFormatDescriptor& d) override {
    XLS_ASSIGN_OR_RETURN(result_, FlattenTuple(d, ir_value_, ctx_));
    return absl::OkStatus();
  }
  absl::Status HandleLeafValue(const ValueFormatDescriptor& d) override {
    result_ = FlattenLeaf(d, ir_value_, ctx_);
    return absl::OkStatus();
  }

  absl::Span<const FormatFragment> result() const { return result_; }

 private:
  BValue ir_value_;
  ConvertContext& ctx_;
  std::vector<FormatFragment> result_;
};

absl::StatusOr<std::vector<FormatFragment>> Flatten(
    const ValueFormatDescriptor& vfd, const BValue& v, ConvertContext& ctx) {
  FlattenVisitor visitor(v, ctx);
  XLS_RETURN_IF_ERROR(vfd.Accept(visitor));
  return std::vector<FormatFragment>(visitor.result().begin(),
                                     visitor.result().end());
}

absl::StatusOr<std::vector<FormatFragment>> FlattenFromBits(
    const ValueFormatDescriptor& vfd, const BValue& bits, ConvertContext& ctx) {
  switch (vfd.kind()) {
    case ValueFormatDescriptorKind::kLeafValue:
      return FlattenLeaf(vfd, bits, ctx);
    case ValueFormatDescriptorKind::kEnum:
      return FlattenEnum(vfd, bits, ctx);
    case ValueFormatDescriptorKind::kArray:
      return FlattenArrayFromBits(vfd, bits, ctx);
    case ValueFormatDescriptorKind::kTuple:
      return FlattenTupleFromBits(vfd, bits, ctx);
    case ValueFormatDescriptorKind::kStruct:
      return FlattenStructFromBits(vfd, bits, ctx);
    case ValueFormatDescriptorKind::kSum: {
      if (!vfd.sum_tag_bit_count().has_value() ||
          !vfd.sum_payload_slot_bit_count().has_value()) {
        return absl::InvalidArgumentError(
            "Cannot lower semantic sum formatting without shared-slot "
            "metadata.");
      }
      const int64_t payload_slot_bit_count =
          vfd.sum_payload_slot_bit_count().value();
      const int64_t tag_bit_count = vfd.sum_tag_bit_count().value();
      BValue tag = ctx.fn_builder.BitSlice(bits, payload_slot_bit_count,
                                           tag_bit_count);
      BValue payload_slot =
          ctx.fn_builder.BitSlice(bits, 0, payload_slot_bit_count);
      return FlattenSumFromParts(vfd, tag, payload_slot, ctx);
    }
  }
  return absl::InternalError("Unhandled ValueFormatDescriptorKind.");
}

BValue OrPredicates(absl::Span<const FormatFragment> alternatives,
                    ConvertContext& ctx) {
  if (alternatives.empty()) {
    return FalsePredicate(ctx);
  }
  std::vector<BValue> predicates;
  predicates.reserve(alternatives.size());
  for (const FormatFragment& alternative : alternatives) {
    predicates.push_back(alternative.predicate);
  }
  if (predicates.size() == 1) {
    return predicates.front();
  }
  return ctx.fn_builder.Or(predicates);
}

}  // namespace

absl::StatusOr<BValue> ConvertFormatMacro(const FormatMacro& node,
                                          const BValue& entry_token,
                                          const BValue& control_predicate,
                                          absl::Span<const BValue> arg_vals,
                                          int64_t verbosity,
                                          const TypeInfo& current_type_info,
                                          BuilderBase& function_builder) {
  ConvertContext ctx{.fn_builder = function_builder};
  std::vector<FormatFragment> alternatives = {EmptyFragment(ctx)};

  size_t next_argno = 0;
  for (size_t node_format_index = 0; node_format_index < node.format().size();
       ++node_format_index) {
    const FormatStep& step = node.format().at(node_format_index);
    if (std::holds_alternative<std::string>(step)) {
      AppendStep(alternatives, step);
    } else {
      XLS_RET_CHECK(std::holds_alternative<FormatPreference>(step));
      FormatPreference preference = std::get<FormatPreference>(step);
      const BValue& arg_val = arg_vals.at(next_argno);
      const Expr* arg_expr = node.args().at(next_argno);

      std::optional<Type*> maybe_type = current_type_info.GetItem(arg_expr);
      XLS_RET_CHECK(maybe_type.has_value());
      Type* type = maybe_type.value();
      XLS_ASSIGN_OR_RETURN(auto value_format_descriptor,
                           MakeValueFormatDescriptor(*type, preference));
      XLS_ASSIGN_OR_RETURN(std::vector<FormatFragment> arg_fragments,
                           Flatten(value_format_descriptor, arg_val, ctx));
      alternatives = CrossProduct(alternatives, arg_fragments, ctx);
      next_argno += 1;
    }
  }

  BValue token = entry_token;
  if (ctx.saw_sum) {
    BValue any_valid_alternative = OrPredicates(alternatives, ctx);
    BValue trace_is_inactive = function_builder.Not(control_predicate);
    BValue well_formed_or_inactive =
        function_builder.Or(trace_is_inactive, any_valid_alternative);
    token = function_builder.Assert(
        token, well_formed_or_inactive,
        "Cannot trace malformed semantic sum value.");
  }

  for (const FormatFragment& alternative : alternatives) {
    BValue predicate =
        AndPredicates(control_predicate, alternative.predicate, ctx);
    token = function_builder.Trace(token, predicate, alternative.ir_args,
                                   alternative.fmt_steps, verbosity);
  }
  return token;
}

}  // namespace xls::dslx
