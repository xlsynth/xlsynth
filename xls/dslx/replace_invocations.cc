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

#include "xls/dslx/replace_invocations.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/ret_check.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_cloner.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/typecheck_module.h"
#include "xls/dslx/warning_collector.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/bits.h"

namespace xls::dslx {

namespace {

bool MatchesCalleeEnv(const InvocationData& data,
                      const std::optional<ParametricEnv>& want_env) {
  if (!want_env.has_value()) {
    // No filter specified: match all.
    return true;
  }
  // If the callee had no parametric bindings, match only an explicitly empty
  // requested environment.
  if (data.env_to_callee_data().empty()) {
    return want_env->empty();
  }
  for (const auto& kv : data.env_to_callee_data()) {
    const InvocationCalleeData& callee_data = kv.second;
    if (callee_data.callee_bindings == *want_env) {
      return true;
    }
  }
  return false;
}

}  // namespace

absl::StatusOr<TypecheckedModule> ReplaceInvocationsInModule(
    const TypecheckedModule& tm, absl::Span<const Function* const> callers,
    absl::Span<const InvocationRewriteRule> rules, ImportData& import_data,
    std::string_view install_subject) {
  const Module& module = *tm.module;
  TypeInfo& type_info = *tm.type_info;
  XLS_RET_CHECK(!callers.empty());
  XLS_RET_CHECK(!rules.empty());
  for (const Function* f : callers) {
    XLS_RET_CHECK_NE(f, nullptr);
    XLS_RET_CHECK_EQ(f->owner(), &module);
  }
  for (const InvocationRewriteRule& r : rules) {
    XLS_RET_CHECK_NE(r.from_callee, nullptr);
    XLS_RET_CHECK_NE(r.to_callee, nullptr);
    XLS_RET_CHECK_EQ(r.from_callee->owner(), &module);
    XLS_RET_CHECK_EQ(r.to_callee->owner(), &module);
    // Validate match env shape if provided.
    if (r.match_callee_env.has_value()) {
      const ParametricEnv& me = *r.match_callee_env;
      const auto& pbs = r.from_callee->parametric_bindings();
      if (me.size() != static_cast<int64_t>(pbs.size())) {
        return absl::InvalidArgumentError(
            "match_callee_env arity does not match callee parametric arity");
      }
      for (int64_t i = 0; i < static_cast<int64_t>(pbs.size()); ++i) {
        if (me.bindings()[i].identifier != pbs[i]->identifier()) {
          return absl::InvalidArgumentError(
              "match_callee_env order or names do not match callee bindings");
        }
      }
    }
  }

  CloneReplacer replacer =
      [&type_info, callers,
       rules](const AstNode* node) -> absl::StatusOr<std::optional<AstNode*>> {
    const Invocation* inv = dynamic_cast<const Invocation*>(node);
    if (inv == nullptr) {
      return std::nullopt;
    }
    bool in_any_caller = false;
    for (const Function* c : callers) {
      if (ContainedWithinFunction(*inv, *c)) {
        in_any_caller = true;
        break;
      }
    }
    if (!in_any_caller) {
      return std::nullopt;
    }

    std::optional<const InvocationData*> data_opt =
        type_info.GetRootInvocationData(inv);
    if (!data_opt.has_value()) {
      return std::nullopt;
    }
    const InvocationData* data = *data_opt;

    const InvocationRewriteRule* matched_rule = nullptr;
    for (const InvocationRewriteRule& r : rules) {
      if (data->callee() != r.from_callee) {
        continue;
      }
      if (!MatchesCalleeEnv(*data, r.match_callee_env)) {
        continue;
      }
      matched_rule = &r;
      break;  // First match wins.
    }
    if (matched_rule == nullptr) {
      return std::nullopt;
    }

    const NameDef* target = matched_rule->to_callee->name_def();
    NameRef* new_callee = node->owner()->Make<NameRef>(
        inv->callee()->span(), target->identifier(), target,
        inv->callee()->in_parens());

    std::vector<Expr*> new_args;
    new_args.reserve(inv->args().size());
    for (Expr* arg : inv->args()) {
      new_args.push_back(arg);
    }

    std::vector<ExprOrType> new_parametrics;
    if (matched_rule->to_callee_env.has_value()) {
      if (!matched_rule->to_callee_env->empty()) {
        absl::flat_hash_map<std::string, InterpValue> env_map =
            matched_rule->to_callee_env->ToMap();

        for (const ParametricBinding* pb :
             matched_rule->to_callee->parametric_bindings()) {
          if (!pb->expr()) {
            if (!env_map.contains(pb->identifier())) {
              return absl::InvalidArgumentError(
                  absl::StrCat("Missing required binding `", pb->identifier(),
                               "` for replacement callee"));
            }
          }
        }

        for (const ParametricBinding* pb :
             matched_rule->to_callee->parametric_bindings()) {
          auto it = env_map.find(pb->identifier());
          if (it == env_map.end()) {
            continue;  // optional
          }
          const InterpValue& iv = it->second;
          TypeAnnotation* ann = pb->type_annotation();
          if (ann == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("Parametric binding `", pb->identifier(),
                             "` lacks a type annotation; explicit replacement "
                             "not supported"));
          }
          if (auto* bta = dynamic_cast<BuiltinTypeAnnotation*>(ann)) {
            if (!iv.IsBits()) {
              return absl::InvalidArgumentError(absl::StrCat(
                  "Parametric `", pb->identifier(),
                  "` expected bits value for builtin type ", bta->ToString()));
            }
            std::string digits = iv.ToString(/*humanize=*/true);
            BuiltinNameDef* bnd =
                node->owner()->GetOrCreateBuiltinNameDef(bta->builtin_type());
            BuiltinTypeAnnotation* typed =
                node->owner()->Make<BuiltinTypeAnnotation>(
                    inv->span(), bta->builtin_type(), bnd);
            Number* num = node->owner()->Make<Number>(inv->span(), digits,
                                                      NumberKind::kOther, typed,
                                                      /*in_parens=*/false);
            new_parametrics.push_back(static_cast<Expr*>(num));
            continue;
          }
          if (auto* trta = dynamic_cast<TypeRefTypeAnnotation*>(ann)) {
            TypeRef* tr = trta->type_ref();
            XLS_ASSIGN_OR_RETURN(
                TypeInfo::TypeSource ts,
                type_info.ResolveTypeDefinition(tr->type_definition()));
            if (!std::holds_alternative<EnumDef*>(ts.definition)) {
              return absl::InvalidArgumentError(
                  absl::StrCat("Unsupported type annotation for parametric `",
                               pb->identifier(), "` in explicit replacement"));
            }
            const EnumDef* enum_def = std::get<EnumDef*>(ts.definition);
            XLS_ASSIGN_OR_RETURN(Bits want_bits, iv.GetBits());
            std::optional<std::string> member_name;
            for (const EnumMember& em : enum_def->values()) {
              std::optional<InterpValue> mv =
                  type_info.GetConstExprOption(em.value);
              if (!mv.has_value()) {
                continue;
              }
              absl::StatusOr<Bits> mb = mv->GetBits();
              if (mb.ok() && *mb == want_bits) {
                member_name = em.name_def->identifier();
                break;
              }
            }
            if (!member_name.has_value()) {
              return absl::InvalidArgumentError(
                  absl::StrCat("Could not map enum value for parametric `",
                               pb->identifier(), "` to an enum member"));
            }
            const NameDef* enum_nd = enum_def->name_def();
            NameRef* enum_ref = node->owner()->Make<NameRef>(
                inv->span(), enum_nd->identifier(), enum_nd,
                /*in_parens=*/false);
            ColonRef* cref = node->owner()->Make<ColonRef>(
                inv->span(), ColonRef::Subject(enum_ref), *member_name,
                /*in_parens=*/false);
            new_parametrics.push_back(static_cast<Expr*>(cref));
            continue;
          }
          return absl::InvalidArgumentError(
              absl::StrCat("Unsupported parametric type annotation for `",
                           pb->identifier(), "` in explicit replacement"));
        }
      }
    } else {
      for (const ExprOrType& eot : inv->explicit_parametrics()) {
        new_parametrics.push_back(eot);
      }
    }

    Invocation* replacement = node->owner()->Make<Invocation>(
        inv->span(), new_callee, std::move(new_args),
        std::move(new_parametrics), inv->in_parens(),
        inv->originating_invocation());
    return replacement;
  };

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Module> cloned,
                       CloneModule(module, std::move(replacer)));

  // Re-parse the cloned text to ensure fresh binding to the new module's
  // definitions, then typecheck and install under the explicit subject.
  std::filesystem::path base_path = tm.module->fs_path().has_value()
                                        ? *tm.module->fs_path()
                                        : std::filesystem::path(module.name());
  std::filesystem::path new_path = base_path;

  std::string cloned_text = cloned->ToString();
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Module> reparsed,
                       ParseModule(cloned_text, new_path.string(),
                                   /*module_name=*/module.name(),
                                   import_data.file_table()));

  WarningCollector warnings(import_data.enabled_warnings());
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<ModuleInfo> module_info,
      TypecheckModule(std::move(reparsed), new_path, &import_data, &warnings));

  XLS_ASSIGN_OR_RETURN(ImportTokens subject,
                       ImportTokens::FromString(install_subject));
  XLS_ASSIGN_OR_RETURN(ModuleInfo * stored,
                       import_data.Put(subject, std::move(module_info)));

  return TypecheckedModule{
      .module = &stored->module(),
      .type_info = stored->type_info(),
      .warnings = std::move(warnings),
  };
}

absl::StatusOr<TypecheckedModule> ReplaceInvocationsInModule(
    const TypecheckedModule& tm, const Function* caller,
    const InvocationRewriteRule& rule, ImportData& import_data,
    std::string_view install_subject) {
  const Function* callers_arr[] = {caller};
  const InvocationRewriteRule rules_arr[] = {rule};
  return ReplaceInvocationsInModule(tm, absl::MakeSpan(callers_arr),
                                    absl::MakeSpan(rules_arr), import_data,
                                    install_subject);
}

}  // namespace xls::dslx
