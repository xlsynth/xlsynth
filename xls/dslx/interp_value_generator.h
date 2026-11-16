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

#ifndef XLS_DSLX_INTERP_VALUE_GENERATOR_H_
#define XLS_DSLX_INTERP_VALUE_GENERATOR_H_

#include <cstdint>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {

// Supplies a caller-specific distribution for leaf bits values. Typed
// traversal remains responsible for selecting only declared, inhabited enum
// and sum constructors and for canonicalizing their encoded payload slots.
using InterpValueBitsGenerator = absl::FunctionRef<absl::StatusOr<InterpValue>(
    absl::BitGenRef, const BitsLikeProperties&, absl::Span<const InterpValue>)>;

// Generates source-domain values, caching inhabited constructors and payload
// widths per concrete sum type. A channel type requests a generated message of
// its payload type, not a channel handle. All referenced types must outlive the
// generator.
class InterpValueGenerator {
 public:
  // Generates one source-domain value using the caller's leaf-bit generator.
  absl::StatusOr<InterpValue> Generate(absl::BitGenRef bit_gen,
                                       const Type& type,
                                       absl::Span<const InterpValue> prior,
                                       InterpValueBitsGenerator bits_generator);

  // Generates one source-domain value using uniformly distributed leaf bits.
  absl::StatusOr<InterpValue> Generate(absl::BitGenRef bit_gen,
                                       const Type& type,
                                       absl::Span<const InterpValue> prior);

  // Generates one value per type and passes earlier values to the callback as
  // `prior`.
  absl::StatusOr<std::vector<InterpValue>> GenerateValues(
      absl::BitGenRef bit_gen, absl::Span<const Type* const> types,
      InterpValueBitsGenerator bits_generator);

  // Generates one value per type using uniformly distributed leaf bits.
  absl::StatusOr<std::vector<InterpValue>> GenerateValues(
      absl::BitGenRef bit_gen, absl::Span<const Type* const> types);

 private:
  absl::StatusOr<InterpValue> GenerateSum(
      absl::BitGenRef bit_gen, const SumType& sum_type,
      absl::Span<const InterpValue> prior,
      InterpValueBitsGenerator bits_generator);

  struct SumGenerationInfo {
    std::vector<int64_t> inhabited_variant_indices;
    int64_t payload_slot_bit_count;
  };

  absl::flat_hash_map<const SumType*, SumGenerationInfo> sum_generation_info_;
};

// Generates a value in the source-level domain of `type`, including tokens,
// structs, tuples, arrays, numeric enums, and recursively nested semantic sums.
// A channel type requests a message of its payload type, not a channel handle.
absl::StatusOr<InterpValue> GenerateInterpValue(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior,
    InterpValueBitsGenerator bits_generator);

// Generates a source-domain value using uniformly distributed bits leaves.
absl::StatusOr<InterpValue> GenerateInterpValue(
    absl::BitGenRef bit_gen, const Type& type,
    absl::Span<const InterpValue> prior);

// Generates one valid source-domain value per type. Earlier generated values
// are available to the bits callback as `prior`.
absl::StatusOr<std::vector<InterpValue>> GenerateInterpValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types,
    InterpValueBitsGenerator bits_generator);

// Generates one valid source-domain value per type using uniform bits leaves.
absl::StatusOr<std::vector<InterpValue>> GenerateInterpValues(
    absl::BitGenRef bit_gen, absl::Span<const Type* const> types);

}  // namespace xls::dslx

#endif  // XLS_DSLX_INTERP_VALUE_GENERATOR_H_
