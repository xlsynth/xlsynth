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

// Concrete width for bits packed with one shared payload per nested sum.
absl::StatusOr<int64_t> GetPackedSumBitCount(const Type& type);

// Projects the same IR shape that UnpackPackedSumPayload constructs, without
// building values. In particular, empty arrays keep their packed element type.
absl::StatusOr<xls::Type*> GetPackedSumIrType(Package& package,
                                              const Type& type);

absl::StatusOr<BValue> BuildPackedSumDiscriminant(BuilderBase& builder,
                                                  const SumType& sum,
                                                  int64_t variant_index,
                                                  const SourceInfo& loc);

// Constructs `(semantic tag, (shared payload bits,))` from only the active
// variant's members. Unused high payload bits are zeroed.
absl::StatusOr<BValue> BuildPackedSumValue(
    BuilderBase& builder, const SumType& sum,
    const SumTypeEncoding::VariantInfo& variant,
    absl::Span<const BValue> members, const SourceInfo& loc);

// Reconstructs one packed member without checking sum tags or padding. Callers
// can slice out only the members that their operation needs to observe.
absl::StatusOr<BValue> UnpackPackedSumPayload(BuilderBase& builder,
                                              const Type& type, BValue bits,
                                              const SourceInfo& loc);

// Compares tags and only the selected payload, ignoring padding recursively.
// Equal undeclared tags use the last declared variant's payload interpretation;
// this operation does not assert tag validity.
absl::StatusOr<BValue> BuildPackedSumEquality(BuilderBase& builder,
                                              const Type& type, BValue lhs,
                                              BValue rhs,
                                              const SourceInfo& loc);

}  // namespace xls::dslx::internal

#endif  // XLS_DSLX_IR_CONVERT_PACKED_SUM_IR_H_
