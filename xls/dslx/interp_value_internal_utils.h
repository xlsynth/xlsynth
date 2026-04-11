// Copyright 2021 The XLS Authors
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

#ifndef XLS_DSLX_INTERP_VALUE_INTERNAL_UTILS_H_
#define XLS_DSLX_INTERP_VALUE_INTERNAL_UTILS_H_

#include <cstdint>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "xls/dslx/interp_value.h"

namespace xls::dslx {

class Type;

namespace internal {

// Borrowed view over the Phase 1 tuple encoding for a semantic sum value.
struct EncodedSumView {
  const InterpValue& tag;
  absl::Span<const InterpValue> payload_slots;
};

inline absl::StatusOr<EncodedSumView> GetEncodedSumView(
    const InterpValue& value) {
  if (!value.IsTuple()) {
    return absl::InvalidArgumentError(
        "Expected encoded sum value to be tuple-valued.");
  }
  const std::vector<InterpValue>& sum_elements = value.GetValuesOrDie();
  if (sum_elements.size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected encoded sum value to have 2 elements; got %d",
                        static_cast<int64_t>(sum_elements.size())));
  }

  const InterpValue& payload_slots = sum_elements[1];
  if (!payload_slots.IsTuple()) {
    return absl::InvalidArgumentError(
        "Expected encoded sum payload slots to be tuple-valued.");
  }

  return EncodedSumView{
      .tag = sum_elements[0],
      .payload_slots = payload_slots.GetValuesOrDie(),
  };
}

// Creates the Phase 1 tuple encoding used to carry semantic sum values.
InterpValue CreateEncodedSumTuple(InterpValue tag,
                                  std::vector<InterpValue> payload_slots);

// Creates a shape-correct dead InterpValue for internal implementation use.
//
// This is intentionally separate from the public value-creation helpers because
// callers use it for inactive Phase 1 sum payload slots and other operands that
// are semantically unused but must still carry the right concrete shape.
absl::StatusOr<InterpValue> CreateInternalPlaceholderValueFromType(
    const Type& type);

}  // namespace internal
}  // namespace xls::dslx

#endif  // XLS_DSLX_INTERP_VALUE_INTERNAL_UTILS_H_
