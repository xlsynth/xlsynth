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
#include <string>
#include <string_view>
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
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

struct FlattenedLeafType {
  const Type* type;
  std::optional<int64_t> dense_max_value;
  std::vector<int64_t> excluded_dense_values;
};

struct FlattenedLeafTypes {
  std::vector<std::unique_ptr<Type>> owned;
  std::vector<FlattenedLeafType> flat;
  // True when the semantic product being flattened has no inhabited values.
  //
  // Empty sums and empty enums contribute no storage leaf to the semantic match
  // domain. In a product such as `(Never, bool)`, the bool leaf is still needed
  // to interpret patterns, but the remaining region starts empty.
  bool is_empty = false;
};

bool IsEmptyEnum(const Type& type) {
  return type.IsEnum() && type.AsEnum().nominal_type().values().empty();
}

bool IsInhabited(const Type& type) {
  bool result;
  if (IsEmptyEnum(type)) {
    result = false;
  } else if (type.IsTuple()) {
    result = std::all_of(type.AsTuple().members().begin(),
                         type.AsTuple().members().end(),
                         [](const std::unique_ptr<Type>& member) {
                           return IsInhabited(*member);
                         });
  } else if (type.IsSum()) {
    result = std::any_of(type.AsSum().variants().begin(),
                         type.AsSum().variants().end(),
                         [](const SumTypeVariant& variant) {
                           for (int64_t i = 0; i < variant.size(); ++i) {
                             if (!IsInhabited(variant.GetMemberType(i))) {
                               return false;
                             }
                           }
                           return true;
                         });
  } else {
    result = true;
  }
  return result;
}

int64_t GetLeafTypeCount(const Type& type) {
  if (IsEmptyEnum(type)) {
    return 0;
  }
  if (type.IsTuple()) {
    int64_t result = 0;
    for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
      result += GetLeafTypeCount(*member);
    }
    return result;
  }
  if (type.IsSum()) {
    if (type.AsSum().variant_count() == 0) {
      return 0;
    }
    int64_t result = 1;
    const Phase1SumTypeEncoding encoding(type.AsSum());
    CHECK_OK(encoding.ForEachPayloadType(
        [&](const Type& payload_type) -> absl::Status {
          result += GetLeafTypeCount(payload_type);
          return absl::OkStatus();
        }));
    return result;
  }
  return 1;
}

void AppendStorageLeafTypes(const Type& type, FlattenedLeafTypes* result) {
  if (IsEmptyEnum(type)) {
    // Empty payload slots have no pattern-visible storage leaf.
  } else if (type.IsTuple()) {
    for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
      AppendStorageLeafTypes(*member, result);
    }
  } else if (type.IsSum()) {
    if (type.AsSum().variant_count() != 0) {
      const Phase1SumTypeEncoding encoding(type.AsSum());
      result->owned.push_back(std::make_unique<BitsType>(
          /*is_signed=*/false, encoding.tag_bit_count().value()));
      std::vector<int64_t> excluded_dense_values;
      for (int64_t variant_index = 0;
           variant_index < type.AsSum().variant_count(); ++variant_index) {
        const SumTypeVariant& variant =
            type.AsSum().variants().at(variant_index);
        bool is_inhabited = true;
        for (int64_t member_index = 0; member_index < variant.size();
             ++member_index) {
          if (!IsInhabited(variant.GetMemberType(member_index))) {
            is_inhabited = false;
            break;
          }
        }
        if (!is_inhabited) {
          excluded_dense_values.push_back(variant_index);
        }
      }
      result->flat.push_back(FlattenedLeafType{
          .type = result->owned.back().get(),
          .dense_max_value = type.AsSum().variant_count() - 1,
          .excluded_dense_values = std::move(excluded_dense_values),
      });
      CHECK_OK(encoding.ForEachPayloadType(
          [&](const Type& payload_type) -> absl::Status {
            AppendStorageLeafTypes(payload_type, result);
            return absl::OkStatus();
          }));
    }
  } else {
    result->flat.push_back(FlattenedLeafType{
        .type = &type,
        .dense_max_value = std::nullopt,
        .excluded_dense_values = {},
    });
  }
}

void AppendLeafTypes(const Type& type, FlattenedLeafTypes* result) {
  result->is_empty = result->is_empty || !IsInhabited(type);
  AppendStorageLeafTypes(type, result);
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

FlattenedLeafTypes GetSumVariantPayloadLeafTypes(
    const SumType& sum_type, std::string_view variant_name) {
  FlattenedLeafTypes result;
  const Phase1SumTypeEncoding encoding(sum_type);
  Phase1SumTypeEncoding::VariantInfo variant =
      encoding.GetVariant(variant_name).value();
  CHECK_OK(encoding.ForEachActivePayloadSlot(
      variant,
      [&](int64_t slot_index, int64_t active_index,
          const Type& slot_type) -> absl::Status {
        static_cast<void>(slot_index);
        static_cast<void>(active_index);
        AppendLeafTypes(slot_type, &result);
        return absl::OkStatus();
      }));
  return result;
}

// Sentinel type to indicate that some wildcard is present for a value. This
// lets us collapse out varieties of wildcards e.g. RestOfTuple and
// WildcardPattern and NameDef.
struct SomeWildcard {};

// PatternLeaf but where RestOfTuple has been resolved.
using IntervalPatternLeaf = std::variant<SomeWildcard, InterpValue, NameRef*,
                                         Range*, ColonRef*, Number*>;

InterpValueInterval MakeFullIntervalForLeafType(const FlattenedLeafType& type) {
  if (type.dense_max_value.has_value()) {
    std::optional<BitsLikeProperties> bits_like = GetBitsLike(*type.type);
    CHECK(bits_like.has_value())
        << "MakeFullIntervalForLeafType; got non-bits dense leaf type: "
        << type.type->ToString();
    int64_t bit_count = bits_like->size.GetAsInt64().value();
    return InterpValueInterval(
        InterpValue::MakeUBits(bit_count, 0),
        InterpValue::MakeUBits(bit_count, *type.dense_max_value));
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

int64_t GetDenseEnumBitCount(const EnumType& enum_type) {
  int64_t source_bit_count = enum_type.size().GetAsInt64().value();
  int64_t member_count = enum_type.nominal_type().values().size();
  int64_t maximum_member_index = std::max<int64_t>(0, member_count - 1);
  return std::max(source_bit_count,
                  Bits::MinBitCountUnsigned(maximum_member_index));
}

std::optional<int64_t> GetEnumMemberIndexByName(const EnumType& enum_type,
                                                std::string_view member_name) {
  const EnumDef& enum_def = enum_type.nominal_type();
  for (int64_t i = 0; i < enum_def.values().size(); ++i) {
    if (enum_def.values()[i].name_def->identifier() == member_name) {
      return i;
    }
  }
  return std::nullopt;
}

InterpValueInterval MakePointIntervalForEnumMember(const EnumType& enum_type,
                                                   int64_t member_index) {
  InterpValue member_position =
      InterpValue::MakeUBits(GetDenseEnumBitCount(enum_type), member_index);
  return InterpValueInterval(member_position, member_position);
}

// Follow local and imported constants back to their source enum declaration so
// equal-valued declared variants do not lose their identities in InterpValue.
const ColonRef* ResolveEnumPatternMember(const Expr& expression,
                                         const TypeInfo& type_info) {
  const Expr* current = &expression;
  const TypeInfo* current_type_info = &type_info;
  const ColonRef* member = nullptr;
  while (current != nullptr) {
    if (const NameRef* name_ref = dynamic_cast<const NameRef*>(current);
        name_ref != nullptr) {
      const ConstantDef* constant =
          dynamic_cast<const ConstantDef*>(name_ref->GetDefiner());
      current = constant == nullptr ? nullptr : constant->value();
    } else if (const ColonRef* colon_ref =
                   dynamic_cast<const ColonRef*>(current);
               colon_ref != nullptr) {
      std::optional<ImportSubject> import = colon_ref->ResolveImportSubject();
      if (!import.has_value()) {
        member = colon_ref;
        current = nullptr;
      } else if (std::optional<const ImportedInfo*> imported_info =
                     current_type_info->GetImported(*import);
                 imported_info.has_value()) {
        std::optional<ConstantDef*> constant =
            (*imported_info)->module->GetMember<ConstantDef>(colon_ref->attr());
        if (constant.has_value()) {
          current = (*constant)->value();
          current_type_info = (*imported_info)->type_info;
        } else {
          current = nullptr;
        }
      } else {
        current = nullptr;
      }
    } else {
      current = nullptr;
    }
  }
  return member;
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
    const IntervalPatternLeaf& leaf, const FlattenedLeafType& leaf_type,
    const TypeInfo& type_info, const ImportData& import_data) {
  std::optional<InterpValueInterval> result = absl::visit(
      Visitor{
          [&](SomeWildcard /*unused*/) -> std::optional<InterpValueInterval> {
            return MakeFullIntervalForLeafType(leaf_type);
          },
          [&](const InterpValue& value) -> std::optional<InterpValueInterval> {
            return MakePointIntervalForType(*leaf_type.type, value,
                                            import_data);
          },
          [&](NameRef* name_ref) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(name_ref);
            if (value.has_value()) {
              if (leaf_type.type->IsEnum()) {
                const ColonRef* enum_member =
                    ResolveEnumPatternMember(*name_ref, type_info);
                if (enum_member != nullptr) {
                  std::optional<int64_t> member_index =
                      GetEnumMemberIndexByName(leaf_type.type->AsEnum(),
                                               enum_member->attr());
                  if (member_index.has_value()) {
                    return MakePointIntervalForEnumMember(
                        leaf_type.type->AsEnum(), *member_index);
                  }
                }
              }
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
            if (leaf_type.type->IsEnum()) {
              const ColonRef* enum_member =
                  ResolveEnumPatternMember(*colon_ref, type_info);
              if (enum_member != nullptr) {
                std::optional<int64_t> member_index = GetEnumMemberIndexByName(
                    leaf_type.type->AsEnum(), enum_member->attr());
                if (member_index.has_value()) {
                  return MakePointIntervalForEnumMember(
                      leaf_type.type->AsEnum(), *member_index);
                }
              }
            }
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
          << leaf_type.type->ToString() << "` result: "
          << (result.has_value() ? result->ToString(/*show_types=*/false)
                                 : "nullopt");
  return result;
}

NdIntervalWithEmpty PatternLeavesToInterval(
    absl::Span<const IntervalPatternLeaf> pattern_leaves,
    absl::Span<const FlattenedLeafType> leaf_types, const TypeInfo& type_info,
    const ImportData& import_data) {
  CHECK_EQ(pattern_leaves.size(), leaf_types.size())
      << "Pattern leaves and leaf types must be the same size.";

  std::vector<std::optional<InterpValueInterval>> intervals;
  intervals.reserve(pattern_leaves.size());
  for (int64_t i = 0; i < pattern_leaves.size(); ++i) {
    intervals.push_back(PatternToIntervalInternal(
        pattern_leaves[i], leaf_types[i], type_info, import_data));
  }
  return NdIntervalWithEmpty(intervals);
}

IntervalPatternLeaf ToIntervalPatternLeaf(const PatternTree& pattern) {
  return absl::visit(
      Visitor{
          [&](NameDef* name_def) -> IntervalPatternLeaf {
            return SomeWildcard();
          },
          [&](NameRef* name_ref) -> IntervalPatternLeaf { return name_ref; },
          [&](Range* range) -> IntervalPatternLeaf { return range; },
          [&](ColonRef* colon_ref) -> IntervalPatternLeaf { return colon_ref; },
          [&](WildcardPattern* wildcard_pattern) -> IntervalPatternLeaf {
            return SomeWildcard();
          },
          [&](Number* number) -> IntervalPatternLeaf { return number; },
          [&](SumVariantPayloadPattern* /*constructor_pattern*/)
              -> IntervalPatternLeaf {
            LOG(FATAL) << "SumVariantPayloadPattern not yet supported in "
                          "MatchExhaustivenessChecker";
            return SomeWildcard();
          },
          [&](RestOfTuple* rest_of_tuple) -> IntervalPatternLeaf {
            LOG(FATAL) << "RestOfTuple not valid for conversion to "
                          "IntervalPatternLeaf";
            return SomeWildcard();
          },
          [&](TuplePattern* /*unused*/) -> IntervalPatternLeaf {
            LOG(FATAL) << "TuplePattern not valid for conversion to "
                          "IntervalPatternLeaf";
            return SomeWildcard();
          }},
      pattern);
}

int64_t GetSumVariantIndex(const SumType& sum_type,
                           std::string_view constructor_name) {
  return Phase1SumTypeEncoding(sum_type)
      .GetVariant(constructor_name)
      .value()
      .variant_index;
}

InterpValue MakeSumTagValue(const SumType& sum_type, int64_t variant_index) {
  int64_t bit_count = sum_type.tag_bit_count().GetAsInt64().value();
  return InterpValue::MakeUBits(bit_count, variant_index);
}

void AppendWildcardLeavesForType(const Type& type,
                                 std::vector<IntervalPatternLeaf>* result) {
  result->insert(result->end(), GetLeafTypeCount(type), SomeWildcard());
}

std::vector<IntervalPatternLeaf> ExpandPatternLeaves(
    const PatternTree& pattern, const Type& type, const FileTable& file_table);

// Expands one active payload member. Callers decide how to represent inactive
// storage slots, such as adding wildcard leaves for the full storage layout.
std::vector<IntervalPatternLeaf> ExpandActiveSumPayloadMemberPatternLeaves(
    const SumTypeVariant& variant,
    const SumVariantPayloadPattern& constructor_pattern, int64_t active_index,
    const FileTable& file_table) {
  if (variant.is_tuple()) {
    CHECK(constructor_pattern.is_tuple());
    CHECK_EQ(constructor_pattern.tuple_payload_patterns().size(),
             variant.size());
    return ExpandPatternLeaves(
        constructor_pattern.tuple_payload_patterns()[active_index],
        variant.GetMemberType(active_index), file_table);
  } else {
    CHECK(variant.is_struct());
    CHECK(constructor_pattern.is_struct());
    CHECK_EQ(constructor_pattern.struct_payload_field_patterns().size(),
             variant.size());
    const std::string_view member_name = variant.GetMemberName(active_index);
    auto it = std::find_if(
        constructor_pattern.struct_payload_field_patterns().begin(),
        constructor_pattern.struct_payload_field_patterns().end(),
        [&](const SumVariantPayloadPattern::StructPayloadFieldPattern&
                named_pattern) { return named_pattern.first == member_name; });
    CHECK(it != constructor_pattern.struct_payload_field_patterns().end())
        << "Missing named pattern for member `" << member_name << "`";
    return ExpandPatternLeaves(it->second, variant.GetMemberType(active_index),
                               file_table);
  }
}

void AppendSumVariantPayloadPatternLeaves(
    const SumTypeVariant& variant,
    const SumVariantPayloadPattern* constructor_pattern,
    const FileTable& file_table, std::vector<IntervalPatternLeaf>* result) {
  if (constructor_pattern == nullptr) {
    CHECK(variant.is_unit());
    return;
  }
  for (int64_t member_index = 0; member_index < variant.size();
       ++member_index) {
    std::vector<IntervalPatternLeaf> member_leaves =
        ExpandActiveSumPayloadMemberPatternLeaves(variant, *constructor_pattern,
                                                  member_index, file_table);
    result->insert(result->end(), member_leaves.begin(), member_leaves.end());
  }
}

struct ExpandedSumVariantPattern {
  int64_t variant_index;
  std::vector<IntervalPatternLeaf> leaves;
};

ExpandedSumVariantPattern ExpandSumVariantPayloadPatternLeaves(
    const PatternTree& pattern, const SumType& type,
    const FileTable& file_table) {
  return absl::visit(
      Visitor{
          [&](SumVariantPayloadPattern* constructor_pattern)
              -> ExpandedSumVariantPattern {
            int64_t variant_index = GetSumVariantIndex(
                type, constructor_pattern->constructor_ref()->attr());
            std::vector<IntervalPatternLeaf> result;
            AppendSumVariantPayloadPatternLeaves(type.variants()[variant_index],
                                                 constructor_pattern,
                                                 file_table, &result);
            return ExpandedSumVariantPattern{variant_index, std::move(result)};
          },
          [&](ColonRef* colon_ref) -> ExpandedSumVariantPattern {
            int64_t variant_index = GetSumVariantIndex(type, colon_ref->attr());
            CHECK(type.variants()[variant_index].is_unit());
            return ExpandedSumVariantPattern{variant_index, {}};
          },
          [&](const auto&) -> ExpandedSumVariantPattern {
            LOG(FATAL) << "Unsupported pattern for sum type `"
                       << type.ToString() << "`";
            return {0, {}};
          }},
      pattern);
}

std::vector<IntervalPatternLeaf> ExpandSumPatternLeaves(
    const PatternTree& pattern, const SumType& type,
    const FileTable& file_table) {
  const Phase1SumTypeEncoding encoding(type);
  auto make_variant_pattern_leaves =
      [&](const Phase1SumTypeEncoding::VariantInfo& active_variant,
          const SumVariantPayloadPattern* constructor_pattern)
      -> std::vector<IntervalPatternLeaf> {
    std::vector<IntervalPatternLeaf> result;
    result.push_back(MakeSumTagValue(type, active_variant.variant_index));
    const SumTypeVariant& variant = *active_variant.variant;
    CHECK_OK(encoding.VisitPayloadAssemblyOrder(
        active_variant,
        [&](int64_t active_index) -> absl::Status {
          CHECK_NE(constructor_pattern, nullptr);
          std::vector<IntervalPatternLeaf> member_leaves =
              ExpandActiveSumPayloadMemberPatternLeaves(
                  variant, *constructor_pattern, active_index, file_table);
          result.insert(result.end(), member_leaves.begin(),
                        member_leaves.end());
          return absl::OkStatus();
        },
        [&](const Type& inactive_type) -> absl::Status {
          AppendWildcardLeavesForType(inactive_type, &result);
          return absl::OkStatus();
        }));
    return result;
  };

  return absl::visit(
      Visitor{
          [&](SumVariantPayloadPattern* constructor_pattern)
              -> std::vector<IntervalPatternLeaf> {
            Phase1SumTypeEncoding::VariantInfo variant =
                encoding
                    .GetVariant(constructor_pattern->constructor_ref()->attr())
                    .value();
            return make_variant_pattern_leaves(variant, constructor_pattern);
          },
          [&](ColonRef* colon_ref) -> std::vector<IntervalPatternLeaf> {
            Phase1SumTypeEncoding::VariantInfo variant =
                encoding.GetVariant(colon_ref->attr()).value();
            CHECK(variant.variant->is_unit());
            return make_variant_pattern_leaves(variant,
                                               /*constructor_pattern=*/nullptr);
          },
          [&](const auto&) -> std::vector<IntervalPatternLeaf> {
            LOG(FATAL) << "Unsupported pattern for sum type `"
                       << type.ToString() << "`";
            return {};
          }},
      pattern);
}

std::vector<IntervalPatternLeaf> ExpandPatternLeaves(
    const PatternTree& pattern, const Type& type, const FileTable& file_table) {
  VLOG(5) << "ExpandPatternLeaves; pattern: `" << PatternToString(pattern)
          << "` type: `" << type.ToString() << "`";
  // For an irrefutable pattern, simply return wildcards for every leaf.
  if (IsIrrefutablePattern(pattern)) {
    return std::vector<IntervalPatternLeaf>(GetLeafTypeCount(type),
                                            SomeWildcard());
  }
  if (type.IsSum()) {
    CHECK(!std::holds_alternative<TuplePattern*>(pattern))
        << "Expected a leaf pattern for sum type, got `"
        << PatternToString(pattern) << "`";
    return ExpandSumPatternLeaves(pattern, type.AsSum(), file_table);
  }
  // If the type is not a tuple then we expect the pattern to be a single leaf.
  if (!type.IsTuple()) {
    CHECK(!std::holds_alternative<TuplePattern*>(pattern))
        << "Expected a single leaf for non-tuple type";
    return {ToIntervalPatternLeaf(pattern)};
  }
  // Walk through the pattern and expand any RestOfTuple markers into the
  // appropriate number of wildcards.
  //
  // In order to do this we have to recursively call to ExpandPatternLeaves for
  // any sub-tuples encountered.
  absl::Span<const std::unique_ptr<Type>> tuple_members =
      type.AsTuple().members();
  std::vector<PatternTree> flattened = FlattenPattern1(pattern);

  // Note: there can be fewer flatten1'd nodes than tuple elements because of
  // RestOfTuple markers.
  //
  // We need the `+1` here because we can have RestOfTuple markers that map to
  // zero elements in the tuple (i.e. useless/redundant ones).
  CHECK_LE(flattened.size(), tuple_members.size() + 1);

  // The results correspond to leaf types.
  std::vector<IntervalPatternLeaf> result;

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

    if (std::holds_alternative<TuplePattern*>(node)) {
      CHECK_LT(types_index, tuple_members.size());
      const Type& type_at_index = *tuple_members[types_index];

      std::vector<IntervalPatternLeaf> sub_pattern_leaves =
          ExpandPatternLeaves(node, type_at_index, file_table);

      result.insert(result.end(), sub_pattern_leaves.begin(),
                    sub_pattern_leaves.end());
      types_index += 1;
      continue;
    }
    auto append_non_rest_leaf = [&]() {
      CHECK_LT(types_index, tuple_members.size());
      const Type& type_at_index = *tuple_members[types_index];
      if (type_at_index.IsSum()) {
        std::vector<IntervalPatternLeaf> sum_pattern_leaves =
            ExpandSumPatternLeaves(node, type_at_index.AsSum(), file_table);
        result.insert(result.end(), sum_pattern_leaves.begin(),
                      sum_pattern_leaves.end());
      } else {
        result.push_back(ToIntervalPatternLeaf(node));
      }
      types_index += 1;
    };
    absl::visit(
        Visitor{
            [&](const NameRef* /*unused*/) { append_non_rest_leaf(); },
            [&](const Range* /*unused*/) { append_non_rest_leaf(); },
            [&](const ColonRef* /*unused*/) { append_non_rest_leaf(); },
            [&](const Number* /*unused*/) { append_non_rest_leaf(); },
            [&](const SumVariantPayloadPattern* /*unused*/) {
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
            [&](const TuplePattern*) {
              LOG(FATAL) << "TuplePattern reached leaf handler";
            },
            [&](const auto* irrefutable_leaf) {
              // Push back wildcards of the right size for the type.
              CHECK_LT(types_index, tuple_members.size());
              const Type& type_at_index = *tuple_members[types_index];
              AppendWildcardLeavesForType(type_at_index, &result);
              types_index += 1;
            }},
        node);
  }

  // Check that we got a consistent count between the razed tuple types and the
  // PatternLeaf vector.
  CHECK_EQ(result.size(), GetLeafTypeCount(type))
      << "Sub-pattern leaves and tuple type must be the same size.";
  return result;
}

NdIntervalWithEmpty PatternToInterval(
    const PatternTree& pattern, const Type& matched_type,
    absl::Span<const FlattenedLeafType> leaf_types, const TypeInfo& type_info,
    const ImportData& import_data) {
  std::vector<IntervalPatternLeaf> pattern_leaves =
      ExpandPatternLeaves(pattern, matched_type, type_info.file_table());
  NdIntervalWithEmpty result = PatternLeavesToInterval(
      pattern_leaves, leaf_types, type_info, import_data);
  VLOG(5) << "PatternToInterval; pattern: `" << PatternToString(pattern)
          << "` type: `" << matched_type.ToString()
          << "` result: " << result.ToString(/*show_types=*/false);
  return result;
}

std::vector<InterpValue> GetDimExtents(
    absl::Span<const InterpValueInterval> intervals) {
  std::vector<InterpValue> dim_extents;
  dim_extents.reserve(intervals.size());
  for (const InterpValueInterval& interval : intervals) {
    dim_extents.push_back(interval.max());
  }
  return dim_extents;
}

NdRegion MakeFullNdRegion(const FlattenedLeafTypes& leaf_types) {
  std::vector<InterpValueInterval> intervals =
      GetFullIntervals(leaf_types.flat);
  std::vector<InterpValue> dim_extents = GetDimExtents(intervals);
  if (leaf_types.is_empty) {
    return NdRegion::MakeEmpty(std::move(dim_extents));
  }
  NdRegion result = NdRegion::MakeFromNdInterval(NdInterval(intervals),
                                                 std::move(dim_extents));
  for (int64_t i = 0; i < leaf_types.flat.size(); ++i) {
    const FlattenedLeafType& leaf_type = leaf_types.flat.at(i);
    for (int64_t excluded_value : leaf_type.excluded_dense_values) {
      std::vector<std::optional<InterpValueInterval>> excluded_intervals(
          intervals.begin(), intervals.end());
      std::optional<BitsLikeProperties> bits_like =
          GetBitsLike(*leaf_type.type);
      CHECK(bits_like.has_value());
      int64_t bit_count = bits_like->size.GetAsInt64().value();
      InterpValue value = InterpValue::MakeUBits(bit_count, excluded_value);
      excluded_intervals[i] = InterpValueInterval(value, value);
      result = result.SubtractInterval(
          NdIntervalWithEmpty(std::move(excluded_intervals)));
    }
  }
  return result;
}

std::optional<std::vector<InterpValue>> SampleSimplestUncoveredLeafValues(
    const NdRegion& remaining, absl::Span<const FlattenedLeafType> leaf_types,
    const ImportData& import_data) {
  if (remaining.IsEmpty()) {
    return std::nullopt;
  }

  const NdInterval& nd_interval = remaining.disjoint().front();
  CHECK_EQ(nd_interval.dims().size(), leaf_types.size());
  std::vector<InterpValue> components;
  components.reserve(nd_interval.dims().size());
  for (int64_t i = 0; i < nd_interval.dims().size(); ++i) {
    const Type& type = *leaf_types[i].type;
    const InterpValueInterval& interval = nd_interval.dims()[i];
    const InterpValue& min = interval.min();
    if (type.IsEnum()) {
      const EnumType& enum_type = type.AsEnum();
      const EnumDef& enum_def = enum_type.nominal_type();

      absl::StatusOr<const TypeInfo*> enum_def_type_info =
          import_data.GetRootTypeInfoForNode(&enum_def);
      CHECK_OK(enum_def_type_info.status())
          << "Enum type info not found for enum: " << enum_type.ToString();

      int64_t member_index = min.GetBitValueUnsigned().value();
      CHECK_LT(member_index, enum_def.values().size())
          << "Member index out of bounds: " << member_index
          << " for enum: " << enum_type.ToString();
      const EnumMember& member = enum_def.values()[member_index];
      InterpValue member_value =
          enum_def_type_info.value()->GetConstExpr(member.name_def).value();
      VLOG(5) << "SampleSimplestUncoveredLeafValues; enum_type: "
              << enum_type.ToString() << " member_index: " << member_index
              << " member: " << member.name_def->ToString()
              << " member_value: " << member_value.ToString();
      components.push_back(std::move(member_value));
      continue;
    }
    components.push_back(min);
  }
  return components;
}

}  // namespace

struct MatchExhaustivenessChecker::Impl {
  struct CoveredPattern {
    NdInterval interval;
    Span span;
    std::string spelling;
    bool is_irrefutable;
  };

  struct SumVariantState {
    std::string variant_name;
    FlattenedLeafTypes leaf_types;
    NdRegion original;
    NdRegion remaining;
    std::vector<CoveredPattern> covered_patterns;
  };

  Impl(const Span& matched_expr_span, const ImportData& import_data,
       const TypeInfo& type_info, const Type& matched_type)
      : matched_expr_span_(matched_expr_span),
        import_data_(import_data),
        type_info_(type_info),
        matched_type_(matched_type),
        original_(NdRegion::MakeEmpty({})),
        remaining_(NdRegion::MakeEmpty({})) {}

  const FileTable& file_table() const { return type_info_.file_table(); }

  PatternAddResult AddInterval(const PatternTree& pattern,
                               const NdIntervalWithEmpty& interval,
                               const NdRegion& original, NdRegion& remaining,
                               std::vector<CoveredPattern>& covered_patterns) {
    PatternAddResult result{
        .coverage = PatternCoverage::kUnmatchable,
        .is_exhaustive = false,
    };
    std::optional<NdInterval> nonempty_interval = interval.ToNonEmpty();
    if (nonempty_interval.has_value()) {
      bool matches_original_domain =
          std::any_of(original.disjoint().begin(), original.disjoint().end(),
                      [&](const NdInterval& original_interval) {
                        return original_interval.Intersects(*nonempty_interval);
                      });
      if (matches_original_domain) {
        bool adds_coverage = std::any_of(
            remaining.disjoint().begin(), remaining.disjoint().end(),
            [&](const NdInterval& remaining_interval) {
              return remaining_interval.Intersects(*nonempty_interval);
            });
        if (adds_coverage) {
          result.coverage = PatternCoverage::kAddsCoverage;
          remaining = remaining.SubtractInterval(interval);
        } else {
          result.coverage = PatternCoverage::kPreviouslyCovered;
          bool is_irrefutable = IsIrrefutablePattern(pattern);
          std::string spelling = PatternToString(pattern);
          for (const CoveredPattern& previous : covered_patterns) {
            bool exact_interval =
                previous.interval.Covers(*nonempty_interval) &&
                nonempty_interval->Covers(previous.interval);
            bool same_constructor_scope =
                previous.is_irrefutable == is_irrefutable;
            bool same_wildcard_spelling =
                !is_irrefutable || previous.spelling == spelling;
            if (exact_interval && same_constructor_scope &&
                same_wildcard_spelling) {
              result.first_covering_span = previous.span;
              result.is_exact_duplicate = true;
              break;
            }
          }
          if (!result.first_covering_span.has_value()) {
            for (const CoveredPattern& previous : covered_patterns) {
              if (previous.interval.Intersects(*nonempty_interval)) {
                result.first_covering_span = previous.span;
                break;
              }
            }
          }
          CHECK(result.first_covering_span.has_value())
              << "Covered pattern has no previously matching source: "
              << spelling;
        }
        covered_patterns.push_back(CoveredPattern{
            .interval = std::move(*nonempty_interval),
            .span = GetPatternSpan(pattern),
            .spelling = PatternToString(pattern),
            .is_irrefutable = IsIrrefutablePattern(pattern),
        });
      }
    }
    return result;
  }

  const Span matched_expr_span_;
  const ImportData& import_data_;
  const TypeInfo& type_info_;
  const Type& matched_type_;
  const SumType* matched_sum_type_ = nullptr;
  FlattenedLeafTypes leaf_types_;
  std::vector<SumVariantState> sum_variant_states_;
  NdRegion original_;
  NdRegion remaining_;
  std::vector<CoveredPattern> covered_patterns_;
};

// -- class MatchExhaustivenessChecker

MatchExhaustivenessChecker::MatchExhaustivenessChecker(
    const Span& matched_expr_span, const ImportData& import_data,
    const TypeInfo& type_info, const Type& matched_type)
    : impl_(std::make_unique<Impl>(matched_expr_span, import_data, type_info,
                                   matched_type)) {
  if (impl_->matched_type_.IsSum()) {
    impl_->matched_sum_type_ = &impl_->matched_type_.AsSum();
    impl_->sum_variant_states_.reserve(
        impl_->matched_sum_type_->variant_count());
    for (const SumTypeVariant& variant : impl_->matched_sum_type_->variants()) {
      FlattenedLeafTypes variant_leaf_types = GetSumVariantPayloadLeafTypes(
          *impl_->matched_sum_type_, variant.variant().identifier());
      NdRegion variant_remaining = MakeFullNdRegion(variant_leaf_types);
      impl_->sum_variant_states_.push_back(
          Impl::SumVariantState{std::string(variant.variant().identifier()),
                                std::move(variant_leaf_types),
                                variant_remaining,
                                std::move(variant_remaining),
                                {}});
    }
    return;
  }
  impl_->leaf_types_ =
      GetLeafTypes(matched_type, matched_expr_span, file_table());
  impl_->remaining_ = MakeFullNdRegion(impl_->leaf_types_);
  impl_->original_ = impl_->remaining_;
}

MatchExhaustivenessChecker::~MatchExhaustivenessChecker() = default;

const FileTable& MatchExhaustivenessChecker::file_table() const {
  return impl_->file_table();
}

bool MatchExhaustivenessChecker::IsExhaustive() const {
  if (impl_->matched_sum_type_ != nullptr) {
    return std::all_of(impl_->sum_variant_states_.begin(),
                       impl_->sum_variant_states_.end(),
                       [](const Impl::SumVariantState& variant_state) {
                         return variant_state.remaining.IsEmpty();
                       });
  }
  return impl_->remaining_.IsEmpty();
}

MatchExhaustivenessChecker::PatternAddResult
MatchExhaustivenessChecker::AddPattern(const PatternTree& pattern) {
  VLOG(5) << "MatchExhaustivenessChecker::AddPattern: `"
          << PatternToString(pattern) << "` matched_type: `"
          << impl_->matched_type_.ToString() << "` @ "
          << GetPatternSpan(pattern).ToString(file_table());

  PatternAddResult result{
      .coverage = PatternCoverage::kUnmatchable,
      .is_exhaustive = false,
  };
  if (impl_->matched_sum_type_ != nullptr) {
    if (IsIrrefutablePattern(pattern)) {
      for (Impl::SumVariantState& variant_state : impl_->sum_variant_states_) {
        std::vector<IntervalPatternLeaf> payload_wildcards(
            variant_state.leaf_types.flat.size(), SomeWildcard());
        NdIntervalWithEmpty full_interval = PatternLeavesToInterval(
            payload_wildcards, variant_state.leaf_types.flat, impl_->type_info_,
            impl_->import_data_);
        PatternAddResult variant_result = impl_->AddInterval(
            pattern, full_interval, variant_state.original,
            variant_state.remaining, variant_state.covered_patterns);
        if (variant_result.coverage == PatternCoverage::kAddsCoverage) {
          result = variant_result;
        } else if (result.coverage != PatternCoverage::kAddsCoverage &&
                   (result.coverage == PatternCoverage::kUnmatchable ||
                    variant_result.is_exact_duplicate)) {
          result = variant_result;
        }
      }
    } else {
      CHECK(!std::holds_alternative<TuplePattern*>(pattern))
          << "Expected a leaf pattern for sum type, got `"
          << PatternToString(pattern) << "`";
      ExpandedSumVariantPattern variant_pattern =
          ExpandSumVariantPayloadPatternLeaves(
              pattern, *impl_->matched_sum_type_, file_table());
      Impl::SumVariantState& variant_state =
          impl_->sum_variant_states_.at(variant_pattern.variant_index);
      NdIntervalWithEmpty payload_interval = PatternLeavesToInterval(
          variant_pattern.leaves, variant_state.leaf_types.flat,
          impl_->type_info_, impl_->import_data_);
      result = impl_->AddInterval(
          pattern, payload_interval, variant_state.original,
          variant_state.remaining, variant_state.covered_patterns);
    }
  } else {
    NdIntervalWithEmpty this_pattern_interval = PatternToInterval(
        pattern, impl_->matched_type_, impl_->leaf_types_.flat,
        impl_->type_info_, impl_->import_data_);
    result =
        impl_->AddInterval(pattern, this_pattern_interval, impl_->original_,
                           impl_->remaining_, impl_->covered_patterns_);
  }
  result.is_exhaustive = IsExhaustive();
  return result;
}

std::optional<InterpValue>
MatchExhaustivenessChecker::SampleSimplestUncoveredValue() const {
  if (impl_->matched_sum_type_ != nullptr) {
    for (const Impl::SumVariantState& variant_state :
         impl_->sum_variant_states_) {
      std::optional<std::vector<InterpValue>> payload_values =
          SampleSimplestUncoveredLeafValues(variant_state.remaining,
                                            variant_state.leaf_types.flat,
                                            impl_->import_data_);
      if (!payload_values.has_value()) {
        continue;
      }
      absl::StatusOr<InterpValue> sample =
          CreateSumValue(*impl_->matched_sum_type_, variant_state.variant_name,
                         *payload_values);
      CHECK_OK(sample.status());
      return sample.value();
    }
    return std::nullopt;
  }

  std::optional<std::vector<InterpValue>> components =
      SampleSimplestUncoveredLeafValues(
          impl_->remaining_, impl_->leaf_types_.flat, impl_->import_data_);
  if (!components.has_value()) {
    return std::nullopt;
  }

  if (components->empty()) {
    return InterpValue::MakeTuple({});
  }
  if (components->size() == 1) {
    return (*components)[0];
  }
  return InterpValue::MakeTuple(*components);
}

InterpValueInterval MakeFullIntervalForEnumType(const EnumType& enum_type) {
  int64_t bit_count = GetDenseEnumBitCount(enum_type);
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
  // The `value` provided is the `i`th value in the dense enum space -- let's
  // determine that value `i`.
  int64_t member_index =
      GetEnumMemberIndex(enum_type, value, import_data).value();
  const InterpValue value_as_bits =
      InterpValue::MakeUBits(GetDenseEnumBitCount(enum_type), member_index);
  VLOG(5) << "MakePointIntervalForEnumType; value_as_bits: "
          << value_as_bits.ToString() << " member_index: " << member_index;
  return InterpValueInterval(value_as_bits, value_as_bits);
}

}  // namespace xls::dslx
