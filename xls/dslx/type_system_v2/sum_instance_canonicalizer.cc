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

#include "xls/dslx/type_system_v2/sum_instance_canonicalizer.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_cloner.h"
#include "xls/dslx/type_system_v2/import_utils.h"

namespace xls::dslx {
namespace {

absl::flat_hash_map<const NameDef*, NameDef*> BuildNameDefMap(
    const absl::flat_hash_map<const AstNode*, AstNode*>& old_to_new) {
  absl::flat_hash_map<const NameDef*, NameDef*> name_def_map;
  for (const auto& [old_node, new_node] : old_to_new) {
    if (const auto* old_name_def = dynamic_cast<const NameDef*>(old_node)) {
      name_def_map.emplace(old_name_def,
                           absl::down_cast<NameDef*>(new_node));
    }
  }
  return name_def_map;
}

absl::StatusOr<std::optional<AstNode*>> RebindLocalTypeRef(
    const AstNode* node, Module* target_module,
    const absl::flat_hash_map<const AstNode*, AstNode*>&) {
  if (node->kind() != AstNodeKind::kTypeRef) {
    return std::nullopt;
  }
  const auto* type_ref = absl::down_cast<const TypeRef*>(node);
  TypeDefinition type_definition = type_ref->type_definition();
  AstNode* definition_node = TypeDefinitionToAstNode(type_definition);
  if (definition_node->owner() != node->owner()) {
    return std::nullopt;
  }
  AnyNameDef name_def = TypeDefinitionGetNameDef(type_definition);
  std::string identifier =
      absl::visit(Visitor{[](const auto* def) { return def->identifier(); }},
                  name_def);
  absl::StatusOr<TypeDefinition> canonical_definition =
      target_module->GetTypeDefinition(identifier);
  if (!canonical_definition.ok()) {
    return std::nullopt;
  }
  return target_module->Make<TypeRef>(type_ref->span(),
                                      *canonical_definition);
}

absl::StatusOr<Expr*> CloneExprIntoModule(
    Expr* expr, Module* target_module,
    const absl::flat_hash_map<const AstNode*, AstNode*>& old_to_new) {
  if (auto it = old_to_new.find(expr); it != old_to_new.end()) {
    return absl::down_cast<Expr*>(it->second);
  }
  absl::flat_hash_map<const NameDef*, NameDef*> name_def_map =
      BuildNameDefMap(old_to_new);
  XLS_ASSIGN_OR_RETURN(
      auto pairs,
      CloneAstAndGetAllPairs(
          expr, std::optional<Module*>{target_module},
          ChainCloneReplacers(&RebindLocalTypeRef,
                              NameRefReplacer(&name_def_map))));
  return absl::down_cast<Expr*>(pairs.at(expr));
}

absl::StatusOr<ColonRef*> CloneColonRefIntoModule(
    ColonRef* colon_ref, Module* target_module,
    const absl::flat_hash_map<const AstNode*, AstNode*>& old_to_new) {
  XLS_ASSIGN_OR_RETURN(
      Expr * expr,
      CloneExprIntoModule(colon_ref, target_module, old_to_new));
  return absl::down_cast<ColonRef*>(expr);
}

absl::StatusOr<const ColonRef*> GetConstructorRef(
    const TypeAnnotation* type_annotation) {
  auto* type_ref_type_annotation =
      dynamic_cast<const TypeRefTypeAnnotation*>(type_annotation);
  XLS_RET_CHECK_NE(type_ref_type_annotation, nullptr);
  TypeDefinition type_definition =
      type_ref_type_annotation->type_ref()->type_definition();
  XLS_RET_CHECK(std::holds_alternative<ColonRef*>(type_definition));
  return std::get<ColonRef*>(type_definition);
}

bool IsPatternContext(const AstNode* node) {
  for (const AstNode* current = node->parent(); current != nullptr;
       current = current->parent()) {
    if (dynamic_cast<const NameDefTree*>(current) != nullptr ||
        dynamic_cast<const ConstructorPattern*>(current) != nullptr) {
      return true;
    }
  }
  return false;
}

}  // namespace

absl::StatusOr<std::optional<std::unique_ptr<Module>>>
CanonicalizeSumInstances(const Module& module, const ImportData& import_data) {
  bool changed = false;
  CloneReplacer replacer =
      [&changed, &import_data](
          const AstNode* node, Module* target_module,
          const absl::flat_hash_map<const AstNode*, AstNode*>& old_to_new)
      -> absl::StatusOr<std::optional<AstNode*>> {
    if (const auto* invocation = dynamic_cast<const Invocation*>(node)) {
      auto* constructor =
          dynamic_cast<ColonRef*>(const_cast<Expr*>(invocation->callee()));
      if (constructor == nullptr) {
        return std::nullopt;
      }
      XLS_ASSIGN_OR_RETURN(
          std::optional<SumConstructorRef> constructor_ref,
          ResolveSumConstructor(constructor, import_data));
      if (!constructor_ref.has_value()) {
        return std::nullopt;
      }
      XLS_ASSIGN_OR_RETURN(
          ColonRef * cloned_constructor,
          CloneColonRefIntoModule(constructor, target_module, old_to_new));
      std::vector<Expr*> args;
      args.reserve(invocation->args().size());
      for (Expr* arg : invocation->args()) {
        XLS_ASSIGN_OR_RETURN(
            Expr * cloned_arg,
            CloneExprIntoModule(arg, target_module, old_to_new));
        args.push_back(cloned_arg);
      }
      changed = true;
      return target_module->Make<SumInstance>(
          invocation->span(), cloned_constructor,
          SumInstance::PayloadKind::kTuple, std::move(args),
          std::vector<SumInstance::NamedArg>{}, invocation->in_parens());
    } else if (const auto* struct_instance =
                   dynamic_cast<const StructInstance*>(node)) {
      XLS_ASSIGN_OR_RETURN(
          std::optional<SumConstructorRef> constructor_ref,
          ResolveSumConstructor(struct_instance->struct_ref(), import_data));
      if (!constructor_ref.has_value()) {
        return std::nullopt;
      }
      XLS_ASSIGN_OR_RETURN(const ColonRef* constructor,
                           GetConstructorRef(struct_instance->struct_ref()));
      XLS_ASSIGN_OR_RETURN(
          ColonRef * cloned_constructor,
          CloneColonRefIntoModule(const_cast<ColonRef*>(constructor),
                                  target_module, old_to_new));
      std::vector<SumInstance::NamedArg> named_args;
      named_args.reserve(struct_instance->members().size());
      for (const auto& [name, arg] : struct_instance->members()) {
        XLS_ASSIGN_OR_RETURN(
            Expr * cloned_arg,
            CloneExprIntoModule(arg, target_module, old_to_new));
        named_args.push_back(std::make_pair(name, cloned_arg));
      }
      changed = true;
      return target_module->Make<SumInstance>(
          struct_instance->span(), cloned_constructor,
          SumInstance::PayloadKind::kStruct, std::vector<Expr*>{},
          std::move(named_args), struct_instance->in_parens());
    } else if (const auto* constructor =
                   dynamic_cast<const ColonRef*>(node)) {
      const AstNode* parent = constructor->parent();
      if ((dynamic_cast<const SumInstance*>(parent) != nullptr &&
           absl::down_cast<const SumInstance*>(parent)->constructor() ==
               constructor) ||
          IsPatternContext(constructor)) {
        return std::nullopt;
      }
      XLS_ASSIGN_OR_RETURN(
          std::optional<SumConstructorRef> constructor_ref,
          ResolveSumConstructor(constructor, import_data));
      if (!constructor_ref.has_value() ||
          !constructor_ref->variant->is_unit()) {
        return std::nullopt;
      }
      XLS_ASSIGN_OR_RETURN(
          ColonRef * cloned_constructor,
          CloneColonRefIntoModule(const_cast<ColonRef*>(constructor),
                                  target_module, old_to_new));
      changed = true;
      return target_module->Make<SumInstance>(
          constructor->span(), cloned_constructor,
          SumInstance::PayloadKind::kUnit, std::vector<Expr*>{},
          std::vector<SumInstance::NamedArg>{}, constructor->in_parens());
    } else {
      return std::nullopt;
    }
  };

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Module> cloned,
                       CloneModule(module, std::move(replacer)));
  XLS_RETURN_IF_ERROR(
      VerifyClone(&module, cloned.get(), *module.file_table()));
  if (!changed) {
    return std::nullopt;
  }
  while (true) {
    XLS_ASSIGN_OR_RETURN(
        std::optional<std::unique_ptr<Module>> nested_canonicalization,
        CanonicalizeSumInstances(*cloned, import_data));
    if (!nested_canonicalization.has_value()) {
      break;
    }
    cloned = std::move(*nested_canonicalization);
  }
  return std::make_optional(std::move(cloned));
}

}  // namespace xls::dslx
