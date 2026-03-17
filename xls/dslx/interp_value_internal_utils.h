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

#include "absl/status/statusor.h"
#include "xls/dslx/interp_value.h"

namespace xls::dslx {

class Type;

namespace internal {

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
