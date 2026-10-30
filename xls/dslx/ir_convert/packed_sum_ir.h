// Copyright 2026 The XLS Authors
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

#ifndef XLS_DSLX_IR_CONVERT_PACKED_SUM_IR_H_
#define XLS_DSLX_IR_CONVERT_PACKED_SUM_IR_H_

#include <cstdint>

#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/source_location.h"

namespace xls {
class Package;
class Type;
}  // namespace xls

namespace xls::dslx::internal {

// Returns the concrete width of any candidate payload type, using one shared
// payload per nested semantic sum. Returns an error if the width exceeds the
// unsigned 32-bit range, even if the type contains no semantic sum.
absl::StatusOr<int64_t> GetPackedSumBitCount(const Type& type);

// Returns the direct sum carrier `(semantic tag, (shared payload bits,))`.
absl::StatusOr<xls::Type*> GetPackedSumIrType(Package& package,
                                              const SumType& sum);

// Projects the direct sum carrier without validating its tag or padding.
BValue GetPackedSumTag(BuilderBase& builder, BValue value,
                       const SourceInfo& loc);
BValue GetPackedSumPayload(BuilderBase& builder, BValue value,
                           const SourceInfo& loc);
// Returns all of the carrier's bits, with the raw tag above the payload.
BValue GetPackedSumRawBits(BuilderBase& builder, BValue value,
                           const SourceInfo& loc);

// Locates tuple, struct, or variant members in a packed sum payload. Start with
// the total width of those members and visit them in declaration order. The
// first member occupies the most-significant bits; returned offsets count from
// the least-significant bit. This computes positions without constructing IR.
class PackedSumMemberLayout {
 public:
  struct Slice {
    int64_t offset;
    int64_t width;
  };

  explicit PackedSumMemberLayout(int64_t bit_count) : offset_(bit_count) {}

  absl::StatusOr<Slice> Next(const Type& member);

 private:
  int64_t offset_;
};

absl::StatusOr<BValue> BuildPackedSumDiscriminant(BuilderBase& builder,
                                                  const SumType& sum,
                                                  int64_t variant_index,
                                                  const SourceInfo& loc);

// Constructs `(semantic tag, (shared payload bits,))` from only the active
// variant's members. Unused high payload bits are zeroed.
absl::StatusOr<BValue> BuildPackedSumValue(
    BuilderBase& builder, const SumTypeEncoding& encoding,
    const SumTypeEncoding::VariantInfo& variant,
    absl::Span<const BValue> members, const SourceInfo& loc);

// Reconstructs one packed member without checking sum tags or padding. Callers
// can slice out only the members that their operation needs to observe. The
// first array element occupies the least-significant bits, unlike the first
// tuple or struct member, which occupies the most-significant bits. The
// resolver supplies the canonical IR element type for every array, even if
// empty.
absl::StatusOr<BValue> UnpackPackedSumPayload(
    BuilderBase& builder, const Type& type, BValue bits,
    absl::FunctionRef<absl::StatusOr<xls::Type*>(const Type&)> resolve_type,
    const SourceInfo& loc);

// Compares tags and only the selected payload, ignoring padding recursively.
// If a sum declares variants, equal undeclared tags use the last variant's
// payload interpretation. A sum with no variants has no fallback and compares
// its raw representation, so it compares equal exactly when the tag bits match.
// This operation does not assert tag validity.
absl::StatusOr<BValue> BuildPackedSumEquality(BuilderBase& builder,
                                              const Type& type, BValue lhs,
                                              BValue rhs,
                                              const SourceInfo& loc);

}  // namespace xls::dslx::internal

#endif  // XLS_DSLX_IR_CONVERT_PACKED_SUM_IR_H_
