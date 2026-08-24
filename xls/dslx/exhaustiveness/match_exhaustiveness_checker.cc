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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/visitor.h"
#include "xls/dslx/exhaustiveness/interp_value_interval.h"
#include "xls/dslx/exhaustiveness/nd_region.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

struct EnumValueDomain {
  std::vector<InterpValue> values;
  absl::flat_hash_map<Bits, int64_t> value_indices;
};

EnumValueDomain MakeEnumValueDomain(const EnumType& enum_type) {
  EnumValueDomain result;
  result.values.reserve(enum_type.members().size());
  for (const InterpValue& member : enum_type.members()) {
    if (result.value_indices
            .try_emplace(member.GetBitsOrDie(), result.values.size())
            .second) {
      result.values.push_back(member);
    }
  }
  return result;
}

struct FlattenedLeafType {
  const Type* type;
  std::optional<int64_t> dense_max_value;
  std::vector<int64_t> excluded_dense_values;
  std::optional<EnumValueDomain> enum_domain;
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
          .enum_domain = std::nullopt,
      });
      CHECK_OK(encoding.ForEachPayloadType(
          [&](const Type& payload_type) -> absl::Status {
            AppendStorageLeafTypes(payload_type, result);
            return absl::OkStatus();
          }));
    }
  } else {
    std::optional<EnumValueDomain> enum_domain;
    if (type.IsEnum()) {
      enum_domain = MakeEnumValueDomain(type.AsEnum());
    }
    result->flat.push_back(FlattenedLeafType{
        .type = &type,
        .dense_max_value = std::nullopt,
        .excluded_dense_values = {},
        .enum_domain = std::move(enum_domain),
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
  if (type.enum_domain.has_value()) {
    const EnumType& enum_type = type.type->AsEnum();
    CHECK(!type.enum_domain->values.empty());
    int64_t bit_count = enum_type.size().GetAsInt64().value();
    return InterpValueInterval(
        InterpValue::MakeUBits(bit_count, 0),
        InterpValue::MakeUBits(bit_count, type.enum_domain->values.size() - 1));
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

InterpValueInterval MakePointIntervalForLeafType(
    const FlattenedLeafType& leaf_type, const InterpValue& value) {
  const Type& type = *leaf_type.type;
  VLOG(5) << "MakePointIntervalForLeafType; type: `" << type.ToString()
          << "` value: `" << value.ToString() << "`";
  if (type.IsEnum()) {
    CHECK(value.IsEnum());
    CHECK_EQ(value.GetEnumData()->def, &type.AsEnum().nominal_type())
        << "Enum value belongs to a different nominal enum type.";
    CHECK(leaf_type.enum_domain.has_value());
    const auto it =
        leaf_type.enum_domain->value_indices.find(value.GetBitsOrDie());
    CHECK(it != leaf_type.enum_domain->value_indices.end());
    InterpValue coordinate = InterpValue::MakeUBits(
        type.AsEnum().size().GetAsInt64().value(), it->second);
    return InterpValueInterval(coordinate, coordinate);
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
    const TypeInfo& type_info) {
  std::optional<InterpValueInterval> result = absl::visit(
      Visitor{
          [&](SomeWildcard /*unused*/) -> std::optional<InterpValueInterval> {
            return MakeFullIntervalForLeafType(leaf_type);
          },
          [&](const InterpValue& value) -> std::optional<InterpValueInterval> {
            return MakePointIntervalForLeafType(leaf_type, value);
          },
          [&](NameRef* name_ref) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(name_ref);
            if (value.has_value()) {
              return MakePointIntervalForLeafType(leaf_type, *value);
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
            return MakePointIntervalForLeafType(leaf_type, *value);
          },
          [&](Number* number) -> std::optional<InterpValueInterval> {
            std::optional<InterpValue> value =
                type_info.GetConstExprOption(number);
            CHECK(value.has_value());
            return MakePointIntervalForLeafType(leaf_type, *value);
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
    absl::Span<const FlattenedLeafType> leaf_types, const TypeInfo& type_info) {
  CHECK_EQ(pattern_leaves.size(), leaf_types.size())
      << "Pattern leaves and leaf types must be the same size.";

  std::vector<std::optional<InterpValueInterval>> intervals;
  intervals.reserve(pattern_leaves.size());
  for (int64_t i = 0; i < pattern_leaves.size(); ++i) {
    intervals.push_back(
        PatternToIntervalInternal(pattern_leaves[i], leaf_types[i], type_info));
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
          [&](StructPattern* /*unused*/) -> IntervalPatternLeaf {
            LOG(FATAL) << "StructPattern not valid for conversion to "
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
  int64_t bit_count = sum_type.storage_tag_bit_count().GetAsInt64().value();
  return InterpValue::MakeUBits(bit_count, variant_index);
}

void AppendWildcardLeavesForType(const Type& type,
                                 std::vector<IntervalPatternLeaf>* result) {
  result->insert(result->end(), GetLeafTypeCount(type), SomeWildcard());
}

std::vector<IntervalPatternLeaf> ExpandPatternLeaves(
    const PatternTree& pattern, const Type& type, const TypeInfo& type_info,
    const FileTable& file_table);

struct SumConstantValue {
  InterpValue value;
  int64_t variant_index;
};

SumConstantValue ResolveSumConstantValue(const Expr& expression,
                                         const SumType& sum_type,
                                         const TypeInfo& type_info) {
  std::optional<InterpValue> value = type_info.GetConstExprOption(&expression);
  if (!value.has_value()) {
    const Expr* constructor_expression = &expression;
    bool resolving_local_alias = true;
    while (resolving_local_alias) {
      resolving_local_alias = false;
      if (const auto* name_ref =
              dynamic_cast<const NameRef*>(constructor_expression);
          name_ref != nullptr) {
        const AstNode* definer = name_ref->GetDefiner();
        if (const auto* constant = dynamic_cast<const ConstantDef*>(definer);
            constant != nullptr) {
          value = type_info.GetConstExprOption(constant->name_def());
          if (!value.has_value()) {
            constructor_expression = constant->value();
            resolving_local_alias = true;
          }
        } else if (const auto* binding = dynamic_cast<const Let*>(definer);
                   binding != nullptr && binding->is_const()) {
          constructor_expression = binding->rhs();
          resolving_local_alias = true;
        }
      } else if (const auto* colon_ref =
                     dynamic_cast<const ColonRef*>(constructor_expression);
                 colon_ref != nullptr) {
        absl::StatusOr<TypeInfo::ResolvedColonRefSubject> subject =
            type_info.GetResolvedColonRefSubject(colon_ref);
        if (subject.ok()) {
          if (Impl* const* impl = std::get_if<Impl*>(&*subject);
              impl != nullptr) {
            std::optional<ConstantDef*> constant =
                (*impl)->GetConstant(colon_ref->attr());
            if (constant.has_value()) {
              value = type_info.GetConstExprOption((*constant)->name_def());
              if (!value.has_value()) {
                constructor_expression = (*constant)->value();
                resolving_local_alias = true;
              }
            }
          }
        }
      }
    }
    if (!value.has_value()) {
      // Validation precedes collection for phase-1 unit-constructor aliases.
      // Preserve only this timing gap; payload/import values are owned by the
      // authoritative TypeInfo constexpr producer.
      if (const auto* constructor =
              dynamic_cast<const ColonRef*>(constructor_expression);
          constructor != nullptr) {
        absl::StatusOr<Phase1SumTypeEncoding::VariantInfo> variant =
            Phase1SumTypeEncoding(sum_type).GetVariant(constructor->attr());
        if (variant.ok() && variant->variant->is_unit()) {
          value = CreateSumValue(sum_type, constructor->attr(), {}).value();
        }
      }
    }
  }
  CHECK(value.has_value()) << "Missing semantic-sum constexpr value for `"
                           << expression.ToString() << "`";
  absl::StatusOr<internal::EncodedSumView> encoded =
      internal::GetEncodedSumView(*value);
  CHECK_OK(encoded.status()) << "Invalid semantic-sum constexpr value for `"
                             << expression.ToString() << "`";
  int64_t variant_index = encoded->tag.GetBitValueUnsigned().value();
  return SumConstantValue{
      .value = std::move(*value),
      .variant_index = variant_index,
  };
}

void AppendConstantValueLeaves(const InterpValue& value, const Type& type,
                               std::vector<IntervalPatternLeaf>* result) {
  if (type.IsTuple()) {
    absl::Span<const std::unique_ptr<Type>> members = type.AsTuple().members();
    const std::vector<InterpValue>& member_values = value.GetValuesOrDie();
    CHECK_EQ(member_values.size(), members.size());
    for (int64_t i = 0; i < members.size(); ++i) {
      AppendConstantValueLeaves(member_values[i], *members[i], result);
    }
  } else if (type.IsSum()) {
    const SumType& sum_type = type.AsSum();
    absl::StatusOr<internal::EncodedSumView> encoded =
        internal::GetEncodedSumView(value);
    CHECK_OK(encoded.status());
    int64_t variant_index = encoded->tag.GetBitValueUnsigned().value();
    CHECK_LT(variant_index, sum_type.variants().size());
    const Phase1SumTypeEncoding encoding(sum_type);
    Phase1SumTypeEncoding::VariantInfo variant =
        encoding
            .GetVariant(
                sum_type.variants()[variant_index].variant().identifier())
            .value();
    result->push_back(MakeSumTagValue(sum_type, variant.variant_index));
    std::vector<const InterpValue*> active_payload_values(
        variant.variant->size(), nullptr);
    CHECK_OK(encoding.ForEachActivePayloadSlot(
        variant,
        [&](int64_t slot_index, int64_t active_index,
            const Type& /*slot_type*/) -> absl::Status {
          CHECK_LT(slot_index, encoded->payload_slots.size());
          active_payload_values[active_index] =
              &encoded->payload_slots[slot_index];
          return absl::OkStatus();
        }));
    CHECK_OK(encoding.VisitPayloadAssemblyOrder(
        variant,
        [&](int64_t active_index) -> absl::Status {
          CHECK(active_payload_values[active_index] != nullptr);
          AppendConstantValueLeaves(
              *active_payload_values[active_index],
              variant.variant->GetMemberType(active_index), result);
          return absl::OkStatus();
        },
        [&](const Type& inactive_type) -> absl::Status {
          AppendWildcardLeavesForType(inactive_type, result);
          return absl::OkStatus();
        }));
  } else {
    result->push_back(value);
  }
}

void AppendSumConstructorPayloadLeaves(
    const SumConstantValue& constant, const SumType& sum_type,
    std::vector<IntervalPatternLeaf>* result) {
  const Phase1SumTypeEncoding encoding(sum_type);
  CHECK_LT(constant.variant_index, sum_type.variants().size());
  Phase1SumTypeEncoding::VariantInfo variant =
      encoding
          .GetVariant(sum_type.variants()[constant.variant_index]
                          .variant()
                          .identifier())
          .value();
  absl::StatusOr<internal::EncodedSumView> encoded =
      internal::GetEncodedSumView(constant.value);
  CHECK_OK(encoded.status());
  CHECK_OK(encoding.ForEachActivePayloadSlot(
      variant,
      [&](int64_t slot_index, int64_t active_index,
          const Type& slot_type) -> absl::Status {
        static_cast<void>(active_index);
        CHECK_LT(slot_index, encoded->payload_slots.size());
        AppendConstantValueLeaves(encoded->payload_slots[slot_index], slot_type,
                                  result);
        return absl::OkStatus();
      }));
}

// Expands one active payload member. Callers decide how to represent inactive
// storage slots, such as adding wildcard leaves for the full storage layout.
std::vector<IntervalPatternLeaf> ExpandActiveSumPayloadMemberPatternLeaves(
    const SumTypeVariant& variant,
    const SumVariantPayloadPattern& constructor_pattern, int64_t active_index,
    const TypeInfo& type_info, const FileTable& file_table) {
  if (variant.is_tuple()) {
    const auto* payload =
        std::get_if<TuplePattern*>(&constructor_pattern.payload());
    CHECK(payload != nullptr);
    CHECK_EQ((*payload)->members().size(), variant.size());
    return ExpandPatternLeaves((*payload)->members()[active_index],
                               variant.GetMemberType(active_index), type_info,
                               file_table);
  } else {
    CHECK(variant.is_struct());
    const auto* payload =
        std::get_if<StructPattern*>(&constructor_pattern.payload());
    CHECK(payload != nullptr);
    const std::vector<StructPattern::Field>& fields = (*payload)->fields();
    CHECK_EQ(fields.size(), variant.size());
    const std::string_view member_name = variant.GetMemberName(active_index);
    auto it = std::find_if(fields.begin(), fields.end(),
                           [&](const StructPattern::Field& named_pattern) {
                             return named_pattern.first == member_name;
                           });
    CHECK(it != fields.end())
        << "Missing named pattern for member `" << member_name << "`";
    return ExpandPatternLeaves(it->second, variant.GetMemberType(active_index),
                               type_info, file_table);
  }
}

void AppendSumVariantPayloadPatternLeaves(
    const SumTypeVariant& variant,
    const SumVariantPayloadPattern* constructor_pattern,
    const TypeInfo& type_info, const FileTable& file_table,
    std::vector<IntervalPatternLeaf>* result) {
  if (constructor_pattern == nullptr) {
    CHECK(variant.is_unit());
    return;
  }
  for (int64_t member_index = 0; member_index < variant.size();
       ++member_index) {
    std::vector<IntervalPatternLeaf> member_leaves =
        ExpandActiveSumPayloadMemberPatternLeaves(
            variant, *constructor_pattern, member_index, type_info, file_table);
    result->insert(result->end(), member_leaves.begin(), member_leaves.end());
  }
}

struct ExpandedSumVariantPattern {
  int64_t variant_index;
  std::vector<IntervalPatternLeaf> leaves;
};

std::optional<Phase1SumTypeEncoding::VariantInfo> GetDirectUnitSumVariant(
    const ColonRef& pattern, const SumType& type, const TypeInfo& type_info) {
  std::optional<Phase1SumTypeEncoding::VariantInfo> result;
  if (!type_info.IsKnownConstExpr(&pattern)) {
    absl::StatusOr<Phase1SumTypeEncoding::VariantInfo> variant =
        Phase1SumTypeEncoding(type).GetVariant(pattern.attr());
    if (variant.ok() && variant->variant->is_unit()) {
      result.emplace(*variant);
    }
  }
  return result;
}

ExpandedSumVariantPattern ExpandSumVariantPayloadPatternLeaves(
    const PatternTree& pattern, const SumType& type, const TypeInfo& type_info,
    const FileTable& file_table) {
  auto expand_constant = [&](const Expr& expression) {
    SumConstantValue constant =
        ResolveSumConstantValue(expression, type, type_info);
    std::vector<IntervalPatternLeaf> leaves;
    AppendSumConstructorPayloadLeaves(constant, type, &leaves);
    return ExpandedSumVariantPattern{constant.variant_index, std::move(leaves)};
  };
  return absl::visit(
      Visitor{[&](SumVariantPayloadPattern* constructor_pattern)
                  -> ExpandedSumVariantPattern {
                int64_t variant_index = GetSumVariantIndex(
                    type, constructor_pattern->constructor_ref()->attr());
                std::vector<IntervalPatternLeaf> result;
                AppendSumVariantPayloadPatternLeaves(
                    type.variants()[variant_index], constructor_pattern,
                    type_info, file_table, &result);
                return ExpandedSumVariantPattern{variant_index,
                                                 std::move(result)};
              },
              [&](ColonRef* colon_ref) -> ExpandedSumVariantPattern {
                std::optional<Phase1SumTypeEncoding::VariantInfo> variant =
                    GetDirectUnitSumVariant(*colon_ref, type, type_info);
                if (variant.has_value()) {
                  return ExpandedSumVariantPattern{variant->variant_index, {}};
                }
                return expand_constant(*colon_ref);
              },
              [&](NameRef* name_ref) -> ExpandedSumVariantPattern {
                return expand_constant(*name_ref);
              },
              [&](const auto&) -> ExpandedSumVariantPattern {
                LOG(FATAL) << "Unsupported pattern for sum type `"
                           << type.ToString() << "`";
                return {0, {}};
              }},
      pattern);
}

std::vector<IntervalPatternLeaf> ExpandSumPatternLeaves(
    const PatternTree& pattern, const SumType& type, const TypeInfo& type_info,
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
          if (constructor_pattern != nullptr) {
            std::vector<IntervalPatternLeaf> member_leaves =
                ExpandActiveSumPayloadMemberPatternLeaves(
                    variant, *constructor_pattern, active_index, type_info,
                    file_table);
            result.insert(result.end(), member_leaves.begin(),
                          member_leaves.end());
          } else {
            CHECK(variant.is_unit());
          }
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
            std::optional<Phase1SumTypeEncoding::VariantInfo> variant =
                GetDirectUnitSumVariant(*colon_ref, type, type_info);
            if (variant.has_value()) {
              return make_variant_pattern_leaves(
                  *variant, /*constructor_pattern=*/nullptr);
            }
            SumConstantValue constant =
                ResolveSumConstantValue(*colon_ref, type, type_info);
            std::vector<IntervalPatternLeaf> result;
            AppendConstantValueLeaves(constant.value, type, &result);
            return result;
          },
          [&](NameRef* name_ref) -> std::vector<IntervalPatternLeaf> {
            SumConstantValue constant =
                ResolveSumConstantValue(*name_ref, type, type_info);
            std::vector<IntervalPatternLeaf> result;
            AppendConstantValueLeaves(constant.value, type, &result);
            return result;
          },
          [&](const auto&) -> std::vector<IntervalPatternLeaf> {
            LOG(FATAL) << "Unsupported pattern for sum type `"
                       << type.ToString() << "`";
            return {};
          }},
      pattern);
}

std::vector<IntervalPatternLeaf> ExpandPatternLeaves(
    const PatternTree& pattern, const Type& type, const TypeInfo& type_info,
    const FileTable& file_table) {
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
    return ExpandSumPatternLeaves(pattern, type.AsSum(), type_info, file_table);
  }
  // If the type is not a tuple then we expect the pattern to be a single leaf.
  if (!type.IsTuple()) {
    CHECK(!std::holds_alternative<TuplePattern*>(pattern))
        << "Expected a single leaf for non-tuple type";
    return {ToIntervalPatternLeaf(pattern)};
  }
  if (const auto* name_ref = std::get_if<NameRef*>(&pattern);
      name_ref != nullptr) {
    std::optional<InterpValue> value = type_info.GetConstExprOption(*name_ref);
    CHECK(value.has_value()) << "Missing tuple constexpr value for `"
                             << (*name_ref)->ToString() << "`";
    std::vector<IntervalPatternLeaf> result;
    AppendConstantValueLeaves(*value, type, &result);
    return result;
  } else if (const auto* colon_ref = std::get_if<ColonRef*>(&pattern);
             colon_ref != nullptr) {
    std::optional<InterpValue> value = type_info.GetConstExprOption(*colon_ref);
    CHECK(value.has_value()) << "Missing tuple constexpr value for `"
                             << (*colon_ref)->ToString() << "`";
    std::vector<IntervalPatternLeaf> result;
    AppendConstantValueLeaves(*value, type, &result);
    return result;
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
          ExpandPatternLeaves(node, type_at_index, type_info, file_table);

      result.insert(result.end(), sub_pattern_leaves.begin(),
                    sub_pattern_leaves.end());
      types_index += 1;
      continue;
    }
    auto append_non_rest_leaf = [&]() {
      CHECK_LT(types_index, tuple_members.size());
      const Type& type_at_index = *tuple_members[types_index];
      if (type_at_index.IsSum() || type_at_index.IsTuple()) {
        std::vector<IntervalPatternLeaf> member_pattern_leaves =
            ExpandPatternLeaves(node, type_at_index, type_info, file_table);
        result.insert(result.end(), member_pattern_leaves.begin(),
                      member_pattern_leaves.end());
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
    absl::Span<const FlattenedLeafType> leaf_types, const TypeInfo& type_info) {
  std::vector<IntervalPatternLeaf> pattern_leaves = ExpandPatternLeaves(
      pattern, matched_type, type_info, type_info.file_table());
  NdIntervalWithEmpty result =
      PatternLeavesToInterval(pattern_leaves, leaf_types, type_info);
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

std::string FormatSumVariant(const SumType& sum_type,
                             const SumTypeVariant& variant,
                             absl::Span<const std::string> payload_values) {
  CHECK_EQ(variant.size(), payload_values.size());
  std::string result = sum_type.nominal_type().identifier();
  result += "::";
  result += variant.variant().identifier();

  if (variant.is_unit()) {
    CHECK(payload_values.empty());
  } else if (variant.is_tuple()) {
    result += "(";
    for (int64_t i = 0; i < payload_values.size(); ++i) {
      if (i != 0) {
        result += ", ";
      }
      result += payload_values[i];
    }
    result += ")";
  } else {
    CHECK(variant.is_struct());
    result += " {";
    for (int64_t i = 0; i < payload_values.size(); ++i) {
      result += i == 0 ? " " : ", ";
      result += variant.GetMemberName(i);
      result += ": ";
      result += payload_values[i];
    }
    result += " }";
  }
  return result;
}

std::string FormatSampleForType(
    const Type& type, absl::Span<const InterpValueInterval> dimensions,
    absl::Span<const FlattenedLeafType> leaf_types, int64_t* leaf_index) {
  std::string result;
  if (type.IsEnum()) {
    CHECK_LT(*leaf_index, dimensions.size());
    const EnumType& enum_type = type.AsEnum();
    const EnumDef& enum_def = enum_type.nominal_type();
    const FlattenedLeafType& leaf_type = leaf_types[*leaf_index];
    CHECK(leaf_type.enum_domain.has_value());
    int64_t value_index =
        dimensions[(*leaf_index)++].min().GetBitValueUnsigned().value();
    const std::vector<InterpValue>& distinct_values =
        leaf_type.enum_domain->values;
    CHECK_LT(value_index, distinct_values.size());
    const InterpValue& value = distinct_values[value_index];
    int64_t member_index = 0;
    while (member_index < enum_type.members().size() &&
           !enum_type.members()[member_index].Eq(value)) {
      ++member_index;
    }
    CHECK_LT(member_index, enum_def.values().size());
    result = enum_def.identifier();
    result += "::";
    result += enum_def.GetMemberName(member_index);
  } else if (type.IsTuple()) {
    result += "(";
    const TupleType& tuple_type = type.AsTuple();
    for (int64_t i = 0; i < tuple_type.size(); ++i) {
      if (i != 0) {
        result += ", ";
      }
      result += FormatSampleForType(tuple_type.GetMemberType(i), dimensions,
                                    leaf_types, leaf_index);
    }
    result += ")";
  } else if (type.IsSum()) {
    CHECK_LT(*leaf_index, dimensions.size());
    const SumType& sum_type = type.AsSum();
    int64_t variant_index =
        dimensions[(*leaf_index)++].min().GetBitValueUnsigned().value();
    CHECK_LT(variant_index, sum_type.variant_count());
    const SumTypeVariant& variant = sum_type.variants().at(variant_index);
    const Phase1SumTypeEncoding encoding(sum_type);
    Phase1SumTypeEncoding::VariantInfo variant_info =
        encoding.GetVariant(variant.variant().identifier()).value();
    std::vector<std::string> payload_values;
    payload_values.reserve(variant.size());
    CHECK_OK(encoding.VisitPayloadAssemblyOrder(
        variant_info,
        [&](int64_t active_index) -> absl::Status {
          payload_values.push_back(FormatSampleForType(
              variant.GetMemberType(active_index), dimensions, leaf_types,
              leaf_index));
          return absl::OkStatus();
        },
        [&](const Type& inactive_type) -> absl::Status {
          *leaf_index += GetLeafTypeCount(inactive_type);
          CHECK_LE(*leaf_index, dimensions.size());
          return absl::OkStatus();
        }));
    result = FormatSumVariant(sum_type, variant, payload_values);
  } else {
    CHECK_LT(*leaf_index, dimensions.size());
    result = dimensions[(*leaf_index)++].min().ToString();
  }
  return result;
}

std::string FormatLegacySample(
    absl::Span<const InterpValueInterval> dimensions) {
  std::vector<InterpValue> components;
  components.reserve(dimensions.size());
  for (const InterpValueInterval& interval : dimensions) {
    components.push_back(interval.min());
  }

  std::string result;
  if (components.size() == 1) {
    result = components.front().ToString();
  } else {
    result = InterpValue::MakeTuple(components).ToString();
  }
  return result;
}

}  // namespace

struct MatchExhaustivenessChecker::Impl {
  struct CoverageDomain {
    NdRegion original;
    NdRegion remaining;
    // PatternTree wrappers are copyable values containing AST-owned pointers.
    // Retain the wrapper itself because callers may pass temporary wrappers.
    std::vector<PatternTree> covered_patterns;
    // Compact semantic fingerprints point into owned wrappers; collisions are
    // resolved against the actual inhabited intervals.
    absl::flat_hash_map<size_t, std::vector<int64_t>> covered_pattern_indices;
    std::optional<Span> covering_pattern_span;
    // Trailing patterns cannot add coverage; their history need not be
    // replayed.
    std::optional<int64_t> exhaustive_pattern_count;
  };

  struct SumVariantState {
    std::string variant_name;
    FlattenedLeafTypes leaf_types;
    CoverageDomain coverage;
  };

  Impl(const Span& matched_expr_span, const TypeInfo& type_info,
       const Type& matched_type)
      : matched_expr_span_(matched_expr_span),
        type_info_(type_info),
        matched_type_(matched_type),
        coverage_(CoverageDomain{
            .original = NdRegion::MakeEmpty({}),
            .remaining = NdRegion::MakeEmpty({}),
            .covered_patterns = {},
            .covered_pattern_indices = {},
            .covering_pattern_span = std::nullopt,
            .exhaustive_pattern_count = std::nullopt,
        }) {}

  const FileTable& file_table() const { return type_info_.file_table(); }

  static size_t SemanticPatternFingerprint(const NdInterval& interval,
                                           const NdRegion& domain,
                                           bool is_irrefutable,
                                           std::string_view spelling) {
    size_t fingerprint = absl::HashOf(
        is_irrefutable, is_irrefutable ? spelling : std::string_view{});
    for (const NdInterval& original : domain.disjoint()) {
      if (!original.Intersects(interval)) {
        continue;
      }
      for (int64_t i = 0; i < interval.dims().size(); ++i) {
        const InterpValueInterval& candidate = interval.dims()[i];
        const InterpValueInterval& inhabited = original.dims()[i];
        const InterpValue& minimum = candidate.min() < inhabited.min()
                                         ? inhabited.min()
                                         : candidate.min();
        const InterpValue& maximum = inhabited.max() < candidate.max()
                                         ? inhabited.max()
                                         : candidate.max();
        fingerprint = absl::HashOf(fingerprint, minimum.GetBitsOrDie(),
                                   maximum.GetBitsOrDie());
      }
    }
    return fingerprint;
  }

  static bool CoversSameInhabitedValues(const NdInterval& first,
                                        const NdInterval& second,
                                        const NdRegion& domain) {
    auto has_coverage_outside = [&](const NdInterval& included,
                                    const NdInterval& excluded) {
      return std::any_of(domain.disjoint().begin(), domain.disjoint().end(),
                         [&](const NdInterval& original_interval) {
                           std::vector<NdInterval> uncovered =
                               original_interval.SubtractInterval(excluded);
                           return std::any_of(
                               uncovered.begin(), uncovered.end(),
                               [&](const NdInterval& candidate) {
                                 return included.Intersects(candidate);
                               });
                         });
    };
    return !has_coverage_outside(first, second) &&
           !has_coverage_outside(second, first);
  }

  NdInterval ReconstructCoveredInterval(
      const PatternTree& pattern,
      const FlattenedLeafTypes& domain_leaf_types) const {
    NdIntervalWithEmpty interval = [&]() {
      if (matched_sum_type_ == nullptr) {
        return PatternToInterval(pattern, matched_type_, domain_leaf_types.flat,
                                 type_info_);
      } else if (IsIrrefutablePattern(pattern)) {
        std::vector<IntervalPatternLeaf> wildcards(
            domain_leaf_types.flat.size(), SomeWildcard());
        return PatternLeavesToInterval(wildcards, domain_leaf_types.flat,
                                       type_info_);
      } else {
        ExpandedSumVariantPattern variant_pattern =
            ExpandSumVariantPayloadPatternLeaves(pattern, *matched_sum_type_,
                                                 type_info_, file_table());
        return PatternLeavesToInterval(variant_pattern.leaves,
                                       domain_leaf_types.flat, type_info_);
      }
    }();
    std::optional<NdInterval> nonempty = interval.ToNonEmpty();
    CHECK(nonempty.has_value());
    return *std::move(nonempty);
  }

  PatternAddResult AddInterval(const PatternTree& pattern,
                               const NdIntervalWithEmpty& interval,
                               const FlattenedLeafTypes& domain_leaf_types,
                               CoverageDomain& domain) {
    PatternAddResult result{
        .outcome = PatternAddResult::Unmatchable{},
    };
    std::optional<NdInterval> nonempty_interval = interval.ToNonEmpty();
    if (nonempty_interval.has_value()) {
      bool matches_original_domain = std::any_of(
          domain.original.disjoint().begin(), domain.original.disjoint().end(),
          [&](const NdInterval& original_interval) {
            return original_interval.Intersects(*nonempty_interval);
          });
      if (matches_original_domain) {
        bool is_irrefutable = IsIrrefutablePattern(pattern);
        std::string spelling = PatternToString(pattern);
        size_t fingerprint = SemanticPatternFingerprint(
            *nonempty_interval, domain.original, is_irrefutable, spelling);
        bool adds_coverage = std::any_of(
            domain.remaining.disjoint().begin(),
            domain.remaining.disjoint().end(),
            [&](const NdInterval& remaining_interval) {
              return remaining_interval.Intersects(*nonempty_interval);
            });
        if (adds_coverage) {
          result.outcome = PatternAddResult::AddsCoverage{};
          domain.remaining = domain.remaining.SubtractInterval(interval);
          if (domain.remaining.IsEmpty()) {
            domain.exhaustive_pattern_count =
                domain.covered_patterns.size() + 1;
          }
        } else {
          std::optional<Span> first_intersecting_span;
          std::optional<Span> exact_previous_span;
          const auto candidates =
              domain.covered_pattern_indices.find(fingerprint);
          if (candidates != domain.covered_pattern_indices.end()) {
            for (int64_t candidate_index : candidates->second) {
              const PatternTree& previous =
                  domain.covered_patterns[candidate_index];
              NdInterval previous_interval =
                  ReconstructCoveredInterval(previous, domain_leaf_types);
              bool same_constructor_scope =
                  IsIrrefutablePattern(previous) == is_irrefutable;
              bool same_wildcard_spelling =
                  !is_irrefutable || PatternToString(previous) == spelling;
              if (same_constructor_scope && same_wildcard_spelling &&
                  CoversSameInhabitedValues(
                      previous_interval, *nonempty_interval, domain.original)) {
                exact_previous_span = GetPatternSpan(previous);
                break;
              }
            }
          }
          if (exact_previous_span.has_value()) {
            first_intersecting_span = exact_previous_span;
          } else if (domain.covering_pattern_span.has_value()) {
            first_intersecting_span = domain.covering_pattern_span;
          } else {
            int64_t search_count = domain.exhaustive_pattern_count.value_or(
                domain.covered_patterns.size());
            for (int64_t i = 0; i < search_count; ++i) {
              const PatternTree& previous = domain.covered_patterns[i];
              NdInterval previous_interval =
                  ReconstructCoveredInterval(previous, domain_leaf_types);
              if (previous_interval.Intersects(*nonempty_interval)) {
                first_intersecting_span = GetPatternSpan(previous);
                break;
              }
            }
          }
          CHECK(exact_previous_span.has_value() ||
                first_intersecting_span.has_value())
              << "Covered pattern has no previously matching source: "
              << spelling;
          result.outcome = PatternAddResult::Overlap{
              .kind = exact_previous_span.has_value()
                          ? MatchPatternOverlapKind::kExactDuplicate
                          : MatchPatternOverlapKind::kFullyCovered,
              .previous_pattern_span = exact_previous_span.has_value()
                                           ? *exact_previous_span
                                           : *first_intersecting_span,
          };
        }
        if (is_irrefutable && !domain.covering_pattern_span.has_value()) {
          domain.covering_pattern_span = GetPatternSpan(pattern);
        }
        domain.covered_pattern_indices[fingerprint].push_back(
            domain.covered_patterns.size());
        domain.covered_patterns.push_back(pattern);
      }
    }
    return result;
  }

  const Span matched_expr_span_;
  const TypeInfo& type_info_;
  const Type& matched_type_;
  const SumType* matched_sum_type_ = nullptr;
  FlattenedLeafTypes leaf_types_;
  std::vector<SumVariantState> sum_variant_states_;
  CoverageDomain coverage_;
};

// -- class MatchExhaustivenessChecker

MatchExhaustivenessChecker::MatchExhaustivenessChecker(
    const Span& matched_expr_span, const TypeInfo& type_info,
    const Type& matched_type)
    : impl_(
          std::make_unique<Impl>(matched_expr_span, type_info, matched_type)) {
  if (impl_->matched_type_.IsSum()) {
    impl_->matched_sum_type_ = &impl_->matched_type_.AsSum();
    impl_->sum_variant_states_.reserve(
        impl_->matched_sum_type_->variant_count());
    for (const SumTypeVariant& variant : impl_->matched_sum_type_->variants()) {
      FlattenedLeafTypes variant_leaf_types = GetSumVariantPayloadLeafTypes(
          *impl_->matched_sum_type_, variant.variant().identifier());
      NdRegion variant_remaining = MakeFullNdRegion(variant_leaf_types);
      impl_->sum_variant_states_.push_back(Impl::SumVariantState{
          .variant_name = std::string(variant.variant().identifier()),
          .leaf_types = std::move(variant_leaf_types),
          .coverage =
              Impl::CoverageDomain{
                  .original = variant_remaining,
                  .remaining = std::move(variant_remaining),
                  .covered_patterns = {},
              },
      });
    }
    return;
  }
  impl_->leaf_types_ =
      GetLeafTypes(matched_type, matched_expr_span, file_table());
  impl_->coverage_.remaining = MakeFullNdRegion(impl_->leaf_types_);
  impl_->coverage_.original = impl_->coverage_.remaining;
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
                         return variant_state.coverage.remaining.IsEmpty();
                       });
  }
  return impl_->coverage_.remaining.IsEmpty();
}

MatchExhaustivenessChecker::PatternAddResult
MatchExhaustivenessChecker::AddPattern(const PatternTree& pattern) {
  VLOG(5) << "MatchExhaustivenessChecker::AddPattern: `"
          << PatternToString(pattern) << "` matched_type: `"
          << impl_->matched_type_.ToString() << "` @ "
          << GetPatternSpan(pattern).ToString(file_table());

  PatternAddResult result{
      .outcome = PatternAddResult::Unmatchable{},
  };
  if (impl_->matched_sum_type_ != nullptr) {
    if (IsIrrefutablePattern(pattern)) {
      for (Impl::SumVariantState& variant_state : impl_->sum_variant_states_) {
        std::vector<IntervalPatternLeaf> payload_wildcards(
            variant_state.leaf_types.flat.size(), SomeWildcard());
        NdIntervalWithEmpty full_interval = PatternLeavesToInterval(
            payload_wildcards, variant_state.leaf_types.flat,
            impl_->type_info_);
        PatternAddResult variant_result =
            impl_->AddInterval(pattern, full_interval, variant_state.leaf_types,
                               variant_state.coverage);
        if (variant_result.adds_coverage()) {
          result = variant_result;
        } else if (!result.adds_coverage() &&
                   (result.is_unmatchable() ||
                    (variant_result.overlap() != nullptr &&
                     variant_result.overlap()->kind ==
                         MatchPatternOverlapKind::kExactDuplicate))) {
          result = variant_result;
        }
      }
    } else {
      CHECK(!std::holds_alternative<TuplePattern*>(pattern))
          << "Expected a leaf pattern for sum type, got `"
          << PatternToString(pattern) << "`";
      ExpandedSumVariantPattern variant_pattern =
          ExpandSumVariantPayloadPatternLeaves(pattern,
                                               *impl_->matched_sum_type_,
                                               impl_->type_info_, file_table());
      Impl::SumVariantState& variant_state =
          impl_->sum_variant_states_.at(variant_pattern.variant_index);
      NdIntervalWithEmpty payload_interval = PatternLeavesToInterval(
          variant_pattern.leaves, variant_state.leaf_types.flat,
          impl_->type_info_);
      result =
          impl_->AddInterval(pattern, payload_interval,
                             variant_state.leaf_types, variant_state.coverage);
    }
  } else {
    NdIntervalWithEmpty this_pattern_interval =
        PatternToInterval(pattern, impl_->matched_type_,
                          impl_->leaf_types_.flat, impl_->type_info_);
    result = impl_->AddInterval(pattern, this_pattern_interval,
                                impl_->leaf_types_, impl_->coverage_);
  }
  return result;
}

std::optional<std::string>
MatchExhaustivenessChecker::FormatSimplestUncoveredValue() const {
  std::optional<std::string> result;
  if (impl_->matched_sum_type_ != nullptr) {
    for (const Impl::SumVariantState& variant_state :
         impl_->sum_variant_states_) {
      if (variant_state.coverage.remaining.IsEmpty()) {
        continue;
      }
      const SumTypeVariant& variant =
          impl_->matched_sum_type_->variants().at(GetSumVariantIndex(
              *impl_->matched_sum_type_, variant_state.variant_name));
      absl::Span<const InterpValueInterval> dimensions =
          variant_state.coverage.remaining.disjoint().front().dims();
      int64_t leaf_index = 0;
      std::vector<std::string> payload_values;
      payload_values.reserve(variant.size());
      for (int64_t i = 0; i < variant.size(); ++i) {
        payload_values.push_back(FormatSampleForType(
            variant.GetMemberType(i), dimensions, variant_state.leaf_types.flat,
            &leaf_index));
      }
      CHECK_EQ(leaf_index, dimensions.size());
      result =
          FormatSumVariant(*impl_->matched_sum_type_, variant, payload_values);
      break;
    }
  } else if (!impl_->coverage_.remaining.IsEmpty()) {
    absl::Span<const InterpValueInterval> dimensions =
        impl_->coverage_.remaining.disjoint().front().dims();
    if (impl_->matched_type_.HasEnum() ||
        TypeContainsSemanticSum(impl_->matched_type_)) {
      int64_t leaf_index = 0;
      result = FormatSampleForType(impl_->matched_type_, dimensions,
                                   impl_->leaf_types_.flat, &leaf_index);
      CHECK_EQ(leaf_index, dimensions.size());
    } else {
      result = FormatLegacySample(dimensions);
    }
  }
  return result;
}

}  // namespace xls::dslx
