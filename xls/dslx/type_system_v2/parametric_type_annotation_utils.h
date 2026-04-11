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

#ifndef XLS_DSLX_TYPE_SYSTEM_V2_PARAMETRIC_TYPE_ANNOTATION_UTILS_H_
#define XLS_DSLX_TYPE_SYSTEM_V2_PARAMETRIC_TYPE_ANNOTATION_UTILS_H_

#include <optional>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/type_system_v2/type_annotation_utils.h"

namespace xls::dslx {

class ImportData;
class InferenceTable;

// Returns `type` with parametric references in `actual_values` substituted by
// their concrete expression/type annotations. The returned annotation is owned
// by `table` when substitution or `Self` replacement requires cloning; when
// `clone_if_no_parametrics` is false and `type` references no parametric
// bindings, the original `type` pointer is returned. If `real_self_type` is
// present, `Self` annotations are replaced with a clone of that concrete type.
// Newly cloned annotations are populated into `table` before being returned so
// later TIv2 stages can resolve their indirect annotation data.
absl::StatusOr<const TypeAnnotation*> GetParametricFreeType(
    const TypeAnnotation* type,
    const absl::flat_hash_map<const NameDef*, ExprOrType>& actual_values,
    InferenceTable& table, ImportData& import_data,
    std::optional<const TypeAnnotation*> real_self_type = std::nullopt,
    bool clone_if_no_parametrics = true);

// Convenience entry point for a payload member type of `sum_ref`. When the sum
// reference carries explicit parametrics, substitutes those parametrics into
// `member_type` with `GetParametricFreeType()` using the sum definition's
// binding order. Non-parametric sums and sum references with no parametric
// arguments return the original `member_type`.
absl::StatusOr<const TypeAnnotation*> GetParametricFreeSumMemberType(
    const TypeAnnotation* member_type, const SumRef& sum_ref,
    InferenceTable& table, ImportData& import_data);

}  // namespace xls::dslx

#endif  // XLS_DSLX_TYPE_SYSTEM_V2_PARAMETRIC_TYPE_ANNOTATION_UTILS_H_
