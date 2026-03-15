// Copyright 2025 The XLS Authors
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

#include "xls/dslx/exhaustiveness/match_exhaustiveness_checker.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/visitor.h"
#include "xls/dslx/exhaustiveness/interp_value_interval.h"
#include "xls/dslx/exhaustiveness/nd_region.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {
namespace {

int64_t GetLeafTypeCount(const Type& type) {
  if (type.IsTuple()) {
    int64_t result = 0;
    for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
      result += GetLeafTypeCount(*member);
    }
    return result;
  }
  if (type.IsSum()) {
    int64_t result = 1;
    for (const SumTypeVariant& variant : type.AsSum().variants()) {
      for (const std::unique_ptr<Type>& member : variant.payload_members()) {
        result += GetLeafTypeCount(*member);
      }
    }
    return result;
  }
  return 1;
}

void AppendLeafTypes(const Type& type, FlattenedLeafTypes* result) {
  if (type.IsTuple()) {
    for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
      AppendLeafTypes(*member, result);
    }
    return;
  }
  if (type.IsSum()) {
    const SumType& sum_type = type.AsSum();
    result->owned.push_back(
        std::make_unique<BitsType>(/*is_signed=*/false, sum_type.tag_bit_count()));
    result->flat.push_back(FlattenedLeafType{
        .type = result->owned.back().get(),
        .dense_max_value = sum_type.variant_count() - 1});
    for (const SumTypeVariant& variant : sum_type.variants()) {
      for (const std::unique_ptr<Type>& member : variant.payload_members()) {
        AppendLeafTypes(*member, result);
      }
    }
    return;
  }
  result->flat.push_back(
      FlattenedLeafType{.type = &type, .dense_max_value = std::nullopt});
}

FlattenedLeafTypes GetLeafTypes(const Type& type, const Span& span,
                                const FileTable& file_table) {
  FlattenedLeafTypes result;
  AppendLeafTypes(type, &result);
  // Validate that all the matched-upon types are either bits or enums.
  for (const FlattenedLeafType& leaf_type : result.flat) {
    CHECK(GetBitsLike(*leaf_type.type).has_value() || leaf_type.type->IsEnum())
        << "Non-bits or non-enum type in matched-upon tuple: "
        << leaf_type.type->ToString() << " @ " << span.ToString(file_table);
  }
  return result;
}

// Sentinel type to indicate that some wildcard is present for a value. This
// lets us collapse out varieties of wildcards e.g. RestOfTuple and
// WildcardPattern and NameDef.
struct SomeWildcard {};

// NameDefTree::Leaf but where RestOfTuple has been resolved.
using PatternLeaf =
    std::variant<SomeWildcard, InterpValue, NameRef*, Range*, ColonRef*,
                 Number*>;

InterpValueInterval MakeFullIntervalForLeafType(const FlattenedLeafType& type) {
  if (type.dense_max_value.has_value()) {
    std::optional<BitsLikeProperties> bits_like = GetBitsLike(*type.type);
    CHECK(bits_like.has_value())
        << "MakeFullIntervalForLeafType; got non-bits dense leaf type: "
        << type.type->ToString();
    int64_t bit_count = bits_like->size.GetAsInt64().value();
    return InterpValueInterval(InterpValue::MakeUBits(bit_count, 0),
                               InterpValue::MakeUBits(
                                   bit_count, *type.dense_max_value));
  }
  if (type.type->IsEnum()) {
    return MakeFullIntervalForEnumType(type.type->AsEnum());
  }
  std::optional<BitsLikeProperties> bits_like = GetBitsLike(*type.type);
  CHECK(bits_like.has_value())
      << "MakeFullIntervalForLeafType; got non-bits type: "
      << type.type->ToString();
  int64_t bit_count = bits_like->size.GetAsInt64().value();
  bool is_signed = bits_like->is_signed.GetAsBool().value();
  InterpValue min = InterpValue::MakeMinValue(is_signed, bit_count);
  InterpValue max = InterpValue::MakeMaxValue(is_signed, bit_count);
  InterpValueInterval result(min, max);
  VLOG(5) << "MakeFullIntervalForLeafType; type: `" << type.type->ToString()
          << "` result: " << result.ToString(/*show_types=*/false);
  return result;
}

// Returns the "full" intervals that can be used to represent the "no values
// have been exhausted" initial state.
std::vector<InterpValueInterval> GetFullIntervals(
    absl::Span<const FlattenedLeafType> leaf_types) {
  std::vector<InterpValueInterval> result;
  for (const FlattenedLeafType& leaf_type : leaf_types) {
    result.push_back(MakeFullIntervalForLeafType(leaf_type));
  }
  return result;
}

InterpValueInterval MakePointIntervalForType(const Type& type,
                                             const InterpValue& value,
                                             const ImportData& import_data) {
  VLOG(5) << "MakePointIntervalForType; type: `" << type.ToString()
          << "` value: `" << value.ToString() << "`";
  if (type.IsEnum()) {
    return MakePointIntervalForEnumType(type.AsEnum(), value, import_data);
  }
  std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
  CHECK(bits_like.has_value())
      << "MakePointIntervalForType; got non-bits type: " << type.ToString();
  return InterpValueInterval(value, value);
}

InterpValueInterval MakeIntervalForType(const Type& type,
                                        const InterpValue& min,
                                        const InterpValue& max) {
  std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
  CHECK(bits_like.has_value())
      << "MakeIntervalForType; got non-bits type: " << type.ToString();
  return InterpValueInterval(min, max);
}

std::optional<InterpValueInterval> PatternToIntervalInternal(
    const PatternLeaf& leaf, const FlattenedLeafType& leaf_type,
    const TypeInfo& type_info,
    const ImportData& import_data) {
  std::optional<InterpValueInterval> result = absl::visit(
      Visitor{
          [&](SomeWildcard /*unused*/) -> std::optional<InterpValueInterval> {
            return MakeFullIntervalForLeafType(leaf_type);
          },
          [&](const InterpValue& value) -> std::optional<InterpValueInterval> {
            return MakePointIntervalForType(*leaf_type.type, value, import_data);
          },
          [&](NameRef* name_ref) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(name_ref);
            if (value.has_value()) {
              return MakePointIntervalForType(*leaf_type.type, value.value(),
                                              import_data);
            }
            return MakeFullIntervalForLeafType(leaf_type);
          },
          [&](Range* range) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> start =
                type_info.GetConstExprOption(range->start());
            std::optional<InterpValue> limit =
                type_info.GetConstExprOption(range->end());
            CHECK(start.has_value());
            CHECK(limit.has_value());
            if (start->Gt(*limit).value().IsTrue()) {
              return std::nullopt;
            }
            if (!range->inclusive_end()) {
              if (start->Eq(limit.value())) {
                return std::nullopt;
              }
              limit = limit->Decrement();
              if (!limit.has_value()) {
                // Underflow -- that means the range must be empty because the
                // limit is exclusive and is known to be representable in the
                // type.
                return std::nullopt;
              }
            }
            return MakeIntervalForType(*leaf_type.type, *start, *limit);
          },
          [&](ColonRef* colon_ref) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(colon_ref);
            CHECK(value.has_value());
            VLOG(5) << "PatternToIntervalInternal; colon_ref: `"
                    << colon_ref->ToString() << "` value: `"
                    << value.value().ToString() << "`" << " leaf_type: `"
                    << leaf_type.type->ToString() << "`";
            return MakePointIntervalForType(*leaf_type.type, value.value(),
                                            import_data);
          },
          [&](Number* number) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(number);
            CHECK(value.has_value());
            return MakePointIntervalForType(*leaf_type.type, value.value(),
                                            import_data);
          }},
      leaf);
  VLOG(5) << "PatternToIntervalInternal; leaf_type: `"
          << leaf_type.type->ToString()
          << "` result: "
          << (result.has_value() ? result->ToString(/*show_types=*/false)
                                 : "nullopt");
  return result;
}

PatternLeaf ToPatternLeaf(const NameDefTree::Leaf& leaf) {
  return absl::visit(
      Visitor{
          [&](NameDef* name_def) -> PatternLeaf { return SomeWildcard(); },
          [&](NameRef* name_ref) -> PatternLeaf { return name_ref; },
          [&](Range* range) -> PatternLeaf { return range; },
          [&](ColonRef* colon_ref) -> PatternLeaf { return colon_ref; },
          [&](WildcardPattern* wildcard_pattern) -> PatternLeaf {
            return SomeWildcard();
          },
          [&](Number* number) -> PatternLeaf { return number; },
          [&](ConstructorPattern* /*constructor_pattern*/) -> PatternLeaf {
            LOG(FATAL) << "ConstructorPattern not yet supported in "
                          "MatchExhaustivenessChecker";
          },
          [&](RestOfTuple* rest_of_tuple) -> PatternLeaf {
            LOG(FATAL) << "RestOfTuple not valid for conversion to PatternLeaf";
          }},
      leaf);
}

int64_t GetSumVariantIndex(const SumType& sum_type,
                           std::string_view constructor_name) {
  for (int64_t i = 0; i < sum_type.variant_count(); ++i) {
    if (sum_type.variants()[i].variant().identifier() == constructor_name) {
      return i;
    }
  }
  LOG(FATAL) << "Unknown sum constructor `" << constructor_name
             << "` for type `" << sum_type.ToString() << "`";
  return 0;
}

InterpValue MakeSumTagValue(const SumType& sum_type, int64_t variant_index) {
  int64_t bit_count = sum_type.tag_bit_count().GetAsInt64().value();
  return InterpValue::MakeUBits(bit_count, variant_index);
}

void AppendWildcardLeavesForType(const Type& type,
                                 std::vector<PatternLeaf>* result) {
  result->insert(result->end(), GetLeafTypeCount(type), SomeWildcard());
}

std::vector<PatternLeaf> ExpandPatternLeaves(const NameDefTree& pattern,
                                             const Type& type,
                                             const FileTable& file_table);

std::vector<PatternLeaf> ExpandSumPatternLeaves(const NameDefTree::Leaf& leaf,
                                                const SumType& type,
                                                const FileTable& file_table) {
  auto make_variant_pattern_leaves =
      [&](int64_t active_variant_index,
          const ConstructorPattern* constructor_pattern)
      -> std::vector<PatternLeaf> {
    std::vector<PatternLeaf> result;
    result.push_back(MakeSumTagValue(type, active_variant_index));
    for (int64_t i = 0; i < type.variant_count(); ++i) {
      const SumTypeVariant& variant = type.variants()[i];
      if (i != active_variant_index) {
        for (const std::unique_ptr<Type>& member : variant.payload_members()) {
          AppendWildcardLeavesForType(*member, &result);
        }
        continue;
      }
      if (constructor_pattern == nullptr) {
        CHECK(variant.is_unit());
        continue;
      }
      if (variant.is_tuple()) {
        CHECK(constructor_pattern->is_tuple());
        CHECK_EQ(constructor_pattern->positional_patterns().size(),
                 variant.size());
        for (int64_t member_index = 0; member_index < variant.size();
             ++member_index) {
          std::vector<PatternLeaf> member_leaves =
              ExpandPatternLeaves(
                  *constructor_pattern->positional_patterns()[member_index],
                  variant.GetMemberType(member_index), file_table);
          result.insert(result.end(), member_leaves.begin(),
                        member_leaves.end());
        }
        continue;
      }
      CHECK(variant.is_struct());
      CHECK(constructor_pattern->is_struct());
      CHECK_EQ(constructor_pattern->named_patterns().size(), variant.size());
      for (int64_t member_index = 0; member_index < variant.size();
           ++member_index) {
        const std::string_view member_name = variant.GetMemberName(member_index);
        auto it = std::find_if(
            constructor_pattern->named_patterns().begin(),
            constructor_pattern->named_patterns().end(),
            [&](const ConstructorPattern::NamedPattern& named_pattern) {
              return named_pattern.first == member_name;
            });
        CHECK(it != constructor_pattern->named_patterns().end())
            << "Missing named pattern for member `" << member_name << "`";
        std::vector<PatternLeaf> member_leaves =
            ExpandPatternLeaves(*it->second, variant.GetMemberType(member_index),
                                file_table);
        result.insert(result.end(), member_leaves.begin(), member_leaves.end());
      }
    }
    return result;
  };

  return absl::visit(
      Visitor{
          [&](ConstructorPattern* constructor_pattern)
              -> std::vector<PatternLeaf> {
            int64_t variant_index = GetSumVariantIndex(
                type, constructor_pattern->constructor()->attr());
            return make_variant_pattern_leaves(variant_index,
                                              constructor_pattern);
          },
          [&](ColonRef* colon_ref) -> std::vector<PatternLeaf> {
            int64_t variant_index =
                GetSumVariantIndex(type, colon_ref->attr());
            CHECK(type.variants()[variant_index].is_unit());
            return make_variant_pattern_leaves(variant_index,
                                              /*constructor_pattern=*/nullptr);
          },
          [&](const auto&) -> std::vector<PatternLeaf> {
            LOG(FATAL) << "Unsupported pattern for sum type `" << type.ToString()
                       << "`";
            return {};
          }},
      leaf);
}

std::vector<PatternLeaf> ExpandPatternLeaves(const NameDefTree& pattern,
                                             const Type& type,
                                             const FileTable& file_table) {
  VLOG(5) << "ExpandPatternLeaves; pattern: `" << pattern.ToString()
          << "` type: `" << type.ToString() << "`";
  // For an irrefutable pattern, simply return wildcards for every leaf.
  if (pattern.IsIrrefutable()) {
    return std::vector<PatternLeaf>(GetLeafTypeCount(type), SomeWildcard());
  }
  if (type.IsSum()) {
    CHECK(pattern.is_leaf())
        << "Expected a leaf pattern for sum type, got `" << pattern.ToString()
        << "`";
    return ExpandSumPatternLeaves(pattern.leaf(), type.AsSum(), file_table);
  }
  // If the type is not a tuple then we expect the pattern to be a single leaf.
  if (!type.IsTuple()) {
    std::vector<NameDefTree::Leaf> leaves = pattern.Flatten();
    CHECK_EQ(leaves.size(), 1)
        << "Expected a single leaf for non-tuple type, got " << leaves.size();
    return {ToPatternLeaf(leaves.front())};
  }
  // Walk through the pattern and expand any RestOfTuple markers into the
  // appropriate number of wildcards.
  //
  // In order to do this we have to recursively call to ExpandPatternLeaves for
  // any sub-tuples encountered.
  absl::Span<const std::unique_ptr<Type>> tuple_members =
      type.AsTuple().members();
  std::vector<std::variant<NameDefTree::Leaf, NameDefTree*>> flattened =
      pattern.Flatten1();

  // Note: there can be fewer flatten1'd nodes than tuple elements because of
  // RestOfTuple markers.
  //
  // We need the `+1` here because we can have RestOfTuple markers that map to
  // zero elements in the tuple (i.e. useless/redundant ones).
  CHECK_LE(flattened.size(), tuple_members.size() + 1);

  // The results correspond to leaf types.
  std::vector<PatternLeaf> result;

  // The tuple type index at *this level* of the tuple.
  // We bump this as we progress through -- note a single "flattened_index"
  // below can advance zero or more type indices.
  int64_t types_index = 0;

  for (int64_t flattened_index = 0; flattened_index < flattened.size();
       ++flattened_index) {
    VLOG(5) << "ExpandPatternLeaves; flattened_index: " << flattened_index
            << " flattened.size(): " << flattened.size()
            << " types_index: " << types_index
            << " tuple_members.size(): " << tuple_members.size();
    CHECK_LT(flattened_index, flattened.size())
        << "Flattened index out of bounds.";
    const auto& node = flattened[flattened_index];

    if (std::holds_alternative<NameDefTree*>(node)) {
      const NameDefTree* sub_pattern = std::get<NameDefTree*>(node);
      CHECK_LT(types_index, tuple_members.size());
      const Type& type_at_index = *tuple_members[types_index];

      std::vector<PatternLeaf> sub_pattern_leaves =
          ExpandPatternLeaves(*sub_pattern, type_at_index, file_table);

      result.insert(result.end(), sub_pattern_leaves.begin(),
                    sub_pattern_leaves.end());
      types_index += 1;
      continue;
    }
    const NameDefTree::Leaf& leaf = std::get<NameDefTree::Leaf>(node);
    auto append_non_rest_leaf = [&]() {
      CHECK_LT(types_index, tuple_members.size());
      const Type& type_at_index = *tuple_members[types_index];
      if (type_at_index.IsSum()) {
        std::vector<PatternLeaf> sum_pattern_leaves =
            ExpandSumPatternLeaves(leaf, type_at_index.AsSum(), file_table);
        result.insert(result.end(), sum_pattern_leaves.begin(),
                      sum_pattern_leaves.end());
      } else {
        result.push_back(ToPatternLeaf(leaf));
      }
      types_index += 1;
    };
    absl::visit(
        Visitor{
            [&](const NameRef* /*unused*/) { append_non_rest_leaf(); },
            [&](const Range* /*unused*/) { append_non_rest_leaf(); },
            [&](const ColonRef* /*unused*/) { append_non_rest_leaf(); },
            [&](const Number* /*unused*/) { append_non_rest_leaf(); },
            [&](const ConstructorPattern* /*unused*/) {
              append_non_rest_leaf();
            },
            [&](const RestOfTuple* /*unused*/) {
              // Instead of using flattened_index here, use types_index (the
              // number of tuple elements already matched) to figure out how
              // many items we need "in the rest".
              int64_t explicit_before = types_index;
              int64_t explicit_after = flattened.size() - flattened_index - 1;
              int64_t to_push =
                  tuple_members.size() - (explicit_before + explicit_after);
              VLOG(5) << "ExpandPatternLeaves; RestOfTuple at flattened_index: "
                      << flattened_index << " types_index: " << types_index
                      << " explicit_after: " << explicit_after
                      << " to_push: " << to_push;
              for (int64_t i = 0; i < to_push; ++i) {
                // We have to push wildcard data corresponding to the type.
                CHECK_LT(types_index, tuple_members.size());
                const Type& type_at_index = *tuple_members[types_index];
                AppendWildcardLeavesForType(type_at_index, &result);
                types_index += 1;
              }
              VLOG(5) << "ExpandPatternLeaves; after RestOfTuple at "
                         "flattened_index: "
                      << flattened_index << " types_index: " << types_index
                      << " result.size(): " << result.size();
            },
            [&](const auto* irrefutable_leaf) {
              // Push back wildcards of the right size for the type.
              CHECK_LT(types_index, tuple_members.size());
              const Type& type_at_index = *tuple_members[types_index];
              AppendWildcardLeavesForType(type_at_index, &result);
              types_index += 1;
            }},
        leaf);
  }

  // Check that we got a consistent count between the razed tuple types and the
  // PatternLeaf vector.
  CHECK_EQ(result.size(), GetLeafTypeCount(type))
      << "Sub-pattern leaves and tuple type must be the same size.";
  return result;
}

NdIntervalWithEmpty PatternToInterval(const NameDefTree& pattern,
                                      const Type& matched_type,
                                      absl::Span<const FlattenedLeafType> leaf_types,
                                      const TypeInfo& type_info,
                                      const ImportData& import_data) {
  std::vector<PatternLeaf> pattern_leaves =
      ExpandPatternLeaves(pattern, matched_type, type_info.file_table());
  CHECK_EQ(pattern_leaves.size(), leaf_types.size())
      << "Pattern leaves and leaf types must be the same size.";

  // Each leaf describes some range in its dimension that it matches on --
  // together, they describe an n-dimensional interval.
  std::vector<std::optional<InterpValueInterval>> intervals;
  intervals.reserve(pattern_leaves.size());
  for (int64_t i = 0; i < pattern_leaves.size(); ++i) {
    intervals.push_back(PatternToIntervalInternal(
        pattern_leaves[i], leaf_types[i], type_info, import_data));
  }
  NdIntervalWithEmpty result(intervals);
  VLOG(5) << "PatternToInterval; pattern: `" << pattern.ToString()
          << "` type: `" << matched_type.ToString()
          << "` result: " << result.ToString(/*show_types=*/false);
  return result;
}

NdRegion MakeFullNdRegion(absl::Span<const FlattenedLeafType> leaf_types) {
  std::vector<InterpValueInterval> intervals = GetFullIntervals(leaf_types);
  std::vector<InterpValue> dim_extents;
  dim_extents.reserve(intervals.size());
  for (const InterpValueInterval& interval : intervals) {
    dim_extents.push_back(interval.max());
  }
  return NdRegion::MakeFromNdInterval(NdInterval(std::move(intervals)),
                                      std::move(dim_extents));
}

}  // namespace

// -- class MatchExhaustivenessChecker

MatchExhaustivenessChecker::MatchExhaustivenessChecker(
    const Span& matched_expr_span, const ImportData& import_data,
    const TypeInfo& type_info, const Type& matched_type)
    : matched_expr_span_(matched_expr_span),
      import_data_(import_data),
      type_info_(type_info),
      matched_type_(matched_type),
      leaf_types_(GetLeafTypes(matched_type, matched_expr_span, file_table())),
      remaining_(MakeFullNdRegion(leaf_types_.flat)) {}

bool MatchExhaustivenessChecker::IsExhaustive() const {
  return remaining_.IsEmpty();
}

bool MatchExhaustivenessChecker::AddPattern(const NameDefTree& pattern) {
  VLOG(5) << "MatchExhaustivenessChecker::AddPattern: `" << pattern.ToString()
          << "` matched_type: `" << matched_type_.ToString() << "` @ "
          << pattern.span().ToString(file_table());

  NdIntervalWithEmpty this_pattern_interval = PatternToInterval(
      pattern, matched_type_, leaf_types_.flat, type_info_, import_data_);
  remaining_ = remaining_.SubtractInterval(this_pattern_interval);
  return IsExhaustive();
}

std::optional<InterpValue>
MatchExhaustivenessChecker::SampleSimplestUncoveredValue() const {
  // If there are no uncovered regions, we are fully exhaustive.
  if (remaining_.IsEmpty()) {
    return std::nullopt;
  }

  // For now, just choose the first uncovered region.
  const NdInterval& nd_interval = remaining_.disjoint().front();
  std::vector<InterpValue> components;

  // For each dimension of the region, grab the lower bound (i.e. the simplest
  // value in that interval).
  for (int64_t i = 0; i < nd_interval.dims().size(); ++i) {
    const Type& type = *leaf_types_.flat[i].type;
    const InterpValueInterval& interval = nd_interval.dims()[i];
    const InterpValue& min = interval.min();
    if (type.IsEnum()) {
      // We have to project back from dense space to enum name space.
      const EnumType& enum_type = type.AsEnum();
      const EnumDef& enum_def = enum_type.nominal_type();

      absl::StatusOr<const TypeInfo*> enum_def_type_info =
          import_data_.GetRootTypeInfoForNode(&enum_def);
      CHECK_OK(enum_def_type_info.status())
          << "Enum type info not found for enum: " << enum_type.ToString();

      int64_t member_index = min.GetBitValueUnsigned().value();
      CHECK_LT(member_index, enum_def.values().size())
          << "Member index out of bounds: " << member_index
          << " for enum: " << enum_type.ToString();
      const EnumMember& member = enum_def.values()[member_index];
      InterpValue member_value =
          enum_def_type_info.value()->GetConstExpr(member.name_def).value();
      VLOG(5) << "SampleSimplestUncoveredValue; enum_type: "
              << enum_type.ToString() << " member_index: " << member_index
              << " member: " << member.name_def->ToString()
              << " member_value: " << member_value.ToString();
      components.push_back(std::move(member_value));
    } else {
      components.push_back(min);
    }
  }

  // If we have a single component, return it directly; otherwise, return a
  // tuple.
  if (components.size() == 1) {
    return components[0];
  }
  return InterpValue::MakeTuple(components);
}

InterpValueInterval MakeFullIntervalForEnumType(const EnumType& enum_type) {
  int64_t bit_count = enum_type.size().GetAsInt64().value();
  const EnumDef& enum_def = enum_type.nominal_type();
  int64_t enum_value_count = enum_def.values().size();
  VLOG(5) << "MakeFullIntervalForEnumType; enum_type: " << enum_type.ToString()
          << " enum_value_count: " << enum_value_count;
  CHECK_GT(enum_value_count, 0)
      << "Cannot make full interval for enum type with no values: "
      << enum_type.ToString();
  // Note: regardless of the requested underlying type of the enum we use a
  // dense unsigned space to represent the values present in the enum namespace.
  InterpValue min = InterpValue::MakeUBits(bit_count, 0);
  InterpValue max = InterpValue::MakeUBits(bit_count, enum_value_count - 1);
  InterpValueInterval result(min, max);
  VLOG(5) << "MakeFullIntervalForEnumType; result: "
          << result.ToString(/*show_types=*/false);
  return result;
}

std::optional<int64_t> GetEnumMemberIndex(const EnumType& enum_type,
                                          const InterpValue& value,
                                          const ImportData& import_data) {
  const EnumDef& enum_def = enum_type.nominal_type();
  const TypeInfo& type_info =
      *import_data.GetRootTypeInfoForNode(&enum_def).value();
  for (int64_t i = 0; i < enum_def.values().size(); ++i) {
    const EnumMember& member = enum_def.values()[i];
    InterpValue member_val = type_info.GetConstExpr(member.name_def).value();
    if (member_val == value) {
      return i;
    }
  }
  return std::nullopt;
}

InterpValueInterval MakePointIntervalForEnumType(
    const EnumType& enum_type, const InterpValue& value,
    const ImportData& import_data) {
  CHECK(value.IsEnum())
      << "MakePointIntervalForEnumType; value is not an enum: "
      << value.ToString();
  int64_t bit_count = enum_type.size().GetAsInt64().value();
  // The `value` provided is the `i`th value in the dense enum space -- let's
  // determine that value `i`.
  int64_t member_index =
      GetEnumMemberIndex(enum_type, value, import_data).value();
  const InterpValue value_as_bits =
      InterpValue::MakeUBits(bit_count, member_index);
  VLOG(5) << "MakePointIntervalForEnumType; value_as_bits: "
          << value_as_bits.ToString() << " member_index: " << member_index;
  return InterpValueInterval(value_as_bits, value_as_bits);
}

}  // namespace xls::dslx
