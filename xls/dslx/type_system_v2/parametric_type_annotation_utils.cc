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

#include "xls/dslx/type_system_v2/parametric_type_annotation_utils.h"

#include <optional>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/log/check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast_cloner.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system_v2/inference_table.h"
#include "xls/dslx/type_system_v2/inference_table_utils.h"
#include "xls/dslx/type_system_v2/populate_table_visitor.h"

namespace xls::dslx {

absl::StatusOr<const TypeAnnotation*> GetParametricFreeType(
    const TypeAnnotation* type,
    const absl::flat_hash_map<const NameDef*, ExprOrType>& actual_values,
    InferenceTable& table, ImportData& import_data,
    std::optional<const TypeAnnotation*> real_self_type,
    bool clone_if_no_parametrics) {
  if (!clone_if_no_parametrics) {
    XLS_ASSIGN_OR_RETURN(
        std::vector<std::pair<const NameRef*, const NameDef*>> refs,
        CollectReferencedUnder(type));
    bool any_parametrics = false;
    for (const auto& [name_ref, name_def] : refs) {
      (void)name_ref;
      if (name_def->parent()->kind() == AstNodeKind::kParametricBinding) {
        any_parametrics = true;
        break;
      }
    }
    if (!any_parametrics) {
      return type;
    }
  }

  CloneReplacer replacer = ChainCloneReplacers(
      &PreserveTypeDefinitionsReplacer,
      ChainCloneReplacers(
          NameRefMapper(table, actual_values, type->owner(),
                        /*add_parametric_binding_type_annotation=*/true),
          [&](const AstNode* node, Module*,
              const absl::flat_hash_map<const AstNode*, AstNode*>&)
              -> absl::StatusOr<std::optional<AstNode*>> {
            // Leave attrs in place; they never need parametric replacement
            // here.
            if (node->kind() == AstNodeKind::kAttr) {
              return const_cast<AstNode*>(node);
            }
            return std::nullopt;
          }));

  replacer = ChainCloneReplacers(
      std::move(replacer),
      [&](const AstNode* node, Module*,
          const absl::flat_hash_map<const AstNode*, AstNode*>&)
          -> absl::StatusOr<std::optional<AstNode*>> {
        if (node->kind() != AstNodeKind::kTypeAnnotation) {
          return std::nullopt;
        }

        const auto* annotation = absl::down_cast<const TypeAnnotation*>(node);
        if (real_self_type.has_value() &&
            annotation->IsAnnotation<SelfTypeAnnotation>()) {
          return table.Clone(*real_self_type, &NoopCloneReplacer,
                             type->owner());
        }

        // Replace the containing TVTA when the entire type variable is known.
        if (annotation->IsAnnotation<TypeVariableTypeAnnotation>()) {
          const auto* tvta =
              absl::down_cast<const TypeVariableTypeAnnotation*>(annotation);
          const auto it = actual_values.find(
              std::get<const NameDef*>(tvta->type_variable()->name_def()));
          if (it != actual_values.end()) {
            return ToAstNode(it->second);
          }
        }
        return std::nullopt;
      });

  XLS_ASSIGN_OR_RETURN(
      absl::flat_hash_map<const AstNode*, AstNode*> clones,
      CloneAstAndGetAllPairs(type, type->owner(), std::move(replacer)));
  AstNode* clone = clones.at(type);
  std::unique_ptr<PopulateTableVisitor> visitor =
      CreatePopulateTableVisitor(type->owner(), &table, &import_data,
                                 /*typecheck_imported_module=*/nullptr);

  // Replacement can fabricate new indirect annotations, so repopulate table
  // data for the cloned subtree before it is reused by TIv2.
  XLS_RETURN_IF_ERROR(visitor->PopulateFromTypeAnnotation(
      absl::down_cast<TypeAnnotation*>(clone)));

  return absl::down_cast<const TypeAnnotation*>(clone);
}

absl::StatusOr<const TypeAnnotation*> GetParametricFreeSumMemberType(
    const TypeAnnotation* member_type, const SumRef& sum_ref,
    InferenceTable& table, ImportData& import_data) {
  if (!sum_ref.def->IsParametric() || sum_ref.parametrics.empty()) {
    return member_type;
  }

  CHECK_GE(sum_ref.def->parametric_bindings().size(),
           sum_ref.parametrics.size());
  absl::flat_hash_map<const NameDef*, ExprOrType> actual_values;
  for (int i = 0; i < sum_ref.parametrics.size(); ++i) {
    actual_values.emplace(sum_ref.def->parametric_bindings()[i]->name_def(),
                          sum_ref.parametrics[i]);
  }
  return GetParametricFreeType(member_type, actual_values, table, import_data,
                               /*real_self_type=*/std::nullopt,
                               /*clone_if_no_parametrics=*/false);
}

}  // namespace xls::dslx
