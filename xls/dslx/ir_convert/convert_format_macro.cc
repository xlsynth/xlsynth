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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
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
  std::vector<FormatStep> fmt_steps;
  std::vector<BValue> ir_args;
  std::optional<BValue> valid;
};

BValue FalsePredicate(ConvertContext& ctx) {
  return ctx.fn_builder.Literal(UBits(/*value=*/0, /*bit_count=*/1));
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

std::optional<BValue> AndPredicates(std::optional<BValue> lhs,
                                    std::optional<BValue> rhs,
                                    ConvertContext& ctx) {
  if (!lhs.has_value()) {
    return rhs;
  } else if (!rhs.has_value()) {
    return lhs;
  }
  return AndPredicates(*lhs, *rhs, ctx);
}

void AppendStep(FormatFragment& fragment, FormatStep step) {
  if (!fragment.fmt_steps.empty() &&
      std::holds_alternative<std::string>(fragment.fmt_steps.back()) &&
      std::holds_alternative<std::string>(step)) {
    absl::StrAppend(&std::get<std::string>(fragment.fmt_steps.back()),
                    std::get<std::string>(step));
  } else {
    fragment.fmt_steps.push_back(std::move(step));
  }
}

void AppendFormatting(FormatFragment& result, FormatFragment fragment) {
  for (FormatStep& step : fragment.fmt_steps) {
    AppendStep(result, std::move(step));
  }
  result.ir_args.insert(result.ir_args.end(),
                        std::make_move_iterator(fragment.ir_args.begin()),
                        std::make_move_iterator(fragment.ir_args.end()));
}

void AppendFragment(FormatFragment& result, FormatFragment fragment,
                    ConvertContext& ctx) {
  result.valid = AndPredicates(result.valid, fragment.valid, ctx);
  AppendFormatting(result, std::move(fragment));
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
absl::StatusOr<FormatFragment> Flatten(const ValueFormatDescriptor& vfd,
                                       const BValue& v, ConvertContext& ctx);

absl::StatusOr<FormatFragment> FlattenTuple(const ValueFormatDescriptor& tfd,
                                            const BValue& v,
                                            ConvertContext& ctx) {
  FormatFragment fragment;
  AppendStep(fragment, "(");
  for (size_t i = 0; i < tfd.size(); ++i) {
    BValue item = ctx.fn_builder.TupleIndex(v, i);
    XLS_ASSIGN_OR_RETURN(FormatFragment child,
                         Flatten(tfd.tuple_elements()[i], item, ctx));
    AppendFragment(fragment, std::move(child), ctx);
    if (i + 1 != tfd.size()) {
      AppendStep(fragment, ", ");
    }
  }
  if (tfd.size() == 1) {
    AppendStep(fragment, ",");
  }
  AppendStep(fragment, ")");
  return fragment;
}

absl::StatusOr<FormatFragment> FlattenStruct(const ValueFormatDescriptor& sfd,
                                             const BValue& v,
                                             ConvertContext& ctx) {
  FormatFragment fragment;
  AppendStep(fragment, absl::StrCat(sfd.struct_name(), "{{"));
  for (size_t i = 0; i < sfd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragment, ", ");
    }
    AppendStep(fragment, absl::StrCat(sfd.struct_field_names()[i], ": "));
    BValue field_value = ctx.fn_builder.TupleIndex(v, i);
    XLS_ASSIGN_OR_RETURN(FormatFragment child,
                         Flatten(sfd.struct_elements()[i], field_value, ctx));
    AppendFragment(fragment, std::move(child), ctx);
  }
  AppendStep(fragment, "}}");
  return fragment;
}

absl::StatusOr<FormatFragment> FlattenArray(const ValueFormatDescriptor& afd,
                                            const BValue& v,
                                            ConvertContext& ctx) {
  FormatFragment fragment;
  AppendStep(fragment, "[");
  for (int64_t i = 0; i < afd.size(); ++i) {
    if (i != 0) {
      AppendStep(fragment, ", ");
    }
    BValue index = ctx.fn_builder.Literal(UBits(i, /*bit_count=*/32));
    BValue elem = ctx.fn_builder.ArrayIndex(v, {index});
    XLS_ASSIGN_OR_RETURN(FormatFragment child,
                         Flatten(afd.array_element_format(), elem, ctx));
    AppendFragment(fragment, std::move(child), ctx);
  }
  AppendStep(fragment, "]");
  return fragment;
}

FormatFragment FlattenEnum(const ValueFormatDescriptor& efd, const BValue& v,
                           ConvertContext& ctx) {
  FormatFragment fragment;
  // IR tracing cannot carry the value-to-name lookup table, so enums continue
  // to print their raw value after the nominal type prefix.
  fragment.fmt_steps.push_back(absl::StrCat(efd.enum_name(), "::"));
  fragment.fmt_steps.push_back(FormatPreference::kDefault);
  fragment.ir_args.push_back(v);
  return fragment;
}

FormatFragment FlattenLeaf(const ValueFormatDescriptor& lfd, const BValue& v,
                           ConvertContext& ctx) {
  FormatFragment fragment;
  FormatPreference preference = lfd.leaf_format();
  if (preference == FormatPreference::kDefault &&
      lfd.leaf_is_signed().value_or(false)) {
    preference = FormatPreference::kSignedDecimal;
  }
  fragment.fmt_steps.push_back(preference);
  fragment.ir_args.push_back(v);
  return fragment;
}

// Reserve positions for rendering operations, not characters or packed bits.
// Alternatives reuse a span; product children occupy disjoint ordered spans,
// including children of width zero. A shared description at the same position
// can therefore combine exclusive incoming guards before lowering its children.
// Work is bounded by description edges and reserved positions, not paths
// through constructor alternatives. Structured extraction above remains
// separate.
class PackedSumFormatter {
 public:
  explicit PackedSumFormatter(ConvertContext& ctx) : ctx_(ctx) {}

  absl::StatusOr<FormatFragment> Format(const ValueFormatDescriptor& root,
                                        BValue tag, BValue payload) {
    ctx_.saw_sum = true;
    output_.resize(RenderingSpan(root));
    BValue enabled = ctx_.fn_builder.Literal(UBits(1, 1));
    XLS_RETURN_IF_ERROR(LowerSum(root, tag, {payload}, enabled, 0));
    // Reverse descriptor postorder gathers every parent, including distinct
    // nominal wrappers in a diamond, before lowering a shared descendant.
    for (auto it = postorder_.rbegin(); it != postorder_.rend(); ++it) {
      const ValueFormatDescriptor& format = **it;
      auto positions = std::move(incoming_[Identity(format)]);
      incoming_.erase(Identity(format));
      for (auto& [start, inputs] : positions) {
        XLS_ASSIGN_OR_RETURN(Incoming merged, MergeInputs(format, inputs));
        XLS_RETURN_IF_ERROR(Lower(format, merged.value, merged.guard, start));
      }
    }
    FormatFragment result;
    // Keep validity independent of emission. Only reached sum interpretations
    // constrain it; an inactive malformed payload cannot suppress a valid
    // trace.
    result.valid = ctx_.fn_builder.And(validity_);
    std::optional<BValue> open_guard;
    for (auto& position : output_) {
      for (ScheduledStep& step : position) {
        std::optional<BValue> guard = step.guard;
        if (IsLiteralUnsignedOne(step.guard.node())) {
          guard = std::nullopt;
        }
        if (guard != open_guard) {
          if (open_guard.has_value()) {
            AppendStep(result, FormatControl::kEndConditional);
          }
          if (guard.has_value()) {
            AppendStep(result, FormatControl::kBeginConditional);
            result.ir_args.push_back(*guard);
          }
          open_guard = guard;
        }
        AppendStep(result, std::move(step.format));
        if (step.argument.has_value()) {
          result.ir_args.push_back(*step.argument);
        }
      }
    }
    if (open_guard.has_value()) {
      AppendStep(result, FormatControl::kEndConditional);
    }
    return result;
  }

 private:
  struct PackedValue {
    BValue root;
    int64_t offset = 0;

    PackedValue At(int64_t delta) const { return {root, offset + delta}; }
  };
  struct Incoming {
    PackedValue value;
    BValue guard;
  };
  struct ScheduledStep {
    BValue guard;
    FormatStep format;
    std::optional<BValue> argument;
  };

  static const void* Identity(const ValueFormatDescriptor& format) {
    return format.IsSum() ? format.sum_format_identity() : &format;
  }

  int64_t MembersSpan(absl::Span<const ValueFormatDescriptor> members) {
    int64_t span = members.empty() ? 1 : 1 + members.size();
    for (const ValueFormatDescriptor& member : members) {
      span += RenderingSpan(member);
    }
    return span;
  }

  int64_t RenderingSpan(const ValueFormatDescriptor& format) {
    const void* identity = Identity(format);
    if (auto it = spans_.find(identity); it != spans_.end()) {
      return it->second;
    } else {
      int64_t span = 1;
      switch (format.kind()) {
        case ValueFormatDescriptorKind::kLeafValue:
          break;
        case ValueFormatDescriptorKind::kEnum:
          span = 2;
          break;
        case ValueFormatDescriptorKind::kArray:
          if (format.size() != 0) {
            span = 1 + format.size() *
                           (1 + RenderingSpan(format.array_element_format()));
          }
          break;
        case ValueFormatDescriptorKind::kTuple:
          span = MembersSpan(format.tuple_elements());
          break;
        case ValueFormatDescriptorKind::kStruct:
          span = MembersSpan(format.struct_elements());
          break;
        case ValueFormatDescriptorKind::kSum:
          for (size_t i = 0; i < format.sum_variant_count(); ++i) {
            span = std::max(
                span, MembersSpan(format.sum_variant(i).payload_formats()));
          }
          break;
      }
      spans_.emplace(identity, span);
      postorder_.push_back(&format);
      return span;
    }
  }

  void Emit(int64_t position, BValue guard, FormatStep step,
            std::optional<BValue> argument = std::nullopt) {
    output_.at(position).push_back({guard, std::move(step), argument});
  }

  void AddInput(const ValueFormatDescriptor& format, int64_t start,
                PackedValue value, BValue guard) {
    incoming_[Identity(format)][start].push_back({value, guard});
  }

  BValue Slice(PackedValue value, int64_t width) {
    return ctx_.fn_builder.BitSlice(value.root, value.offset, width);
  }

  BValue Any(absl::Span<const BValue> guards) {
    if (guards.empty()) {
      return FalsePredicate(ctx_);
    } else if (guards.size() == 1) {
      return guards.front();
    } else {
      return ctx_.fn_builder.Or(guards);
    }
  }

  absl::StatusOr<Incoming> MergeInputs(const ValueFormatDescriptor& format,
                                       absl::Span<const Incoming> inputs) {
    absl::flat_hash_map<std::pair<xls::Node*, int64_t>, size_t> by_value;
    std::vector<PackedValue> values;
    std::vector<std::vector<BValue>> value_guards;
    for (const Incoming& input : inputs) {
      auto [it, inserted] = by_value.emplace(
          std::make_pair(input.value.root.node(), input.value.offset),
          values.size());
      if (inserted) {
        values.push_back(input.value);
        value_guards.emplace_back();
      }
      value_guards[it->second].push_back(input.guard);
    }
    std::vector<BValue> guards;
    for (const auto& group : value_guards) {
      guards.push_back(Any(group));
    }
    BValue enabled = Any(guards);
    if (values.size() == 1) {
      return Incoming{values.front(), enabled};
    } else {
      XLS_ASSIGN_OR_RETURN(int64_t width, RequireFlatBitCount(format));
      std::vector<BValue> cases;
      for (PackedValue value : values) {
        cases.push_back(Slice(value, width));
      }
      // The first case uses the low selector bit. At most one incoming value
      // can be active at a reserved output position.
      std::reverse(guards.begin(), guards.end());
      return Incoming{
          {ctx_.fn_builder.OneHotSelect(ctx_.fn_builder.Concat(guards), cases)},
          enabled};
    }
  }

  absl::Status LowerMembers(absl::Span<const ValueFormatDescriptor> members,
                            absl::Span<const std::string> field_names,
                            PackedValue value, BValue guard, int64_t start,
                            int64_t width, std::string prefix,
                            std::string suffix) {
    if (members.empty()) {
      Emit(start, guard, absl::StrCat(prefix, suffix));
    } else {
      if (!field_names.empty()) {
        absl::StrAppend(&prefix, field_names.front(), ": ");
      }
      Emit(start++, guard, std::move(prefix));
      for (size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
          Emit(start++, guard,
               field_names.empty() ? ", "
                                   : absl::StrCat(", ", field_names[i], ": "));
        }
        XLS_ASSIGN_OR_RETURN(int64_t member_width,
                             RequireFlatBitCount(members[i]));
        width -= member_width;
        AddInput(members[i], start, value.At(width), guard);
        start += RenderingSpan(members[i]);
      }
      Emit(start, guard, std::move(suffix));
    }
    return absl::OkStatus();
  }

  absl::Status LowerSum(const ValueFormatDescriptor& format, BValue tag,
                        PackedValue payload, BValue enabled, int64_t start) {
    std::vector<BValue> declared_tags;
    for (size_t i = 0; i < format.sum_variant_count(); ++i) {
      const ValueFormatSumVariantView variant = format.sum_variant(i);
      XLS_ASSIGN_OR_RETURN(Bits tag_bits, format.sum_variant_tag_bits(i));
      BValue matches =
          ctx_.fn_builder.Eq(tag, ctx_.fn_builder.Literal(std::move(tag_bits)));
      declared_tags.push_back(matches);
      BValue guard = AndPredicates(enabled, matches, ctx_);
      int64_t width = 0;
      for (const ValueFormatDescriptor& member : variant.payload_formats()) {
        XLS_ASSIGN_OR_RETURN(int64_t member_width, RequireFlatBitCount(member));
        width += member_width;
      }
      if (width > format.sum_payload_slot_bit_count()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Variant payload does not fit in semantic sum shared slot for ",
            format.sum_name(), "::", variant.name()));
      }
      std::string prefix =
          absl::StrCat(format.sum_name(), "::", variant.name());
      std::string suffix;
      if (variant.kind() == ValueFormatSumVariantKind::kTuple) {
        prefix += "(";
        suffix = ")";
      } else if (variant.kind() == ValueFormatSumVariantKind::kStruct) {
        prefix += " {{";
        suffix = " }}";
      }
      XLS_RETURN_IF_ERROR(LowerMembers(
          variant.payload_formats(), variant.field_names(), payload, guard,
          start, width, std::move(prefix), std::move(suffix)));
    }
    BValue declared = Any(declared_tags);
    validity_.push_back(
        IsLiteralUnsignedOne(enabled.node())
            ? declared
            : ctx_.fn_builder.Or(ctx_.fn_builder.Not(enabled), declared));
    return absl::OkStatus();
  }

  absl::Status Lower(const ValueFormatDescriptor& format, PackedValue value,
                     BValue guard, int64_t start) {
    XLS_ASSIGN_OR_RETURN(int64_t width, RequireFlatBitCount(format));
    switch (format.kind()) {
      case ValueFormatDescriptorKind::kLeafValue: {
        FormatPreference preference = format.leaf_format();
        if (preference == FormatPreference::kDefault &&
            format.leaf_is_signed().value_or(false)) {
          preference = FormatPreference::kSignedDecimal;
        }
        Emit(start, guard, preference, Slice(value, width));
        break;
      }
      case ValueFormatDescriptorKind::kEnum:
        Emit(start, guard, absl::StrCat(format.enum_name(), "::"));
        Emit(start + 1, guard, FormatPreference::kDefault, Slice(value, width));
        break;
      case ValueFormatDescriptorKind::kTuple:
        return LowerMembers(format.tuple_elements(), {}, value, guard, start,
                            width, "(", format.size() == 1 ? ",)" : ")");
      case ValueFormatDescriptorKind::kStruct:
        return LowerMembers(
            format.struct_elements(), format.struct_field_names(), value, guard,
            start, width, absl::StrCat(format.struct_name(), "{{"), "}}");
      case ValueFormatDescriptorKind::kArray:
        if (format.size() == 0) {
          Emit(start, guard, "[]");
        } else {
          Emit(start++, guard, "[");
          const ValueFormatDescriptor& element = format.array_element_format();
          XLS_ASSIGN_OR_RETURN(int64_t element_width,
                               RequireFlatBitCount(element));
          for (size_t i = 0; i < format.size(); ++i) {
            if (i != 0) {
              Emit(start++, guard, ", ");
            }
            AddInput(element, start, value.At(i * element_width), guard);
            start += RenderingSpan(element);
          }
          Emit(start, guard, "]");
        }
        break;
      case ValueFormatDescriptorKind::kSum:
        return LowerSum(format,
                        Slice(value.At(format.sum_payload_slot_bit_count()),
                              format.sum_tag_bit_count()),
                        value, guard, start);
    }
    return absl::OkStatus();
  }

  ConvertContext& ctx_;
  absl::flat_hash_map<const void*, int64_t> spans_;
  std::vector<const ValueFormatDescriptor*> postorder_;
  absl::flat_hash_map<const void*, std::map<int64_t, std::vector<Incoming>>>
      incoming_;
  std::vector<std::vector<ScheduledStep>> output_;
  std::vector<BValue> validity_;
};

absl::StatusOr<FormatFragment> FlattenSum(const ValueFormatDescriptor& sfd,
                                          const BValue& v,
                                          ConvertContext& ctx) {
  BValue tag = ctx.fn_builder.TupleIndex(v, 0);
  BValue payload_tuple = ctx.fn_builder.TupleIndex(v, 1);
  BValue payload_slot = ctx.fn_builder.TupleIndex(payload_tuple, 0);
  return PackedSumFormatter(ctx).Format(sfd, tag, payload_slot);
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

  FormatFragment TakeResult() { return std::move(result_); }

 private:
  BValue ir_value_;
  ConvertContext& ctx_;
  FormatFragment result_;
};

absl::StatusOr<FormatFragment> Flatten(const ValueFormatDescriptor& vfd,
                                       const BValue& v, ConvertContext& ctx) {
  FlattenVisitor visitor(v, ctx);
  XLS_RETURN_IF_ERROR(vfd.Accept(visitor));
  return visitor.TakeResult();
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
  FormatFragment fragment;

  size_t next_argno = 0;
  for (size_t node_format_index = 0; node_format_index < node.format().size();
       ++node_format_index) {
    const FormatStep& step = node.format().at(node_format_index);
    if (std::holds_alternative<std::string>(step)) {
      AppendStep(fragment, step);
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
      XLS_ASSIGN_OR_RETURN(FormatFragment arg_fragment,
                           Flatten(value_format_descriptor, arg_val, ctx));
      AppendFragment(fragment, std::move(arg_fragment), ctx);
      next_argno += 1;
    }
  }

  BValue token = entry_token;
  if (!ctx.saw_sum) {
    return function_builder.Trace(token, control_predicate, fragment.ir_args,
                                  fragment.fmt_steps, verbosity);
  } else {
    XLS_RET_CHECK(fragment.valid.has_value());
    BValue trace_is_inactive = function_builder.Not(control_predicate);
    BValue well_formed_or_inactive =
        function_builder.Or(trace_is_inactive, *fragment.valid);
    token =
        function_builder.Assert(token, well_formed_or_inactive,
                                "Cannot trace malformed semantic sum value.");
    BValue trace_is_active_and_valid =
        AndPredicates(control_predicate, *fragment.valid, ctx);
    return function_builder.Trace(token, trace_is_active_and_valid,
                                  fragment.ir_args, fragment.fmt_steps,
                                  verbosity);
  }
}

}  // namespace xls::dslx
