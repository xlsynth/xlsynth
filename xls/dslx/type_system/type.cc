// Copyright 2020 The XLS Authors
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

#include "xls/dslx/type_system/type.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/casts.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/ir/bits_ops.h"

namespace xls::dslx {

struct TypeStringContext {
  struct SumUse {
    int64_t count = 0;
    std::optional<int64_t> reference_id;
  };

  const Type& root;
  FullyQualify root_fully_qualify;
  // Clones share their variant vector. Qualification is part of the rendering
  // key because tuple members retain their existing unqualified spelling.
  absl::flat_hash_map<
      std::pair<const std::vector<SumTypeVariant>*, FullyQualify>, SumUse>
      sums;
  int64_t next_reference_id = 1;
};

namespace {

void CountSumsForPrinting(const Type& type, FullyQualify fully_qualify,
                          TypeStringContext& context) {
  if (type.IsAggregate()) {
    if (const auto* sum = dynamic_cast<const SumType*>(&type)) {
      auto& use = context.sums[{&sum->variants(), fully_qualify}];
      ++use.count;
      if (use.count == 1) {
        for (const SumType::ParametricArgument& argument :
             sum->parametric_arguments()) {
          if (const auto* type_argument =
                  std::get_if<std::unique_ptr<const Type>>(&argument)) {
            CountSumsForPrinting(**type_argument, fully_qualify, context);
          }
        }
        for (const SumTypeVariant& variant : sum->variants()) {
          for (int64_t i = 0; i < variant.size(); ++i) {
            CountSumsForPrinting(variant.GetMemberType(i), fully_qualify,
                                 context);
          }
        }
      }
    } else if (const auto* structure =
                   dynamic_cast<const StructTypeBase*>(&type)) {
      for (const auto& member : structure->members()) {
        CountSumsForPrinting(*member, fully_qualify, context);
      }
    } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
      for (const auto& member : tuple->members()) {
        CountSumsForPrinting(*member, FullyQualify::kNo, context);
      }
    } else if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
      CountSumsForPrinting(array->element_type(), fully_qualify, context);
    } else if (const auto* function =
                   dynamic_cast<const FunctionType*>(&type)) {
      for (const auto& param : function->params()) {
        CountSumsForPrinting(*param, fully_qualify, context);
      }
      CountSumsForPrinting(function->return_type(), fully_qualify, context);
    } else if (const auto* meta = dynamic_cast<const MetaType*>(&type)) {
      CountSumsForPrinting(*meta->wrapped(), fully_qualify, context);
    } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
      CountSumsForPrinting(channel->payload_type(), fully_qualify, context);
    }
  }
}

std::vector<std::unique_ptr<Type>> ClonePayloadMembers(
    absl::Span<const std::unique_ptr<Type>> members) {
  std::vector<std::unique_ptr<Type>> cloned_members;
  cloned_members.reserve(members.size());
  for (const auto& next : members) {
    cloned_members.push_back(next->CloneToUnique());
  }
  return cloned_members;
}

void ValidateSumTypeVariantPayload(
    const SumVariant& variant,
    absl::Span<const std::unique_ptr<Type>> payload_members) {
  CHECK_EQ(payload_members.size(), variant.payload_member_count());
  for (const std::unique_ptr<Type>& member_type : payload_members) {
    CHECK(!member_type->IsMeta()) << *member_type;
  }
}

enum class FunctionResultTraversal { kInclude, kExclude };

bool ContainsSemanticSum(const Type& type,
                         FunctionResultTraversal function_result) {
  auto member_contains_sum =
      [function_result](const std::unique_ptr<Type>& member) {
        return ContainsSemanticSum(*member, function_result);
      };
  if (type.IsSum()) {
    return true;
  } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
    return ContainsSemanticSum(channel->payload_type(), function_result);
  } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    return absl::c_any_of(tuple->members(), member_contains_sum);
  } else if (const auto* structure =
                 dynamic_cast<const StructTypeBase*>(&type)) {
    return absl::c_any_of(structure->members(), member_contains_sum);
  } else if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
    return ContainsSemanticSum(array->element_type(), function_result);
  } else if (const auto* function = dynamic_cast<const FunctionType*>(&type)) {
    return absl::c_any_of(function->params(), member_contains_sum) ||
           (function_result == FunctionResultTraversal::kInclude &&
            ContainsSemanticSum(function->return_type(), function_result));
  } else {
    return false;
  }
}

enum class BitCountOperation { kAdd, kMultiply };
enum class BitCountOverflow { kWrap, kReject };

absl::StatusOr<TypeDim> ComputeBitCountOperation(const TypeDim& lhs,
                                                 const TypeDim& rhs,
                                                 BitCountOperation operation,
                                                 BitCountOverflow overflow) {
  // Keep the original diagnostics for malformed dimensions. Actual shared sum
  // widths are unsigned 32-bit dimensions; compute them again without wrapping.
  XLS_ASSIGN_OR_RETURN(TypeDim result, operation == BitCountOperation::kAdd
                                           ? lhs.Add(rhs)
                                           : lhs.Mul(rhs));
  uint64_t bit_count = 0;
  if (overflow == BitCountOverflow::kReject && lhs.value().IsUBits() &&
      lhs.value().GetBitsOrDie().bit_count() == 32 && rhs.value().IsUBits() &&
      rhs.value().GetBitsOrDie().bit_count() == 32) {
    XLS_ASSIGN_OR_RETURN(uint64_t lhs_width, lhs.value().GetBitValueUnsigned());
    XLS_ASSIGN_OR_RETURN(uint64_t rhs_width, rhs.value().GetBitValueUnsigned());
    bit_count = operation == BitCountOperation::kAdd ? lhs_width + rhs_width
                                                     : lhs_width * rhs_width;
  }
  if (bit_count > std::numeric_limits<uint32_t>::max()) {
    return absl::InvalidArgumentError(
        absl::StrCat("shared sum bit count exceeds ",
                     std::numeric_limits<uint32_t>::max(), " bits"));
  } else {
    return result;
  }
}

absl::StatusOr<TypeDim> ComputeTypeBitCount(const Type& type,
                                            BitCountOverflow overflow);

absl::StatusOr<TypeDim> ComputeMemberBitCount(
    absl::Span<const std::unique_ptr<Type>> members,
    BitCountOverflow overflow) {
  TypeDim bit_count = TypeDim::CreateU32(0);
  for (const auto& member : members) {
    XLS_ASSIGN_OR_RETURN(TypeDim member_bit_count,
                         ComputeTypeBitCount(*member, overflow));
    XLS_ASSIGN_OR_RETURN(
        bit_count, ComputeBitCountOperation(bit_count, member_bit_count,
                                            BitCountOperation::kAdd, overflow));
  }
  return bit_count;
}

absl::StatusOr<TypeDim> ComputeTypeBitCount(const Type& type,
                                            BitCountOverflow overflow) {
  if (const auto* sum = dynamic_cast<const SumType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(TypeDim payload_bit_count,
                         sum->GetMaxPayloadBitCount());
    return ComputeBitCountOperation(sum->tag_bit_count(), payload_bit_count,
                                    BitCountOperation::kAdd,
                                    BitCountOverflow::kReject);
  } else if (const auto* structure =
                 dynamic_cast<const StructTypeBase*>(&type)) {
    return ComputeMemberBitCount(structure->members(), overflow);
  } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    return ComputeMemberBitCount(tuple->members(), overflow);
  } else if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
    // The size of the instantiated bits constructor xN[is_signed][N] is N;
    // its element type xN[is_signed] has no width of its own.
    if (IsBitsConstructor(array->element_type())) {
      return array->size();
    } else {
      XLS_ASSIGN_OR_RETURN(
          TypeDim element_bit_count,
          ComputeTypeBitCount(array->element_type(), overflow));
      return ComputeBitCountOperation(element_bit_count, array->size(),
                                      BitCountOperation::kMultiply, overflow);
    }
  } else if (const auto* function = dynamic_cast<const FunctionType*>(&type)) {
    return ComputeMemberBitCount(function->params(), overflow);
  } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
    return ComputeTypeBitCount(channel->payload_type(), overflow);
  } else {
    return type.GetTotalBitCount();
  }
}

absl::StatusOr<TypeDim> GetPublicAggregateBitCount(const Type& type) {
  // Function results do not contribute to GetTotalBitCount. Select the policy
  // once so nested aggregates neither wrap within a sum nor rescan descendants.
  const BitCountOverflow overflow =
      ContainsSemanticSum(type, FunctionResultTraversal::kExclude)
          ? BitCountOverflow::kReject
          : BitCountOverflow::kWrap;
  return ComputeTypeBitCount(type, overflow);
}

absl::StatusOr<uint32_t> ComputeMaxSumPayloadBitCount(
    absl::Span<const SumTypeVariant> variants) {
  TypeDim payload_bit_count = TypeDim::CreateU32(0);
  for (const SumTypeVariant& variant : variants) {
    XLS_ASSIGN_OR_RETURN(TypeDim variant_bits,
                         internal::GetBitCountWithSharedSumPayload(variant));
    XLS_ASSIGN_OR_RETURN(InterpValue variant_is_wider,
                         variant_bits.value().Gt(payload_bit_count.value()));
    if (variant_is_wider.IsTrue()) {
      payload_bit_count = std::move(variant_bits);
    }
  }
  // Variant widths accumulate as u32 TypeDims, so the successful maximum is
  // losslessly representable without retaining a full InterpValue.
  XLS_ASSIGN_OR_RETURN(int64_t bit_count, payload_bit_count.GetAsInt64());
  return static_cast<uint32_t>(bit_count);
}

// These hashes only narrow the sum cache's semantic comparisons. In particular,
// InterpValue equality compares bit patterns across bits and enum value tags.
size_t HashSumArgumentValue(const InterpValue& value) {
  if (value.HasBits()) {
    return absl::HashOf(value.GetBitsOrDie());
  } else if (value.IsArray()) {
    const int64_t length = value.GetLength().value();
    if (length == 0) {
      return absl::HashOf(value.tag(), length);
    } else if (const auto range = value.GetRangeData(); range.has_value()) {
      // Consecutive arrays have the same identity regardless of storage. The
      // symbolic form provides its first value and length without expansion.
      return absl::HashOf(value.tag(), length, true,
                          (*range)->start.GetBitsOrDie());
    } else {
      const auto& members = value.GetValuesOrDie();
      if (members.front().HasBits()) {
        Bits expected = members.front().GetBitsOrDie();
        bool consecutive = true;
        for (const InterpValue& member : members) {
          if (!member.HasBits() || member.GetBitsOrDie() != expected) {
            consecutive = false;
            break;
          }
          expected = bits_ops::Increment(expected);
        }
        if (consecutive) {
          return absl::HashOf(value.tag(), length, true,
                              members.front().GetBitsOrDie());
        }
      }
      size_t hash = absl::HashOf(value.tag(), length, false);
      for (const InterpValue& member : members) {
        hash = absl::HashOf(hash, HashSumArgumentValue(member));
      }
      return hash;
    }
  } else if (value.IsTuple()) {
    const auto& members = value.GetValuesOrDie();
    size_t hash = absl::HashOf(value.tag(), members.size());
    for (const InterpValue& member : members) {
      hash = absl::HashOf(hash, HashSumArgumentValue(member));
    }
    return hash;
  } else {
    // Runtime-only values do not need a new identity model for this index.
    // A conservative collision leaves the existing equality check in charge.
    return absl::HashOf(value.tag());
  }
}

// Sum specializations depend on array elements, not the bounds or storage form
// used by the interpreter to represent them, including inside record tuples.
bool SameSumArgumentValue(const InterpValue& lhs, const InterpValue& rhs) {
  if (lhs.IsArray() || lhs.IsTuple()) {
    if (lhs.tag() != rhs.tag()) {
      return false;
    } else {
      const int64_t length = lhs.GetLength().value();
      const auto lhs_range = lhs.GetRangeData();
      const auto rhs_range = rhs.GetRangeData();
      if (length != rhs.GetLength().value()) {
        return false;
      } else if (lhs_range.has_value() && rhs_range.has_value()) {
        // A symbolic range advances by one from its first element. Equal
        // lengths and first elements therefore describe the same sequence,
        // independent of whether the upper bounds were inclusive.
        return length == 0 || (*lhs_range)->start == (*rhs_range)->start;
      } else if (!lhs_range.has_value() && !rhs_range.has_value()) {
        return absl::c_equal(lhs.GetValuesOrDie(), rhs.GetValuesOrDie(),
                             SameSumArgumentValue);
      } else {
        for (int64_t i = 0; i < length; ++i) {
          if (!SameSumArgumentValue(lhs.Index(i).value(),
                                    rhs.Index(i).value())) {
            return false;
          }
        }
        return true;
      }
    }
  } else {
    return lhs == rhs;
  }
}

// Hashes concrete argument structure while treating a completed child sum as
// one cached hash, rather than visiting both its arguments and its payloads.
size_t HashSumArgumentType(const Type& type);

bool SameSumArgumentType(const Type& lhs, const Type& rhs);

bool SameSumArguments(absl::Span<const NominalParametricArgument> lhs,
                      absl::Span<const NominalParametricArgument> rhs) {
  return absl::c_equal(
      lhs, rhs,
      [](const NominalParametricArgument& a,
         const NominalParametricArgument& b) {
        if (a.index() != b.index()) {
          return false;
        } else if (const auto* value = std::get_if<InterpValue>(&a)) {
          return SameSumArgumentValue(*value, std::get<InterpValue>(b));
        } else {
          return SameSumArgumentType(*std::get<std::unique_ptr<const Type>>(a),
                                     *std::get<std::unique_ptr<const Type>>(b));
        }
      });
}

bool SameSumArgumentTypes(absl::Span<const std::unique_ptr<Type>> lhs,
                          absl::Span<const std::unique_ptr<Type>> rhs) {
  return absl::c_equal(lhs, rhs, [](const auto& a, const auto& b) {
    return SameSumArgumentType(*a, *b);
  });
}

// Clones share immutable argument data. Retain successful pairs while comparing
// distinct descriptions so repeated shared record and proc graphs are visited
// once. Physical record members also participate; proc members do not.
bool SameKnownNominalSumArguments(const StructTypeBase& lhs,
                                  const StructTypeBase& rhs,
                                  bool compare_members) {
  using Arguments = std::vector<SpecializationArgument>;
  using EqualPairs =
      absl::flat_hash_set<std::pair<const Arguments*, const Arguments*>>;
  static thread_local EqualPairs* current_equal_pairs = nullptr;
  const Arguments& a = *lhs.specialization_arguments();
  const Arguments& b = *rhs.specialization_arguments();
  if (&a == &b) {
    return true;
  } else {
    auto compare_contents = [&] {
      const auto& lhs_full = lhs.resolved_parametric_arguments();
      const auto& rhs_full = rhs.resolved_parametric_arguments();
      const bool equal_arguments =
          lhs_full.has_value() && rhs_full.has_value()
              ? SameSumArguments(*lhs_full, *rhs_full)
              : absl::c_equal(a, b, [](const auto& x, const auto& y) {
                  if (x.index() != y.index()) {
                    return false;
                  } else if (const auto* value = std::get_if<InterpValue>(&x)) {
                    return SameSumArgumentValue(*value,
                                                std::get<InterpValue>(y));
                  } else {
                    return std::get<SpecializationTypePtr>(x)->SemanticEquals(
                        *std::get<SpecializationTypePtr>(y));
                  }
                });
      return equal_arguments &&
             (!compare_members ||
              SameSumArgumentTypes(lhs.members(), rhs.members()));
    };
    if (current_equal_pairs == nullptr) {
      EqualPairs equal_pairs;
      current_equal_pairs = &equal_pairs;
      absl::Cleanup reset_context([&] { current_equal_pairs = nullptr; });
      return compare_contents();
    } else {
      const auto pair = std::make_pair(&a, &b);
      if (current_equal_pairs->contains(pair)) {
        return true;
      } else if (compare_contents()) {
        current_equal_pairs->insert(pair);
        return true;
      } else {
        return false;
      }
    }
  }
}

// StructType's general equality intentionally only considers members and the
// nominal declaration. Sum arguments additionally need bindings that do not
// appear in its members, and may use opaque proc declarations as arguments.
bool SameSumArgumentType(const Type& lhs, const Type& rhs) {
  if (const auto* a = dynamic_cast<const ProcType*>(&lhs)) {
    const auto* b = dynamic_cast<const ProcType*>(&rhs);
    if (b == nullptr || &a->nominal_type() != &b->nominal_type()) {
      return false;
    } else {
      const auto* a_arguments = a->specialization_arguments();
      const auto* b_arguments = b->specialization_arguments();
      if (a_arguments == nullptr || b_arguments == nullptr) {
        return !a->nominal_type().IsParametric() || a_arguments == b_arguments;
      } else {
        return SameKnownNominalSumArguments(*a, *b, /*compare_members=*/false);
      }
    }
  } else if (const auto* a = dynamic_cast<const StructType*>(&lhs)) {
    const auto* b = dynamic_cast<const StructType*>(&rhs);
    if (b == nullptr || &a->nominal_type() != &b->nominal_type()) {
      return false;
    } else {
      const auto* a_arguments = a->specialization_arguments();
      const auto* b_arguments = b->specialization_arguments();
      if (a_arguments == nullptr || b_arguments == nullptr) {
        return (!a->nominal_type().IsParametric() ||
                a_arguments == b_arguments) &&
               SameSumArgumentTypes(a->members(), b->members());
      } else {
        return SameKnownNominalSumArguments(*a, *b, /*compare_members=*/true);
      }
    }
  } else if (const auto* a = dynamic_cast<const TupleType*>(&lhs)) {
    const auto* b = dynamic_cast<const TupleType*>(&rhs);
    return b != nullptr && SameSumArgumentTypes(a->members(), b->members());
  } else if (const auto* a = dynamic_cast<const ArrayType*>(&lhs);
             a != nullptr && !GetBitsLike(lhs).has_value()) {
    const auto* b = dynamic_cast<const ArrayType*>(&rhs);
    return b != nullptr && a->size() == b->size() &&
           SameSumArgumentType(a->element_type(), b->element_type());
  } else if (const auto* a = dynamic_cast<const FunctionType*>(&lhs)) {
    const auto* b = dynamic_cast<const FunctionType*>(&rhs);
    return b != nullptr && SameSumArgumentTypes(a->params(), b->params()) &&
           SameSumArgumentType(a->return_type(), b->return_type());
  } else if (const auto* a = dynamic_cast<const ChannelType*>(&lhs)) {
    const auto* b = dynamic_cast<const ChannelType*>(&rhs);
    return b != nullptr && a->direction() == b->direction() &&
           SameSumArgumentType(a->payload_type(), b->payload_type());
  } else {
    // Nested sums perform their own precise argument comparison, and bits-like
    // types keep the general equality's normalization of xN array notation.
    return lhs == rhs;
  }
}

// Preserves member order and arity in aggregate type arguments.
size_t HashSumArgumentTypes(absl::Span<const std::unique_ptr<Type>> types) {
  size_t hash = absl::HashOf(types.size());
  for (const auto& type : types) {
    hash = absl::HashOf(hash, HashSumArgumentType(*type));
  }
  return hash;
}

size_t HashSumArgumentType(const Type& type) {
  if (std::optional<BitsLikeProperties> bits = GetBitsLike(type)) {
    // Use the existing bits-like normalization, including xN array notation.
    return absl::HashOf(HashSumArgumentValue(bits->is_signed.value()),
                        HashSumArgumentValue(bits->size.value()));
  } else if (const auto* sum = dynamic_cast<const SumType*>(&type)) {
    return absl::HashOf(&sum->nominal_type(), sum->parametric_arguments_hash());
  } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    return HashSumArgumentTypes(tuple->members());
  } else if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
    return absl::HashOf(HashSumArgumentValue(array->size().value()),
                        HashSumArgumentType(array->element_type()));
  } else if (const auto* structure =
                 dynamic_cast<const StructTypeBase*>(&type)) {
    // Ordinary struct equality ignores nominal dimensions and unused arguments.
    return absl::HashOf(&structure->struct_def_base(),
                        HashSumArgumentTypes(structure->members()));
  } else if (const auto* enumeration = dynamic_cast<const EnumType*>(&type)) {
    return absl::HashOf(&enumeration->nominal_type(), enumeration->is_signed(),
                        HashSumArgumentValue(enumeration->size().value()));
  } else if (const auto* function = dynamic_cast<const FunctionType*>(&type)) {
    return absl::HashOf(HashSumArgumentTypes(function->params()),
                        HashSumArgumentType(function->return_type()));
  } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
    return absl::HashOf(channel->direction(),
                        HashSumArgumentType(channel->payload_type()));
  } else {
    // Token and non-concrete type forms may collide. This is intentionally not
    // a general Type hash: resolved arguments and semantic equality govern
    // reuse.
    return absl::HashOf(type.GetDebugTypeName());
  }
}

}  // namespace

size_t HashTypeForSumCache(const Type& type) {
  return HashSumArgumentType(type);
}

size_t HashTypeForSumSpecialization(const Type& type) {
  return SpecializationType::FromType(type)->hash();
}

namespace {

bool SameSpecializationArguments(absl::Span<const SpecializationArgument> lhs,
                                 absl::Span<const SpecializationArgument> rhs) {
  return absl::c_equal(lhs, rhs, [](const auto& a, const auto& b) {
    if (a.index() != b.index()) {
      return false;
    } else if (const auto* value = std::get_if<InterpValue>(&a)) {
      return SameSumArgumentValue(*value, std::get<InterpValue>(b));
    } else {
      const auto& x = std::get<SpecializationTypePtr>(a);
      const auto& y = std::get<SpecializationTypePtr>(b);
      return x == y || x->SemanticEquals(*y);
    }
  });
}

std::vector<SpecializationArgument> CompactArguments(
    absl::Span<const NominalParametricArgument> arguments) {
  std::vector<SpecializationArgument> result;
  result.reserve(arguments.size());
  for (const auto& argument : arguments) {
    if (const auto* value = std::get_if<InterpValue>(&argument)) {
      result.push_back(*value);
    } else {
      result.push_back(SpecializationType::FromType(
          *std::get<std::unique_ptr<const Type>>(argument)));
    }
  }
  return result;
}

size_t HashSpecializationDim(const std::optional<TypeDim>& dim) {
  return dim.has_value()
             ? absl::HashOf(true, HashSumArgumentValue(dim->value()))
             : absl::HashOf(false);
}

bool SameSpecializationDim(const std::optional<TypeDim>& lhs,
                           const std::optional<TypeDim>& rhs) {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs.has_value() || SameSumArgumentValue(lhs->value(), rhs->value()));
}

size_t HashSpecializationDescriptionArguments(
    const SpecializationType::Description& description) {
  using Kind = SpecializationType::Kind;
  const auto* record = dynamic_cast<const StructDefBase*>(description.nominal);
  if ((description.kind == Kind::kStruct || description.kind == Kind::kProc) &&
      record != nullptr && !record->IsParametric()) {
    // Older manually created nonparametric records may supply extraneous
    // arguments. When compared with an argument-less record their historical
    // equality ignores these, so the process-local hash must permit a match.
    return SumType::HashSpecializationArguments({});
  } else {
    return SumType::HashSpecializationArguments(description.arguments);
  }
}

}  // namespace

SpecializationType::SpecializationType(Description description)
    : description_(std::move(description)),
      hash_(absl::HashOf(description_.kind, description_.nominal,
                         HashSpecializationDim(description_.size),
                         HashSpecializationDim(description_.signedness),
                         description_.direction,
                         description_.nominal_arguments_known,
                         HashSpecializationDescriptionArguments(description_),
                         description_.children.size())) {
  for (const auto& child : description_.children) {
    CHECK(child != nullptr);
    // Explicit full arguments historically use ordinary MetaType equality,
    // which can equate different nominal specializations of its child.
    if (description_.kind != Kind::kMeta) {
      hash_ = absl::HashOf(hash_, child->hash());
    }
  }
}

/* static */ SpecializationTypePtr SpecializationType::Create(
    Description description) {
  return absl::WrapUnique(new SpecializationType(std::move(description)));
}

bool SpecializationType::SemanticEquals(const SpecializationType& other) const {
  using Pair = std::pair<const SpecializationType*, const SpecializationType*>;
  absl::flat_hash_set<Pair> equal_pairs;
  auto compare = [&](auto&& self, const SpecializationType& lhs,
                     const SpecializationType& rhs) -> bool {
    if (&lhs == &rhs) {
      return true;
    } else if (lhs.hash_ != rhs.hash_) {
      return false;
    } else if (equal_pairs.contains({&lhs, &rhs})) {
      return true;
    } else {
      const auto& a = lhs.description_;
      const auto& b = rhs.description_;
      if (a.kind != b.kind || a.nominal != b.nominal ||
          !SameSpecializationDim(a.size, b.size) ||
          !SameSpecializationDim(a.signedness, b.signedness) ||
          a.direction != b.direction ||
          a.nominal_arguments_known != b.nominal_arguments_known ||
          a.arguments.size() != b.arguments.size() ||
          a.children.size() != b.children.size()) {
        return false;
      } else {
        for (int64_t i = 0; i < a.arguments.size(); ++i) {
          const auto& x = a.arguments[i];
          const auto& y = b.arguments[i];
          if (x.index() != y.index()) {
            return false;
          } else if (const auto* value = std::get_if<InterpValue>(&x)) {
            if (!SameSumArgumentValue(*value, std::get<InterpValue>(y))) {
              return false;
            }
          } else if (!self(self, *std::get<SpecializationTypePtr>(x),
                           *std::get<SpecializationTypePtr>(y))) {
            return false;
          }
        }
        for (int64_t i = 0; i < a.children.size(); ++i) {
          if (!self(self, *a.children[i], *b.children[i])) {
            return false;
          }
        }
        equal_pairs.insert({&lhs, &rhs});
        return true;
      }
    }
  };
  return compare(compare, *this, other);
}

/* static */ SpecializationTypePtr SpecializationType::FromType(
    const Type& type) {
  Description result{.kind = Kind::kToken};
  auto children = [&](absl::Span<const std::unique_ptr<Type>> types) {
    for (const auto& child : types) {
      result.children.push_back(FromType(*child));
    }
  };
  if (std::optional<BitsLikeProperties> bits = GetBitsLike(type)) {
    result.kind = Kind::kBits;
    result.size = bits->size.Clone();
    result.signedness = bits->is_signed.Clone();
  } else if (const auto* sum = dynamic_cast<const SumType*>(&type)) {
    return sum->specialization_type();
  } else if (const auto* record = dynamic_cast<const StructTypeBase*>(&type)) {
    return record->specialization_type();
  } else if (const auto* enumeration = dynamic_cast<const EnumType*>(&type)) {
    result.kind = Kind::kEnum;
    result.nominal = &enumeration->nominal_type();
    result.size = enumeration->size().Clone();
    result.signedness = TypeDim::CreateBool(enumeration->is_signed());
  } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    result.kind = Kind::kTuple;
    children(tuple->members());
  } else if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
    result.kind = Kind::kArray;
    result.size = array->size().Clone();
    result.children.push_back(FromType(array->element_type()));
  } else if (const auto* function = dynamic_cast<const FunctionType*>(&type)) {
    result.kind = Kind::kFunction;
    children(function->params());
    result.children.push_back(FromType(function->return_type()));
  } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
    result.kind = Kind::kChannel;
    result.direction = channel->direction();
    result.children.push_back(FromType(channel->payload_type()));
  } else if (const auto* meta = dynamic_cast<const MetaType*>(&type)) {
    result.kind = Kind::kMeta;
    result.children.push_back(FromType(*meta->wrapped()));
  } else if (const auto* constructor =
                 dynamic_cast<const BitsConstructorType*>(&type)) {
    result.kind = Kind::kBitsConstructor;
    result.signedness = constructor->is_signed().Clone();
  } else {
    CHECK(type.IsToken()) << "not a specialization type: " << type;
  }
  return Create(std::move(result));
}

/* static */ SpecializationTypeShapePtr SpecializationTypeShape::Create(
    Description description) {
  CHECK(description.identity != nullptr);
  return absl::WrapUnique(new SpecializationTypeShape(std::move(description)));
}

/* static */ SpecializationTypeShapePtr SpecializationTypeShape::FromType(
    const Type& type) {
  Description result{.identity = SpecializationType::FromType(type)};
  auto children = [&](absl::Span<const std::unique_ptr<Type>> types) {
    for (const auto& child : types) {
      result.children.push_back(FromType(*child));
    }
  };
  if (const auto* sum = dynamic_cast<const SumType*>(&type)) {
    result.tag_bit_count = sum->tag_bit_count();
    result.argument_shapes = sum->specialization_argument_shapes();
    for (int64_t i = 0; i < sum->variant_count(); ++i) {
      result.discriminants.push_back(sum->GetDiscriminant(i));
      const SumTypeVariant& variant = sum->variants()[i];
      for (int64_t j = 0; j < variant.size(); ++j) {
        result.children.push_back(FromType(variant.GetMemberType(j)));
      }
    }
  } else if (const auto* record = dynamic_cast<const StructTypeBase*>(&type)) {
    children(record->members());
  } else if (const auto* enumeration = dynamic_cast<const EnumType*>(&type)) {
    result.enum_members = enumeration->members();
  } else if (const auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    children(tuple->members());
  } else if (const auto* array = dynamic_cast<const ArrayType*>(&type);
             array != nullptr && !GetBitsLike(type).has_value()) {
    result.children.push_back(FromType(array->element_type()));
  } else if (const auto* function = dynamic_cast<const FunctionType*>(&type)) {
    children(function->params());
    result.children.push_back(FromType(function->return_type()));
  } else if (const auto* channel = dynamic_cast<const ChannelType*>(&type)) {
    result.children.push_back(FromType(channel->payload_type()));
  } else if (const auto* meta = dynamic_cast<const MetaType*>(&type)) {
    result.children.push_back(FromType(*meta->wrapped()));
  }
  return Create(std::move(result));
}

std::unique_ptr<SumType> SpecializationTypeShape::ReuseCompletedSum(
    std::unique_ptr<SumType> candidate) const {
  CHECK(description_.identity->description().kind ==
        SpecializationType::Kind::kSum);
  CHECK(candidate != nullptr);
  DCHECK_EQ(description_.identity->description().nominal,
            &candidate->nominal_type());
  std::lock_guard<std::mutex> lock(completed_sum_mutex_);
  if (completed_sum_ == nullptr) {
    completed_sum_ = std::move(candidate);
  }
  return CloneToUniqueInternal(*completed_sum_);
}

std::unique_ptr<Type> SpecializationTypeShape::Materialize() const {
  using Kind = SpecializationType::Kind;
  const auto& identity = description_.identity->description();
  auto children = [&] {
    std::vector<std::unique_ptr<Type>> result;
    result.reserve(description_.children.size());
    for (const auto& child : description_.children) {
      result.push_back(child->Materialize());
    }
    return result;
  };
  switch (identity.kind) {
    case Kind::kBits: {
      absl::StatusOr<bool> is_signed = identity.signedness->GetAsBool();
      if (is_signed.ok()) {
        return std::make_unique<BitsType>(*is_signed, *identity.size);
      } else {
        return std::make_unique<ArrayType>(
            std::make_unique<BitsConstructorType>(*identity.signedness),
            *identity.size);
      }
    }
    case Kind::kToken:
      return std::make_unique<TokenType>();
    case Kind::kEnum: {
      const auto* def = dynamic_cast<const EnumDef*>(identity.nominal);
      CHECK(def != nullptr);
      return std::make_unique<EnumType>(
          *def, *identity.size, identity.signedness->GetAsBool().value(),
          description_.enum_members);
    }
    case Kind::kStruct: {
      const auto* def = dynamic_cast<const StructDef*>(identity.nominal);
      CHECK(def != nullptr);
      if (identity.nominal_arguments_known) {
        return StructType::CreateWithSpecializationArguments(
            children(), *def, identity.arguments);
      } else {
        return std::make_unique<StructType>(children(), *def);
      }
    }
    case Kind::kProc: {
      const auto* def = dynamic_cast<const ProcDef*>(identity.nominal);
      CHECK(def != nullptr);
      if (identity.nominal_arguments_known) {
        return ProcType::CreateWithSpecializationArguments(children(), *def,
                                                           identity.arguments);
      } else {
        return std::make_unique<ProcType>(children(), *def);
      }
    }
    case Kind::kSum: {
      {
        std::lock_guard<std::mutex> lock(completed_sum_mutex_);
        if (completed_sum_ != nullptr) {
          return completed_sum_->CloneToUnique();
        }
      }
      // Materialize child shapes without holding the lock; the first completed
      // candidate still wins if another caller finishes in the meantime.
      const auto* def = dynamic_cast<const SumDef*>(identity.nominal);
      CHECK(def != nullptr);
      std::vector<SumTypeVariant> variants;
      int64_t child_index = 0;
      for (const SumVariant* variant : def->variants()) {
        std::vector<std::unique_ptr<Type>> members;
        for (int64_t i = 0; i < variant->payload_member_count(); ++i) {
          members.push_back(
              description_.children.at(child_index++)->Materialize());
        }
        if (variant->is_unit()) {
          variants.push_back(SumTypeVariant::MakeUnit(*variant));
        } else if (variant->is_tuple()) {
          variants.push_back(
              SumTypeVariant::MakeTuple(*variant, std::move(members)));
        } else {
          variants.push_back(
              SumTypeVariant::MakeStruct(*variant, std::move(members)));
        }
      }
      CHECK_EQ(child_index, description_.children.size());
      return ReuseCompletedSum(SumType::CreateWithSpecializationArguments(
          *def, std::move(variants), description_.tag_bit_count,
          description_.discriminants, identity.arguments,
          description_.argument_shapes));
    }
    case Kind::kTuple:
      return std::make_unique<TupleType>(children());
    case Kind::kArray:
      return std::make_unique<ArrayType>(
          description_.children.at(0)->Materialize(), *identity.size);
    case Kind::kFunction: {
      auto params = children();
      CHECK(!params.empty());
      std::unique_ptr<Type> result = std::move(params.back());
      params.pop_back();
      return std::make_unique<FunctionType>(std::move(params),
                                            std::move(result));
    }
    case Kind::kChannel:
      return std::make_unique<ChannelType>(
          description_.children.at(0)->Materialize(), *identity.direction);
    case Kind::kMeta:
      return std::make_unique<MetaType>(
          description_.children.at(0)->Materialize());
    case Kind::kBitsConstructor:
      return std::make_unique<BitsConstructorType>(*identity.signedness);
  }
  LOG(FATAL) << "invalid specialization kind";
}

SpecializationArguments::SpecializationArguments(
    std::vector<NominalParametricArgument> full_arguments)
    : arguments_(CompactArguments(full_arguments)),
      has_full_input_(true),
      full_arguments_(std::move(full_arguments)) {}

SpecializationArguments::SpecializationArguments(
    std::vector<SpecializationArgument> arguments,
    std::vector<SpecializationTypeShapePtr> argument_shapes)
    : arguments_(std::move(arguments)),
      has_full_input_(false),
      argument_shapes_(std::move(argument_shapes)) {
  if (argument_shapes_.empty()) {
    argument_shapes_.resize(arguments_.size());
  }
  CHECK_EQ(argument_shapes_.size(), arguments_.size());
}

const std::vector<SpecializationTypeShapePtr>&
SpecializationArguments::argument_shapes() const {
  std::call_once(shapes_once_, [&] {
    if (has_full_input_) {
      argument_shapes_.reserve(arguments_.size());
      for (const auto& argument : *full_arguments_) {
        if (const auto* type =
                std::get_if<std::unique_ptr<const Type>>(&argument)) {
          argument_shapes_.push_back(SpecializationTypeShape::FromType(**type));
        } else {
          argument_shapes_.push_back(nullptr);
        }
      }
    }
  });
  return argument_shapes_;
}

const std::vector<NominalParametricArgument>&
SpecializationArguments::full_arguments() const {
  std::call_once(full_once_, [&] {
    if (!has_full_input_) {
      full_arguments_.emplace();
      full_arguments_->reserve(arguments_.size());
      for (int64_t i = 0; i < arguments_.size(); ++i) {
        const auto& argument = arguments_[i];
        if (const auto* value = std::get_if<InterpValue>(&argument)) {
          full_arguments_->emplace_back(*value);
        } else {
          CHECK(argument_shapes_[i] != nullptr)
              << "full specialization argument shape is unavailable";
          full_arguments_->emplace_back(argument_shapes_[i]->Materialize());
        }
      }
    }
  });
  return *full_arguments_;
}

const std::optional<std::vector<NominalParametricArgument>>&
SpecializationArguments::full_arguments_optional() const {
  (void)full_arguments();
  return full_arguments_;
}

Type::~Type() = default;

std::string Type::ToStringInternal(FullyQualify fully_qualify,
                                   const FileTable* file_table) const {
  TypeStringContext context{.root = *this, .root_fully_qualify = fully_qualify};
  std::string output;
  AppendToStringInternal(fully_qualify, file_table, context, output);
  return output;
}

/* static */ bool Type::Equal(absl::Span<const std::unique_ptr<Type>> a,
                              absl::Span<const std::unique_ptr<Type>> b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (int64_t i = 0; i < a.size(); ++i) {
    if (*a[i] != *b[i]) {
      return false;
    }
  }
  return true;
}

/* static */ std::vector<std::unique_ptr<Type>> Type::CloneSpan(
    absl::Span<const std::unique_ptr<Type>> ts) {
  std::vector<std::unique_ptr<Type>> result;
  result.reserve(ts.size());
  for (const auto& t : ts) {
    CHECK(t != nullptr);
    VLOG(10) << "CloneSpan; cloning: "
             << t->ToStringInternal(FullyQualify::kNo, nullptr);
    result.push_back(t->CloneToUnique());
  }
  return result;
}

std::unique_ptr<Type> Type::MakeUnit() {
  return std::make_unique<TupleType>(std::vector<std::unique_ptr<Type>>{});
}

absl::StatusOr<std::unique_ptr<Type>> Type::FromInterpValue(
    const InterpValue& value) {
  if (value.tag() == InterpValueTag::kUBits ||
      value.tag() == InterpValueTag::kSBits) {
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, value.GetBitCount());
    return std::make_unique<BitsType>(/*is_signed*/ value.IsSigned(),
                                      /*size=*/bit_count);
  }

  if (value.tag() == InterpValueTag::kArray) {
    XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                         value.GetValues());
    if (elements->empty()) {
      return absl::InvalidArgumentError(
          "Cannot get the Type of a 0-element array.");
    }
    XLS_ASSIGN_OR_RETURN(auto element_type, FromInterpValue(elements->at(0)));
    XLS_ASSIGN_OR_RETURN(int64_t size, value.GetLength());
    XLS_RET_CHECK_EQ(static_cast<uint32_t>(size), size);
    auto dim = TypeDim::CreateU32(static_cast<uint32_t>(size));
    return std::make_unique<ArrayType>(std::move(element_type), dim);
  }

  if (value.tag() == InterpValueTag::kTuple) {
    XLS_ASSIGN_OR_RETURN(const std::vector<InterpValue>* elements,
                         value.GetValues());
    std::vector<std::unique_ptr<Type>> members;
    members.reserve(elements->size());
    for (const auto& element : *elements) {
      XLS_ASSIGN_OR_RETURN(auto member, FromInterpValue(element));
      members.push_back(std::move(member));
    }

    return std::make_unique<TupleType>(std::move(members));
  }

  return absl::InvalidArgumentError(
      "Only bits, array, and tuple types can be converted into concrete.");
}

// -- class TypeDim

/* static */ absl::StatusOr<int64_t> TypeDim::GetAs64Bits(
    const InterpValue& value) {
  return value.GetBitValueViaSign();
}

TypeDim::TypeDim(const TypeDim& other)
    : value_(std::move(other.Clone().value_)) {}

TypeDim::TypeDim(InterpValue value) : value_(std::move(value)) {}

TypeDim TypeDim::Clone() const { return TypeDim(value_); }

std::string TypeDim::ToString() const {
  // Note: we don't print out the type/width of the InterpValue that serves as
  // the dimension, because printing `uN[u32:42]` would appear odd vs just
  // `uN[42]`.
  //
  // TODO(https://github.com/google/xls/issues/450) the best solution may to be
  // to have a size type that all InterpValues present on real type dimensions
  // must be. Things are trickier nowadays because we want to permit arbitrary
  // InterpValues to be passed as parametrics, not just ones that become (used)
  // dimension data -- we need to allow for e.g. signed types which may not end
  // up in any particular dimension position.
  return BitsToString(value_.GetBitsOrDie());
}

ModuleType::~ModuleType() = default;

absl::Status ModuleType::Accept(TypeVisitor& v) const {
  return v.HandleModule(*this);
}

void ModuleType::AppendToStringInternal(FullyQualify fully_qualify,
                                        const FileTable*, TypeStringContext&,
                                        std::string& output) const {
  absl::StrAppendFormat(&output, "typeof(module:%s)", module_.name());
}

std::string TypeDim::ToDebugString() const {
  return absl::StrFormat("InterpValue{%s}", value_.ToString());
}

bool TypeDim::operator==(const InterpValue& other) const {
  VLOG(10) << "TypeDim::operator==; this: " << ToDebugString()
           << " other: " << other.ToString();
  return value_ == other;
}

bool TypeDim::operator==(const TypeDim& other) const {
  return value_ == other.value_;
}

absl::StatusOr<TypeDim> TypeDim::Mul(const TypeDim& rhs) const {
  XLS_ASSIGN_OR_RETURN(InterpValue result, value_.Mul(rhs.value_));
  return TypeDim(std::move(result));
}

absl::StatusOr<TypeDim> TypeDim::Add(const TypeDim& rhs) const {
  XLS_ASSIGN_OR_RETURN(InterpValue result, value_.Add(rhs.value_));
  return TypeDim(std::move(result));
}

absl::StatusOr<TypeDim> TypeDim::CeilOfLog2() const {
  XLS_ASSIGN_OR_RETURN(InterpValue result, value_.CeilOfLog2());
  return TypeDim(std::move(result));
}

absl::StatusOr<int64_t> TypeDim::GetAsInt64() const {
  if (!value_.IsBits()) {
    return absl::InvalidArgumentError(
        "Cannot convert non-bits type to int64_t.");
  }

  if (value_.IsSigned()) {
    return value_.GetBitValueSigned();
  }
  return value_.GetBitValueUnsigned();
}

absl::StatusOr<bool> TypeDim::GetAsBool() const {
  if (!value_.IsBits()) {
    return absl::InvalidArgumentError("Cannot convert non-bits type to bool.");
  }

  XLS_RET_CHECK(!value_.IsSigned());
  return value_.GetBitValueUnsigned();
}

// -- Type

bool Type::CompatibleWith(const Type& other) const {
  if (*this == other) {
    return true;  // Equality implies compatibility.
  }

  // For types that encapsulate other types, they may not be strictly equal, but
  // the contained types may still be compatible.
  if (auto [t, u] = std::make_pair(dynamic_cast<const TupleType*>(this),
                                   dynamic_cast<const TupleType*>(&other));
      t != nullptr && u != nullptr) {
    return t->CompatibleWith(*u);
  }

  if (auto [t, u] = std::make_pair(dynamic_cast<const ArrayType*>(this),
                                   dynamic_cast<const ArrayType*>(&other));
      t != nullptr && u != nullptr) {
    return t->element_type().CompatibleWith(u->element_type()) &&
           t->size() == u->size();
  }

  return false;
}

bool Type::IsChannel() const {
  return dynamic_cast<const ChannelType*>(this) != nullptr;
}

bool Type::IsUnit() const {
  if (auto* t = dynamic_cast<const TupleType*>(this)) {
    return t->empty();
  }
  return false;
}

bool Type::IsStruct() const {
  return dynamic_cast<const StructType*>(this) != nullptr;
}

bool Type::IsSum() const {
  return dynamic_cast<const SumType*>(this) != nullptr;
}

bool Type::IsProc() const {
  return dynamic_cast<const ProcType*>(this) != nullptr;
}

bool Type::IsEnum() const {
  return dynamic_cast<const EnumType*>(this) != nullptr;
}

bool Type::IsArray() const {
  return dynamic_cast<const ArrayType*>(this) != nullptr;
}

bool Type::IsMeta() const {
  return dynamic_cast<const MetaType*>(this) != nullptr;
}

bool Type::IsToken() const {
  return dynamic_cast<const TokenType*>(this) != nullptr;
}

bool Type::IsTuple() const {
  return dynamic_cast<const TupleType*>(this) != nullptr;
}

bool Type::IsFunction() const {
  return dynamic_cast<const FunctionType*>(this) != nullptr;
}

bool Type::IsModule() const {
  return dynamic_cast<const ModuleType*>(this) != nullptr;
}

const ChannelType& Type::AsChannel() const {
  auto* c = dynamic_cast<const ChannelType*>(this);
  CHECK(c != nullptr) << "Type is not a channel: " << *this;
  return *c;
}

const EnumType& Type::AsEnum() const {
  auto* s = dynamic_cast<const EnumType*>(this);
  CHECK(s != nullptr) << "Type is not an enum: " << *this;
  return *s;
}

const StructType& Type::AsStruct() const {
  auto* s = dynamic_cast<const StructType*>(this);
  CHECK(s != nullptr) << "Type is not a struct: " << *this;
  return *s;
}

const SumType& Type::AsSum() const {
  auto* s = dynamic_cast<const SumType*>(this);
  CHECK(s != nullptr) << "Type is not a sum: " << *this;
  return *s;
}

const ProcType& Type::AsProc() const {
  auto* s = dynamic_cast<const ProcType*>(this);
  CHECK(s != nullptr) << "Type is not a proc: " << *this;
  return *s;
}

const MetaType& Type::AsMeta() const {
  auto* s = dynamic_cast<const MetaType*>(this);
  CHECK(s != nullptr) << "Type is not a MetaType: " << *this;
  return *s;
}

const ArrayType& Type::AsArray() const {
  auto* s = dynamic_cast<const ArrayType*>(this);
  CHECK(s != nullptr) << "Type is not an array: " << *this;
  return *s;
}

const FunctionType& Type::AsFunction() const {
  auto* s = dynamic_cast<const FunctionType*>(this);
  CHECK(s != nullptr) << "Type is not a function: " << *this;
  return *s;
}

const TupleType& Type::AsTuple() const {
  auto* s = dynamic_cast<const TupleType*>(this);
  CHECK(s != nullptr) << "Type is not a tuple: " << *this;
  return *s;
}

// -- TokenType

TokenType::~TokenType() = default;

// -- MetaType

MetaType::~MetaType() = default;

// -- BitsConstructorType

BitsConstructorType::BitsConstructorType(TypeDim is_signed)
    : is_signed_(std::move(is_signed)) {
  VLOG(10) << "BitsConstructorType constructor; is_signed: "
           << is_signed_.ToDebugString();

  // Check that the InterpValue is a boolean.
  const InterpValue& value = is_signed_.value();
  CHECK(value.IsBool())
      << "BitsConstructorType is_signed must be a boolean; got: "
      << value.ToString();
}

BitsConstructorType::~BitsConstructorType() = default;

absl::Status BitsConstructorType::Accept(TypeVisitor& v) const {
  return v.HandleBitsConstructor(*this);
}

bool BitsConstructorType::operator==(const Type& other) const {
  VLOG(10) << "BitsConstructorType::operator==; this: " << *this
           << " other: " << other;
  if (auto* t = dynamic_cast<const BitsConstructorType*>(&other)) {
    bool result = t->is_signed_ == is_signed_;
    VLOG(10) << "BitsConstructorType::operator==; result: " << result;
    return result;
  }
  if (auto* b = dynamic_cast<const BitsType*>(&other)) {
    return TypeDim::CreateBool(b->is_signed()) == is_signed_ &&
           b->size() == TypeDim::CreateU32(0);
  }
  return false;
}

void BitsConstructorType::AppendToStringInternal(FullyQualify fully_qualify,
                                                 const FileTable*,
                                                 TypeStringContext&,
                                                 std::string& output) const {
  absl::StrAppendFormat(&output, "xN[is_signed=%s]", is_signed_.ToString());
}

std::string BitsConstructorType::GetDebugTypeName() const {
  return "bits-constructor";
}

bool BitsConstructorType::HasEnum() const { return false; }
bool BitsConstructorType::HasToken() const { return false; }

std::vector<TypeDim> BitsConstructorType::GetAllDims() const {
  std::vector<TypeDim> result;
  result.push_back(is_signed_.Clone());
  return result;
}

absl::StatusOr<TypeDim> BitsConstructorType::GetTotalBitCount() const {
  return TypeDim::CreateU32(0);
}

std::unique_ptr<Type> BitsConstructorType::CloneToUnique() const {
  return std::make_unique<BitsConstructorType>(is_signed_.Clone());
}

// -- BitsType

BitsType::BitsType(bool is_signed, int64_t size)
    : BitsType(is_signed,
               TypeDim(InterpValue::MakeU32(static_cast<uint32_t>(size)))) {
  CHECK_EQ(size, static_cast<uint32_t>(size));
}

BitsType::BitsType(bool is_signed, TypeDim size)
    : is_signed_(is_signed), size_(std::move(size)) {}

bool BitsType::operator==(const Type& other) const {
  VLOG(10) << "BitsType::operator==; this: " << *this << " other: " << other;
  if (auto* t = dynamic_cast<const BitsType*>(&other)) {
    return t->is_signed_ == is_signed_ && t->size_ == size_;
  }
  if (IsArrayOfBitsConstructor(other)) {
    const auto* a = absl::down_cast<const ArrayType*>(&other);
    const auto* bc =
        absl::down_cast<const BitsConstructorType*>(&a->element_type());
    return a->size() == size_ &&
           bc->is_signed() == TypeDim::CreateBool(is_signed());
  }
  return false;
}

void BitsType::AppendToStringInternal(FullyQualify fully_qualify,
                                      const FileTable*, TypeStringContext&,
                                      std::string& output) const {
  absl::StrAppendFormat(&output, "%cN[%s]", is_signed_ ? 's' : 'u',
                        size_.ToString());
}

std::string BitsType::GetDebugTypeName() const {
  return is_signed_ ? "sbits" : "ubits";
}

std::unique_ptr<BitsType> BitsType::ToUBits() const {
  return std::make_unique<BitsType>(false, size_.Clone());
}

// -- StructTypeBase

StructTypeBase::StructTypeBase(
    std::vector<std::unique_ptr<Type>> members, const StructDefBase& struct_def,
    absl::flat_hash_map<std::string, TypeDim> nominal_type_dims_by_identifier,
    std::optional<std::vector<NominalParametricArgument>>
        resolved_parametric_arguments)
    : StructTypeBase(
          std::move(members), struct_def,
          std::move(nominal_type_dims_by_identifier),
          resolved_parametric_arguments.has_value()
              ? std::make_shared<const ResolvedParametricData>(
                    std::move(resolved_parametric_arguments))
              : [] {
                  static const auto unknown =
                      std::make_shared<const ResolvedParametricData>(
                          std::nullopt);
                  return unknown;
                }()) {}

StructTypeBase::StructTypeBase(
    std::vector<std::unique_ptr<Type>> members, const StructDefBase& struct_def,
    absl::flat_hash_map<std::string, TypeDim> nominal_type_dims_by_identifier,
    std::shared_ptr<const ResolvedParametricData> resolved_data)
    : members_(std::move(members)),
      struct_def_base_(struct_def),
      nominal_type_dims_by_identifier_(
          std::move(nominal_type_dims_by_identifier)),
      resolved_parametric_data_(std::move(resolved_data)) {
  CHECK_EQ(members_.size(), struct_def_base_.members().size());
  for (const std::unique_ptr<Type>& member_type : members_) {
    CHECK(!member_type->IsMeta()) << *member_type;
  }
}

bool StructTypeBase::HasEnum() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasEnum(); });
}

bool StructTypeBase::HasToken() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasToken(); });
}

std::string StructTypeBase::ToErrorString() const {
  return absl::StrFormat("struct '%s' structure: %s",
                         struct_def_base_.identifier(),
                         ToStringInternal(FullyQualify::kNo, nullptr));
}

void StructTypeBase::AppendToStringInternal(FullyQualify fully_qualify,
                                            const FileTable* file_table,
                                            TypeStringContext& context,
                                            std::string& output) const {
  std::string struct_name = struct_def_base_.identifier();
  if (fully_qualify == FullyQualify::kYes) {
    CHECK(file_table != nullptr);
    struct_name = absl::StrCat(struct_def_base_.span().GetFilename(*file_table),
                               ":", struct_name);
  }
  absl::StrAppend(&output, struct_name, " {");
  if (!members().empty()) {
    absl::StrAppend(&output, " ");
    for (int64_t i = 0; i < members().size(); ++i) {
      if (i != 0) {
        absl::StrAppend(&output, ", ");
      }
      absl::StrAppend(&output, GetMemberName(i), ": ");
      GetMemberType(i).AppendToStringInternal(fully_qualify, file_table,
                                              context, output);
    }
    absl::StrAppend(&output, " ");
  }
  absl::StrAppend(&output, "}");
}

absl::StatusOr<std::vector<std::string>> StructTypeBase::GetMemberNames()
    const {
  std::vector<std::string> results;
  results.reserve(members().size());
  for (int64_t i = 0; i < members().size(); ++i) {
    results.push_back(std::string(GetMemberName(i)));
  }
  return results;
}

absl::StatusOr<int64_t> StructTypeBase::GetMemberIndex(
    std::string_view name) const {
  XLS_ASSIGN_OR_RETURN(std::vector<std::string> names, GetMemberNames());
  auto it = std::find(names.begin(), names.end(), name);
  if (it == names.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Name not present in tuple type %s: %s",
                        ToStringInternal(FullyQualify::kNo, nullptr), name));
  }
  return std::distance(names.begin(), it);
}

std::optional<const Type*> StructTypeBase::GetMemberTypeByName(
    std::string_view target) const {
  for (int64_t i = 0; i < members().size(); ++i) {
    if (GetMemberName(i) == target) {
      return &GetMemberType(i);
    }
  }
  return std::nullopt;
}

std::vector<TypeDim> StructTypeBase::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const std::unique_ptr<Type>& type : members_) {
    std::vector<TypeDim> t_dims = type->GetAllDims();
    for (auto& dim : t_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> StructTypeBase::GetTotalBitCount() const {
  return GetPublicAggregateBitCount(*this);
}

bool StructTypeBase::HasNamedMember(std::string_view target) const {
  for (int64_t i = 0; i < members().size(); ++i) {
    if (GetMemberName(i) == target) {
      return true;
    }
  }
  return false;
}

bool StructTypeBase::operator==(const Type& other) const {
  if (auto* t = dynamic_cast<const StructType*>(&other)) {
    return Equal(members_, t->members_) &&
           &struct_def_base_ == &t->struct_def_base_;
  }
  return false;
}

StructTypeBase::ResolvedParametricData::ResolvedParametricData(
    std::optional<std::vector<NominalParametricArgument>> arguments)
    : full(arguments.has_value()
               ? std::make_shared<const SpecializationArguments>(
                     std::move(*arguments))
               : nullptr) {}

StructTypeBase::ResolvedParametricData::ResolvedParametricData(
    std::vector<SpecializationArgument> arguments,
    std::vector<SpecializationTypeShapePtr> argument_shapes)
    : full(std::make_shared<const SpecializationArguments>(
          std::move(arguments), std::move(argument_shapes))) {}

const std::optional<std::vector<NominalParametricArgument>>&
StructTypeBase::resolved_parametric_arguments() const {
  static const auto* const unavailable =
      new std::optional<std::vector<NominalParametricArgument>>;
  const auto& full = resolved_parametric_data_->full;
  if (full != nullptr && full->has_full_input()) {
    return full->full_arguments_optional();
  } else {
    return *unavailable;
  }
}

std::shared_ptr<const std::vector<NominalParametricArgument>>
StructTypeBase::shared_resolved_parametric_arguments() const {
  const auto& arguments = resolved_parametric_arguments();
  if (arguments.has_value()) {
    return std::shared_ptr<const std::vector<NominalParametricArgument>>(
        resolved_parametric_data_, &*arguments);
  } else {
    return nullptr;
  }
}

std::shared_ptr<const std::vector<SpecializationArgument>>
StructTypeBase::shared_specialization_arguments() const {
  const auto* arguments = specialization_arguments();
  if (arguments != nullptr) {
    return std::shared_ptr<const std::vector<SpecializationArgument>>(
        resolved_parametric_data_, arguments);
  } else {
    return nullptr;
  }
}

SpecializationTypePtr StructTypeBase::specialization_type() const {
  auto build = [&] {
    using Kind = SpecializationType::Kind;
    SpecializationType::Description description{
        .kind = dynamic_cast<const ProcType*>(this) != nullptr ? Kind::kProc
                                                               : Kind::kStruct,
        .nominal = &struct_def_base()};
    if (specialization_arguments() != nullptr) {
      description.arguments = *specialization_arguments();
    } else if (struct_def_base().IsParametric()) {
      description.nominal_arguments_known = false;
    }
    return SpecializationType::Create(std::move(description));
  };
  if (specialization_arguments() != nullptr) {
    std::call_once(resolved_parametric_data_->identity_once,
                   [&] { resolved_parametric_data_->identity = build(); });
    return resolved_parametric_data_->identity;
  } else {
    // Unknown records share one global empty argument owner across
    // declarations.
    return build();
  }
}

const std::vector<SpecializationTypeShapePtr>&
StructTypeBase::specialization_argument_shapes() const {
  static const auto* const unavailable =
      new std::vector<SpecializationTypeShapePtr>;
  const auto& full = resolved_parametric_data_->full;
  return full == nullptr ? *unavailable : full->argument_shapes();
}

StructType::StructType(
    std::vector<std::unique_ptr<Type>> members, const StructDef& struct_def,
    absl::flat_hash_map<std::string, TypeDim> nominal_type_dims_by_identifier,
    std::optional<std::vector<NominalParametricArgument>>
        resolved_parametric_arguments)
    : StructTypeBase(std::move(members), struct_def,
                     std::move(nominal_type_dims_by_identifier),
                     std::move(resolved_parametric_arguments)) {}

/* static */ std::unique_ptr<StructType>
StructType::CreateWithSpecializationArguments(
    std::vector<std::unique_ptr<Type>> members, const StructDef& struct_def,
    std::vector<SpecializationArgument> arguments,
    std::vector<SpecializationTypeShapePtr> argument_shapes) {
  return absl::WrapUnique(
      new StructType(std::move(members), struct_def, {},
                     std::make_shared<const ResolvedParametricData>(
                         std::move(arguments), std::move(argument_shapes))));
}

/* static */ std::unique_ptr<ProcType>
ProcType::CreateWithSpecializationArguments(
    std::vector<std::unique_ptr<Type>> members, const ProcDef& proc_def,
    std::vector<SpecializationArgument> arguments,
    std::vector<SpecializationTypeShapePtr> argument_shapes) {
  return absl::WrapUnique(
      new ProcType(std::move(members), proc_def, {},
                   std::make_shared<const ResolvedParametricData>(
                       std::move(arguments), std::move(argument_shapes))));
}

// -- SumTypeVariant

/* static */ SumTypeVariant SumTypeVariant::MakeUnit(
    const SumVariant& variant) {
  return SumTypeVariant(variant, std::monostate{});
}

/* static */ SumTypeVariant SumTypeVariant::MakeTuple(
    const SumVariant& variant,
    std::vector<std::unique_ptr<Type>> payload_members) {
  return SumTypeVariant(variant,
                        TuplePayload{.members = std::move(payload_members)});
}

/* static */ SumTypeVariant SumTypeVariant::MakeStruct(
    const SumVariant& variant,
    std::vector<std::unique_ptr<Type>> payload_members) {
  return SumTypeVariant(variant,
                        StructPayload{.members = std::move(payload_members)});
}

SumTypeVariant::SumTypeVariant(const SumVariant& variant, Payload payload)
    : variant_(variant), payload_(std::move(payload)) {
  if (is_unit()) {
    CHECK(variant_.is_unit());
  } else if (is_tuple()) {
    CHECK(variant_.is_tuple());
  } else {
    CHECK(variant_.is_struct());
  }
  ValidateSumTypeVariantPayload(variant_, payload_members());
}

int64_t SumTypeVariant::size() const { return payload_members().size(); }

const Type& SumTypeVariant::GetMemberType(int64_t i) const {
  return *payload_members()[i];
}

absl::Span<const std::unique_ptr<Type>> SumTypeVariant::payload_members()
    const {
  if (const auto* payload = std::get_if<TuplePayload>(&payload_)) {
    return payload->members;
  }
  if (const auto* payload = std::get_if<StructPayload>(&payload_)) {
    return payload->members;
  }
  return {};
}

bool SumTypeVariant::operator==(const SumTypeVariant& other) const {
  absl::Span<const std::unique_ptr<Type>> members = payload_members();
  absl::Span<const std::unique_ptr<Type>> other_members =
      other.payload_members();
  if (&variant_ != &other.variant_ || members.size() != other_members.size()) {
    return false;
  }
  for (int64_t i = 0; i < members.size(); ++i) {
    if (*members[i] != *other_members[i]) {
      return false;
    }
  }
  return true;
}

SumTypeVariant SumTypeVariant::Clone() const {
  if (is_unit()) {
    return MakeUnit(variant_);
  } else if (is_tuple()) {
    return MakeTuple(variant_, ClonePayloadMembers(payload_members()));
  } else {
    return MakeStruct(variant_, ClonePayloadMembers(payload_members()));
  }
}

std::vector<TypeDim> SumTypeVariant::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const auto& member : payload_members()) {
    std::vector<TypeDim> member_dims = member->GetAllDims();
    for (TypeDim& dim : member_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> SumTypeVariant::GetTotalBitCount() const {
  return internal::GetBitCountWithSharedSumPayload(*this);
}

bool SumTypeVariant::HasEnum() const {
  return absl::c_any_of(payload_members(),
                        [](const auto& type) { return type->HasEnum(); });
}

bool SumTypeVariant::HasToken() const {
  return absl::c_any_of(payload_members(),
                        [](const auto& type) { return type->HasToken(); });
}

// -- SumType

SumType::Data::Data(
    const SumDef& sum_def, std::vector<SumTypeVariant> variants,
    std::optional<TypeDim> tag_bit_count,
    std::vector<InterpValue> discriminants,
    std::shared_ptr<const SpecializationArguments> parametric_arguments)
    : sum_def(sum_def),
      variants(std::move(variants)),
      max_payload_bit_count(ComputeMaxSumPayloadBitCount(this->variants)),
      tag_bit_count(tag_bit_count.value_or(TypeDim::CreateU32(
          this->variants.size() <= 1
              ? 0
              : Bits::MinBitCountUnsigned(this->variants.size() - 1)))),
      discriminants(std::move(discriminants)),
      parametric_arguments(std::move(parametric_arguments)),
      parametric_arguments_hash(SumType::HashSpecializationArguments(
          this->parametric_arguments->arguments())),
      has_token(absl::c_any_of(
          this->variants,
          [](const SumTypeVariant& variant) { return variant.HasToken(); })) {
  CHECK_EQ(this->variants.size(), sum_def.variants().size());
  for (int64_t i = 0; i < this->variants.size(); ++i) {
    CHECK_EQ(&this->variants[i].variant(), sum_def.variants()[i]);
  }
  if (this->discriminants.empty()) {
    const int64_t bit_count = this->tag_bit_count.GetAsInt64().value();
    this->discriminants.reserve(this->variants.size());
    for (int64_t i = 0; i < this->variants.size(); ++i) {
      this->discriminants.push_back(InterpValue::MakeUBits(bit_count, i));
    }
  }
  CHECK_EQ(this->discriminants.size(), this->variants.size());
}

SumType::SumType(const SumDef& sum_def, std::vector<SumTypeVariant> variants,
                 std::optional<TypeDim> tag_bit_count,
                 std::vector<InterpValue> discriminants,
                 std::vector<ParametricArgument> parametric_arguments)
    : data_(std::make_shared<const Data>(
          sum_def, std::move(variants), std::move(tag_bit_count),
          std::move(discriminants),
          std::make_shared<const SpecializationArguments>(
              std::move(parametric_arguments)))) {}

/* static */ std::unique_ptr<SumType>
SumType::CreateWithSpecializationArguments(
    const SumDef& sum_def, std::vector<SumTypeVariant> variants,
    std::optional<TypeDim> tag_bit_count,
    std::vector<InterpValue> discriminants,
    std::vector<SpecializationArgument> arguments,
    std::vector<SpecializationTypeShapePtr> argument_shapes) {
  return absl::WrapUnique(new SumType(std::make_shared<const Data>(
      sum_def, std::move(variants), std::move(tag_bit_count),
      std::move(discriminants),
      std::make_shared<const SpecializationArguments>(
          std::move(arguments), std::move(argument_shapes)))));
}

SpecializationTypePtr SumType::specialization_type() const {
  std::call_once(data_->identity_once, [&] {
    data_->identity =
        SpecializationType::Create({.kind = SpecializationType::Kind::kSum,
                                    .nominal = &nominal_type(),
                                    .arguments = specialization_arguments()});
  });
  return data_->identity;
}

bool SumType::operator==(const Type& other) const {
  if (const auto* t = dynamic_cast<const SumType*>(&other); t == nullptr) {
    return false;
  } else if (data_ == t->data_) {
    return true;
  } else if (&nominal_type() != &t->nominal_type() ||
             data_->tag_bit_count != t->data_->tag_bit_count ||
             data_->discriminants != t->data_->discriminants) {
    return false;
  } else {
    // A shared child can appear in both arguments and payloads. Keep completed
    // comparisons for this outer sum comparison, including recursive calls
    // through ordinary aggregate operators. The thread-local pointer carries
    // only this scoped context; no results or borrowed pointers outlive it.
    using EqualPairs = absl::flat_hash_set<std::pair<const Data*, const Data*>>;
    static thread_local EqualPairs* current_equal_pairs = nullptr;
    auto compare_contents = [&] {
      if (data_->parametric_arguments->has_full_input() ||
          t->data_->parametric_arguments->has_full_input()) {
        // Explicitly constructed Types can have inconsistent nominal members;
        // their historical full comparison remains authoritative.
        return parametric_arguments_hash() == t->parametric_arguments_hash() &&
               HasSameParametricArguments(t->parametric_arguments()) &&
               absl::c_equal(variants(), t->variants());
      } else {
        return HasSameSpecializationArguments(t->specialization_arguments()) &&
               absl::c_equal(variants(), t->variants());
      }
    };
    if (current_equal_pairs == nullptr) {
      EqualPairs equal_pairs;
      current_equal_pairs = &equal_pairs;
      absl::Cleanup reset_context([&] { current_equal_pairs = nullptr; });
      // Types are acyclic, so the root pair cannot recur. Avoid inserting it,
      // which also keeps shallow comparisons from allocating cache storage.
      return compare_contents();
    } else {
      const auto pair = std::make_pair(data_.get(), t->data_.get());
      if (current_equal_pairs->contains(pair)) {
        return true;
      } else if (compare_contents()) {
        current_equal_pairs->insert(pair);
        return true;
      } else {
        return false;
      }
    }
  }
}

bool SumType::HasSameParametricArguments(
    absl::Span<const ParametricArgument> arguments) const {
  return SameSumArguments(parametric_arguments(), arguments);
}

/* static */ size_t SumType::HashParametricArguments(
    absl::Span<const ParametricArgument> arguments) {
  return HashSpecializationArguments(CompactArguments(arguments));
}

bool SumType::HasSameSpecializationArguments(
    absl::Span<const SpecializationArgument> arguments) const {
  return SameSpecializationArguments(specialization_arguments(), arguments);
}

/* static */ size_t SumType::HashSpecializationArguments(
    absl::Span<const SpecializationArgument> arguments) {
  size_t hash = absl::HashOf(arguments.size());
  for (const SpecializationArgument& argument : arguments) {
    if (const auto* value = std::get_if<InterpValue>(&argument)) {
      hash = absl::HashOf(hash, argument.index(), HashSumArgumentValue(*value));
    } else {
      const auto& type = std::get<SpecializationTypePtr>(argument);
      CHECK(type != nullptr);
      hash = absl::HashOf(hash, argument.index(), type->hash());
    }
  }
  return hash;
}

void SumType::AppendToStringInternal(FullyQualify fully_qualify,
                                     const FileTable* file_table,
                                     TypeStringContext& context,
                                     std::string& output) const {
  // Ordinary type trees need no counting pass. The first sum counts from the
  // original root, including siblings and their original qualification.
  if (context.sums.empty()) {
    CountSumsForPrinting(context.root, context.root_fully_qualify, context);
  }
  TypeStringContext::SumUse& use =
      context.sums.at({&variants(), fully_qualify});
  if (use.reference_id.has_value()) {
    absl::StrAppend(&output, "@", *use.reference_id);
  } else {
    if (use.count > 1) {
      use.reference_id = context.next_reference_id++;
      absl::StrAppend(&output, "@", *use.reference_id, "=");
    }
    if (fully_qualify == FullyQualify::kYes) {
      CHECK(file_table != nullptr);
      absl::StrAppend(&output, nominal_type().span().GetFilename(*file_table),
                      ":");
    }
    absl::StrAppend(&output, nominal_type().identifier());
    if (!parametric_arguments().empty()) {
      absl::StrAppend(&output, "<");
      for (int64_t i = 0; i < parametric_arguments().size(); ++i) {
        if (i != 0) {
          absl::StrAppend(&output, ", ");
        }
        const ParametricArgument& argument = parametric_arguments().at(i);
        if (const auto* value = std::get_if<InterpValue>(&argument)) {
          absl::StrAppend(&output, value->ToString());
        } else {
          std::get<std::unique_ptr<const Type>>(argument)
              ->AppendToStringInternal(fully_qualify, file_table, context,
                                       output);
        }
      }
      absl::StrAppend(&output, ">");
    }
    absl::StrAppend(&output, " { ");
    for (int64_t i = 0; i < variants().size(); ++i) {
      if (i != 0) {
        absl::StrAppend(&output, " | ");
      }
      const SumTypeVariant& variant = variants()[i];
      absl::StrAppend(&output, variant.variant().identifier());
      if (variant.is_tuple()) {
        absl::StrAppend(&output, "(");
        for (int64_t member_i = 0; member_i < variant.size(); ++member_i) {
          if (member_i != 0) {
            absl::StrAppend(&output, ", ");
          }
          variant.GetMemberType(member_i).AppendToStringInternal(
              fully_qualify, file_table, context, output);
        }
        absl::StrAppend(&output, ")");
      } else if (variant.is_struct() && variant.size() == 0) {
        absl::StrAppend(&output, " {}");
      } else if (variant.is_struct()) {
        absl::StrAppend(&output, " { ");
        for (int64_t member_i = 0; member_i < variant.size(); ++member_i) {
          if (member_i != 0) {
            absl::StrAppend(&output, ", ");
          }
          absl::StrAppend(&output, variant.GetMemberName(member_i), ": ");
          variant.GetMemberType(member_i).AppendToStringInternal(
              fully_qualify, file_table, context, output);
        }
        absl::StrAppend(&output, " }");
      }
    }
    absl::StrAppend(&output, " }");
  }
}

std::string SumType::ToErrorString() const {
  return absl::StrFormat("sum '%s' structure: %s", nominal_type().identifier(),
                         ToStringInternal(FullyQualify::kNo, nullptr));
}

bool SumType::HasEnum() const {
  return absl::c_any_of(variants(), [](const SumTypeVariant& variant) {
    return variant.HasEnum();
  });
}

bool SumType::HasToken() const { return data_->has_token; }

std::vector<TypeDim> SumType::GetAllDims() const {
  std::vector<TypeDim> results = {tag_bit_count()};
  for (const SumTypeVariant& variant : variants()) {
    std::vector<TypeDim> variant_dims = variant.GetAllDims();
    for (TypeDim& dim : variant_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> SumType::GetMaxPayloadBitCount() const {
  XLS_ASSIGN_OR_RETURN(uint32_t payload_bit_count,
                       data_->max_payload_bit_count);
  return TypeDim::CreateU32(payload_bit_count);
}

absl::StatusOr<TypeDim> SumType::GetTotalBitCount() const {
  return internal::GetBitCountWithSharedSumPayload(*this);
}

std::unique_ptr<Type> SumType::CloneToUnique() const {
  return std::unique_ptr<Type>(new SumType(data_));
}

// -- TupleType

TupleType::TupleType(std::vector<std::unique_ptr<Type>> members)
    : members_(std::move(members)) {
#ifndef NDEBUG
  for (const auto& member : members_) {
    DCHECK(member != nullptr);
  }
#endif
}

bool TupleType::operator==(const Type& other) const {
  if (auto* t = dynamic_cast<const TupleType*>(&other)) {
    return Equal(members_, t->members_);
  }
  return false;
}

bool TupleType::HasEnum() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasEnum(); });
}

bool TupleType::HasToken() const {
  return absl::c_any_of(members_,
                        [](const auto& type) { return type->HasToken(); });
}

bool TupleType::empty() const { return members_.empty(); }

int64_t TupleType::size() const { return members_.size(); }

bool TupleType::CompatibleWith(const TupleType& other) const {
  if (members_.size() != other.members_.size()) {
    return false;
  }
  for (int64_t i = 0; i < members_.size(); ++i) {
    if (!members_[i]->CompatibleWith(*other.members_[i])) {
      return false;
    }
  }
  // Same member count and all compatible members.
  return true;
}

std::unique_ptr<Type> TupleType::CloneToUnique() const {
  return std::make_unique<TupleType>(CloneSpan(members_));
}

void TupleType::AppendToStringInternal(FullyQualify fully_qualify,
                                       const FileTable* file_table,
                                       TypeStringContext& context,
                                       std::string& output) const {
  absl::StrAppend(&output, "(");
  for (int64_t i = 0; i < members_.size(); ++i) {
    if (i != 0) {
      absl::StrAppend(&output, ", ");
    }
    members_[i]->AppendToStringInternal(FullyQualify::kNo, file_table, context,
                                        output);
  }
  absl::StrAppend(&output, ")");
}

std::string TupleType::ToInlayHintString() const {
  std::string guts = absl::StrJoin(
      members_, ", ", [](std::string* out, const std::unique_ptr<Type>& m) {
        absl::StrAppend(out, m->ToInlayHintString());
      });
  return absl::StrCat("(", guts, ")");
}

std::vector<TypeDim> TupleType::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const std::unique_ptr<Type>& t : members_) {
    std::vector<TypeDim> t_dims = t->GetAllDims();
    for (auto& dim : t_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> TupleType::GetTotalBitCount() const {
  return GetPublicAggregateBitCount(*this);
}

// -- ArrayType

ArrayType::ArrayType(std::unique_ptr<Type> element_type, const TypeDim& size)
    : element_type_(std::move(element_type)), size_(size) {
  CHECK(!element_type_->IsMeta())
      << "Array element cannot be a metatype because arrays cannot hold types; "
         "got: "
      << element_type_->ToStringInternal(FullyQualify::kNo, nullptr);
}

void ArrayType::AppendToStringInternal(FullyQualify fully_qualify,
                                       const FileTable* file_table,
                                       TypeStringContext& context,
                                       std::string& output) const {
  element_type_->AppendToStringInternal(fully_qualify, file_table, context,
                                        output);
  absl::StrAppend(&output, "[", size_.ToString(), "]");
}

std::string ArrayType::ToInlayHintString() const {
  return absl::StrFormat("%s[%s]", element_type_->ToInlayHintString(),
                         size_.ToString());
}

bool ArrayType::operator==(const Type& other) const {
  VLOG(10) << "ArrayType::operator==; this: " << *this << " other: " << other;
  if (auto* o = dynamic_cast<const ArrayType*>(&other)) {
    return size_ == o->size_ && *element_type_ == *o->element_type_;
  }
  if (IsBitsConstructor(element_type())) {
    if (auto* b = dynamic_cast<const BitsType*>(&other)) {
      const auto* bc =
          dynamic_cast<const BitsConstructorType*>(&element_type());
      VLOG(10) << "size: " << size() << " b->size: " << b->size()
               << " bc->is_signed(): " << bc->is_signed()
               << " b->is_signed(): " << b->is_signed();
      return size() == b->size() &&
             bc->is_signed() == TypeDim::CreateBool(b->is_signed());
    }
  }
  return false;
}

std::vector<TypeDim> ArrayType::GetAllDims() const {
  std::vector<TypeDim> results;
  results.push_back(size_.Clone());
  std::vector<TypeDim> element_dims = element_type_->GetAllDims();
  for (auto& dim : element_dims) {
    results.push_back(std::move(dim));
  }
  return results;
}

absl::StatusOr<TypeDim> ArrayType::GetTotalBitCount() const {
  return GetPublicAggregateBitCount(*this);
}

ArrayType::InnerMostElementType ArrayType::GetInnermostElementType() const {
  const ArrayType* array_type = this;
  bool all_dims_known = true;
  while (true) {
    all_dims_known = all_dims_known && array_type->size().GetAsInt64().ok();
    const ArrayType* nested_array_type =
        dynamic_cast<const ArrayType*>(&array_type->element_type());
    if (nested_array_type == nullptr) {
      break;
    }
    array_type = nested_array_type;
  }
  return {std::cref(array_type->element_type()), std::cref(*array_type),
          all_dims_known};
}

int ArrayType::ArrayDimensions() const {
  const Type* element_type = element_type_.get();
  int size = 1;
  while (const ArrayType* child_type =
             dynamic_cast<const ArrayType*>(element_type)) {
    size++;
    element_type = &child_type->element_type();
  }
  return size;
}

// -- EnumType

std::string EnumType::ToStringInternal(FullyQualify fully_qualify,
                                       const FileTable* file_table) const {
  if (fully_qualify == FullyQualify::kYes) {
    return absl::StrCat(enum_def_.span().GetFilename(*file_table), ":",
                        enum_def_.identifier());
  }
  return enum_def_.identifier();
}

void EnumType::AppendToStringInternal(FullyQualify fully_qualify,
                                      const FileTable* file_table,
                                      TypeStringContext&,
                                      std::string& output) const {
  absl::StrAppend(&output, ToStringInternal(fully_qualify, file_table));
}

std::vector<TypeDim> EnumType::GetAllDims() const {
  std::vector<TypeDim> result;
  result.push_back(size_.Clone());
  return result;
}

// -- FunctionType

bool FunctionType::operator==(const Type& other) const {
  if (auto* o = dynamic_cast<const FunctionType*>(&other)) {
    if (params_.size() != o->params_.size()) {
      return false;
    }
    for (int64_t i = 0; i < params_.size(); ++i) {
      if (*params_[i] != *o->params_[i]) {
        return false;
      }
    }
    return *return_type_ == *o->return_type_;
  }
  return false;
}

std::vector<const Type*> FunctionType::GetParams() const {
  std::vector<const Type*> results;
  results.reserve(params_.size());
  for (const auto& param : params_) {
    results.push_back(param.get());
  }
  return results;
}

void FunctionType::AppendToStringInternal(FullyQualify fully_qualify,
                                          const FileTable* file_table,
                                          TypeStringContext& context,
                                          std::string& output) const {
  absl::StrAppend(&output, "(");
  for (int64_t i = 0; i < params_.size(); ++i) {
    if (i != 0) {
      absl::StrAppend(&output, ", ");
    }
    params_[i]->AppendToStringInternal(fully_qualify, file_table, context,
                                       output);
  }
  absl::StrAppend(&output, ") -> ");
  return_type_->AppendToStringInternal(fully_qualify, file_table, context,
                                       output);
}

std::vector<TypeDim> FunctionType::GetAllDims() const {
  std::vector<TypeDim> results;
  for (const auto& param : params_) {
    std::vector<TypeDim> param_dims = param->GetAllDims();
    for (auto& dim : param_dims) {
      results.push_back(std::move(dim));
    }
  }
  return results;
}

absl::StatusOr<TypeDim> FunctionType::GetTotalBitCount() const {
  return GetPublicAggregateBitCount(*this);
}

ChannelType::ChannelType(std::unique_ptr<Type> payload_type,
                         ChannelDirection direction)
    : payload_type_(std::move(payload_type)), direction_(direction) {
  CHECK(payload_type_ != nullptr);
  CHECK(!payload_type_->IsMeta());
}

void ChannelType::AppendToStringInternal(FullyQualify fully_qualify,
                                         const FileTable* file_table,
                                         TypeStringContext& context,
                                         std::string& output) const {
  absl::StrAppend(&output, "chan(");
  payload_type_->AppendToStringInternal(fully_qualify, file_table, context,
                                        output);
  absl::StrAppend(&output,
                  ", dir=", direction_ == ChannelDirection::kIn ? "in" : "out",
                  ")");
}

std::vector<TypeDim> ChannelType::GetAllDims() const {
  return payload_type_->GetAllDims();
}

absl::StatusOr<TypeDim> ChannelType::GetTotalBitCount() const {
  return payload_type_->GetTotalBitCount();
}

bool ChannelType::operator==(const Type& other) const {
  if (auto* o = dynamic_cast<const ChannelType*>(&other)) {
    return *payload_type_ == *o->payload_type_ && direction_ == o->direction_;
  }
  return false;
}

bool ChannelType::HasEnum() const { return payload_type_->HasEnum(); }
bool ChannelType::HasToken() const { return payload_type_->HasToken(); }

std::unique_ptr<Type> ChannelType::CloneToUnique() const {
  return std::make_unique<ChannelType>(payload_type_->CloneToUnique(),
                                       direction_);
}

absl::StatusOr<bool> IsSigned(const Type& c) {
  if (auto* bits = dynamic_cast<const BitsType*>(&c)) {
    return bits->is_signed();
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&c)) {
    std::optional<bool> signedness = enum_type->is_signed();
    if (!signedness.has_value()) {
      return absl::InvalidArgumentError(
          "Signedness not present for EnumType: " +
          c.ToStringInternal(FullyQualify::kNo, nullptr));
    }
    return signedness.value();
  }
  const BitsConstructorType* bc;
  if (IsArrayOfBitsConstructor(c, &bc)) {
    const TypeDim& is_signed = bc->is_signed();
    XLS_ASSIGN_OR_RETURN(int64_t value, is_signed.GetAsInt64());
    return value != 0;
  }
  return absl::InvalidArgumentError(
      "Cannot determined signedness; type is neither enum nor bits: " +
      c.ToStringInternal(FullyQualify::kNo, nullptr));
}

absl::StatusOr<TypeDim> MetaType::GetTotalBitCount() const {
  return absl::InvalidArgumentError(
      "Cannot get total bit count of a meta-type, as these are not "
      "realizable as values; meta-type: " +
      ToString());
}

bool IsBitsLike(const Type& t) {
  return dynamic_cast<const BitsType*>(&t) != nullptr ||
         IsArrayOfBitsConstructor(t);
}

std::string ToTypeString(const BitsLikeProperties& properties) {
  bool is_signed = properties.is_signed.GetAsBool().value();
  return absl::StrFormat("%sN[%s]", is_signed ? "s" : "u",
                         properties.size.ToString());
}

std::optional<BitsLikeProperties> GetBitsLike(const Type& t) {
  if (auto* bits_type = dynamic_cast<const BitsType*>(&t);
      bits_type != nullptr) {
    return BitsLikeProperties{
        .is_signed = TypeDim::CreateBool(bits_type->is_signed()),
        .size = bits_type->size()};
  }
  const BitsConstructorType* bc;
  if (IsArrayOfBitsConstructor(t, &bc)) {
    auto* array = dynamic_cast<const ArrayType*>(&t);
    return BitsLikeProperties{.is_signed = bc->is_signed(),
                              .size = array->size()};
  }
  return std::nullopt;
}

static std::optional<bool> GetKnownSignedness(
    const BitsLikeProperties& properties) {
  const TypeDim& is_signed = properties.is_signed;
  CHECK(is_signed.value().IsBool());
  return is_signed.GetAsBool().value();
}

static std::optional<int64_t> GetKnownBitCount(
    const BitsLikeProperties& properties) {
  const TypeDim& size = properties.size;
  return size.GetAsInt64().value();
}

bool IsKnownU1(const BitsLikeProperties& properties) {
  std::optional<bool> signedness = GetKnownSignedness(properties);
  std::optional<int64_t> bit_count = GetKnownBitCount(properties);
  return signedness == false && bit_count == 1;
}

bool IsKnownU32(const BitsLikeProperties& properties) {
  std::optional<bool> signedness = GetKnownSignedness(properties);
  std::optional<int64_t> bit_count = GetKnownBitCount(properties);
  return signedness == false && bit_count == 32;
}

bool TypeContainsSemanticSum(const Type& type) {
  return ContainsSemanticSum(type, FunctionResultTraversal::kInclude);
}

namespace {

// Cloned wrappers share their immutable variant vector. Keep results only for
// this traversal, so repeated alternatives visit each description once.
using SumInhabitanceMemo =
    absl::flat_hash_map<const std::vector<SumTypeVariant>*, bool>;

absl::StatusOr<bool> TypeIsInhabitedInternal(
    const Type& type, SumInhabitanceMemo& inhabited_sums);

absl::StatusOr<bool> SumVariantIsInhabitedInternal(
    const SumTypeVariant& variant, SumInhabitanceMemo& inhabited_sums) {
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(
        bool member_is_inhabited,
        TypeIsInhabitedInternal(variant.GetMemberType(i), inhabited_sums));
    if (!member_is_inhabited) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<bool> TypeIsInhabitedInternal(
    const Type& type, SumInhabitanceMemo& inhabited_sums) {
  if (auto* channel_type = dynamic_cast<const ChannelType*>(&type)) {
    return TypeIsInhabitedInternal(channel_type->payload_type(),
                                   inhabited_sums);
  } else if (GetBitsLike(type).has_value()) {
    return true;
  } else if (auto* tuple_type = dynamic_cast<const TupleType*>(&type)) {
    for (const std::unique_ptr<Type>& member_type : tuple_type->members()) {
      XLS_ASSIGN_OR_RETURN(
          bool member_is_inhabited,
          TypeIsInhabitedInternal(*member_type, inhabited_sums));
      if (!member_is_inhabited) {
        return false;
      }
    }
    return true;
  } else if (auto* struct_type = dynamic_cast<const StructTypeBase*>(&type)) {
    for (int64_t i = 0; i < struct_type->size(); ++i) {
      XLS_ASSIGN_OR_RETURN(bool member_is_inhabited,
                           TypeIsInhabitedInternal(
                               struct_type->GetMemberType(i), inhabited_sums));
      if (!member_is_inhabited) {
        return false;
      }
    }
    return true;
  } else if (auto* array_type = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t size, array_type->size().GetAsInt64());
    if (size == 0) {
      return true;
    } else {
      return TypeIsInhabitedInternal(array_type->element_type(),
                                     inhabited_sums);
    }
  } else if (auto* sum_type = dynamic_cast<const SumType*>(&type)) {
    const auto* description = &sum_type->variants();
    auto cached = inhabited_sums.find(description);
    if (cached != inhabited_sums.end()) {
      return cached->second;
    } else {
      bool is_inhabited = false;
      for (const SumTypeVariant& variant : sum_type->variants()) {
        XLS_ASSIGN_OR_RETURN(
            bool variant_is_inhabited,
            SumVariantIsInhabitedInternal(variant, inhabited_sums));
        if (variant_is_inhabited) {
          is_inhabited = true;
          break;
        }
      }
      inhabited_sums.emplace(description, is_inhabited);
      return is_inhabited;
    }
  } else if (auto* enum_type = dynamic_cast<const EnumType*>(&type)) {
    return !enum_type->members().empty();
  } else {
    return true;
  }
}

}  // namespace

absl::StatusOr<bool> SumVariantIsInhabited(const SumTypeVariant& variant) {
  SumInhabitanceMemo inhabited_sums;
  return SumVariantIsInhabitedInternal(variant, inhabited_sums);
}

absl::StatusOr<bool> TypeIsInhabited(const Type& type) {
  SumInhabitanceMemo inhabited_sums;
  return TypeIsInhabitedInternal(type, inhabited_sums);
}

namespace internal {

absl::StatusOr<TypeDim> GetBitCountWithSharedSumPayload(
    const SumTypeVariant& variant) {
  TypeDim variant_bits = TypeDim::CreateU32(0);
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(TypeDim member_bits,
                         ComputeTypeBitCount(variant.GetMemberType(i),
                                             BitCountOverflow::kReject));
    XLS_ASSIGN_OR_RETURN(variant_bits,
                         ComputeBitCountOperation(variant_bits, member_bits,
                                                  BitCountOperation::kAdd,
                                                  BitCountOverflow::kReject));
  }
  return variant_bits;
}

absl::StatusOr<TypeDim> GetBitCountWithSharedSumPayload(const Type& type) {
  return ComputeTypeBitCount(type, BitCountOverflow::kReject);
}

}  // namespace internal

}  // namespace xls::dslx
