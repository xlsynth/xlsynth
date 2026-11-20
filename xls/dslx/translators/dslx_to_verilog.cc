// Copyright 2024 The XLS Authors
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

#include "xls/dslx/translators/dslx_to_verilog.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/variant.h"
#include "xls/codegen/vast/vast.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/translators/verilog_sum_naming.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/bits.h"
#include "xls/ir/name_uniquer.h"
#include "xls/ir/source_location.h"

namespace xls::dslx {

namespace {

using verilog_sum::EnumCompanionKey;
using verilog_sum::EscapeName;
using verilog_sum::FamilyNameRequests;
using verilog_sum::MemberName;
using verilog_sum::SignedArrayCompanionKey;
using verilog_sum::SourceName;
using verilog_sum::StructCompanionKey;
using verilog_sum::VariantSuffixes;

const Type* UnboxMetaTypes(const Type* type) {
  while (type->IsMeta()) {
    type = type->AsMeta().wrapped().get();
  }
  return type;
}

// Obtains the concrete Type from an AstNode.
absl::StatusOr<Type*> GetActualType(const AstNode* node, TypeInfo* type_info,
                                    ImportData* import_data) {
  std::optional<Type*> type = type_info->GetItem(node);

  if (!type.has_value()) {
    return absl::InternalError(
        absl::StrFormat("Unable to locate concrete type for param %s in %s",
                        node->ToInlineString(),
                        node->GetSpan()
                            .value_or(FakeSpan())
                            .ToString(import_data->file_table())));
  }

  XLS_RET_CHECK(type.value()->IsMeta());
  return type.value()->AsMeta().wrapped().get();
}

std::optional<AstNode*> GetTypeDefinition(
    const TypeAnnotation* type_annotation) {
  if (auto* ta = dynamic_cast<const TypeRefTypeAnnotation*>(type_annotation)) {
    return TypeDefinitionToAstNode(ta->type_ref()->type_definition());
  }
  return std::nullopt;
}

// Obtain a typedef identifier for the given TypeAnnotation, named
//  1. <dslx_type_name> for DSLX type references.
//  2. <function_name>_<parameter_name>_t for anonymous types (everything else).
std::string GetVerilogTypedefIdentifier(const TypeAnnotation* type_annotation,
                                        std::string_view function_name,
                                        std::string_view param_name) {
  if (dynamic_cast<const TypeRefTypeAnnotation*>(type_annotation)) {
    return type_annotation->ToString();
  }

  return absl::StrCat(function_name, "_", param_name, "_t");
}

// For type definitions, returns the name given to the type.
// Note: unlike nominal type name which uses deduced types that chases through
// aliases, this returns the name of a specific type definition, so you might
// get the name of an alias of an otherwise unnamed type.
std::optional<std::string_view> TypeDefinitionIdentifier(
    const TypeInfo::TypeSource& resolved_type_definition) {
  return absl::visit(
      Visitor{
          [](TypeAlias* alias) -> std::optional<std::string_view> {
            return alias->name_def().identifier();
          },
          [](StructDef* struct_def) -> std::optional<std::string_view> {
            return struct_def->name_def()->identifier();
          },
          [](ProcDef* proc_def) -> std::optional<std::string_view> {
            return proc_def->name_def()->identifier();
          },
          [](EnumDef* enum_def) -> std::optional<std::string_view> {
            return enum_def->name_def()->identifier();
          },
          [](SumDef* sum_def) -> std::optional<std::string_view> {
            return sum_def->name_def()->identifier();
          },
      },
      resolved_type_definition.definition);
}

std::string TypeDefinitionName(const TypeDefinition& definition) {
  return absl::visit([](const auto* name) { return name->identifier(); },
                     TypeDefinitionGetNameDef(definition));
}

bool IsOrdinarySourceTypeName(const AstNode* node,
                              std::optional<std::string_view> declared,
                              std::string_view requested) {
  return declared.has_value() && requested == *declared &&
         (dynamic_cast<const EnumDef*>(node) != nullptr ||
          dynamic_cast<const StructDef*>(node) != nullptr);
}

// Returns the packed width of a concrete type; unresolved dimensions fail.
absl::StatusOr<int64_t> BitCount(const Type& type) {
  XLS_ASSIGN_OR_RETURN(TypeDim bits, type.GetTotalBitCount());
  return bits.GetAsInt64();
}

// Named ordinary dependencies can fail against explicit sum aliases during
// conversion. A sum owns its dependencies and checks them when first emitted.
// A repeated ordinary export reserves a uniquifier name even when it emits no
// declaration. Explicit sum aliases and canonical payload type names can
// reclaim that reservation because they must use exactly the requested
// spelling.
std::string ClaimVisibleName(std::string_view identifier, NameUniquer& uniquer,
                             std::set<std::string>& ephemeral) {
  if (ephemeral.erase(std::string(identifier)) != 0) {
    CHECK_OK(uniquer.ReleaseIdentifier(identifier));
  }
  return uniquer.GetSanitizedUniqueName(identifier);
}

std::string AllocateSumName(std::string_view identifier, NameUniquer& uniquer,
                            const std::set<std::string>& ordinary_source_names,
                            std::set<std::string>& allocated) {
  std::string base = verilog::SanitizeVerilogIdentifier(identifier);
  std::string candidate = base;
  int64_t suffix = 0;
  while (true) {
    if (!ordinary_source_names.contains(candidate) &&
        !allocated.contains(candidate)) {
      // Generated names follow the legacy allocation sequence, including names
      // reserved by repeated ordinary exports that emitted no declaration.
      std::string unique = uniquer.GetSanitizedUniqueName(candidate);
      if (!ordinary_source_names.contains(unique) &&
          !allocated.contains(unique)) {
        allocated.insert(unique);
        return unique;
      } else {
        CHECK_OK(uniquer.ReleaseIdentifier(unique));
      }
    }
    candidate = absl::StrCat(base, "__", ++suffix);
  }
}

std::string AllocateOrdinaryTypeName(std::string_view identifier,
                                     bool already_converted,
                                     NameUniquer& uniquer,
                                     std::set<std::string>& allocated,
                                     std::set<std::string>& ephemeral) {
  std::string name = uniquer.GetSanitizedUniqueName(identifier);
  if (already_converted) {
    ephemeral.insert(name);
  } else {
    allocated.insert(name);
  }
  return name;
}

absl::Status OrdinaryTypeNameConflict(std::string_view name) {
  return absl::InvalidArgumentError(absl::StrFormat(
      "SystemVerilog type `%s` conflicts with an existing package symbol",
      name));
}

// Reports whether declaration-based export would lose a payload's signedness
// or concrete record specialization. Zero-width aggregate fields are ignored.
absl::StatusOr<bool> NeedsSemanticProjection(const Type& type) {
  if (std::optional<BitsLikeProperties> bits = GetBitsLike(type);
      bits.has_value()) {
    return bits->is_signed.GetAsBool();
  } else if (type.IsEnum()) {
    return type.AsEnum().is_signed();
  } else if (type.IsArray()) {
    return NeedsSemanticProjection(type.AsArray().element_type());
  } else if (type.IsStruct() &&
             !type.AsStruct().nominal_type().parametric_bindings().empty()) {
    return true;
  } else if (type.IsStruct() || type.IsTuple()) {
    int64_t count =
        type.IsStruct() ? type.AsStruct().size() : type.AsTuple().size();
    for (int64_t i = 0; i < count; ++i) {
      const Type& member = type.IsStruct() ? type.AsStruct().GetMemberType(i)
                                           : type.AsTuple().GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(member));
      if (width > 0) {
        XLS_ASSIGN_OR_RETURN(bool needs_projection,
                             NeedsSemanticProjection(member));
        if (needs_projection) {
          return true;
        }
      }
    }
  }
  // Nested sums already use their own canonical family in either export mode.
  return false;
}

// Only fixed, unsigned records can reuse the ordinary declaration unchanged in
// a sum view. Other records use a family-owned semantic companion instead.
absl::StatusOr<bool> UsesOrdinaryStructInSum(const StructType& record) {
  XLS_ASSIGN_OR_RETURN(bool needs_projection, NeedsSemanticProjection(record));
  return !needs_projection;
}

absl::Status SumAliasConflict(std::string_view alias,
                              std::string_view family_name) {
  return absl::InvalidArgumentError(absl::StrFormat(
      "SystemVerilog alias `%s` for sum family `%s` conflicts with an "
      "existing package symbol",
      alias, family_name));
}

struct OrdinaryEnumMember {
  const EnumDef* definition;
  int64_t index;
};

using OrdinaryEnumMemberGroups =
    std::map<std::string, std::vector<OrdinaryEnumMember>>;

void AddOrdinaryEnumMembers(OrdinaryEnumMemberGroups& groups,
                            const EnumDef& definition, bool projected) {
  for (int64_t i = 0; i < definition.values().size(); ++i) {
    const std::string& original = definition.GetMemberName(i);
    groups[projected ? verilog::SanitizeVerilogIdentifier(original) : original]
        .push_back({&definition, i});
  }
}

// Uses the same source ordering and disambiguation for the potential ordinary
// enum names reserved before generating sums and the names actually emitted.
// Each caller supplies the declarations and occupied package names for its own
// scope: a private, un-emitted enum cannot qualify an emitted enum's literals.
template <typename GetStem, typename IsOccupied>
std::vector<std::pair<OrdinaryEnumMember, std::string>>
AllocateOrdinaryEnumNames(OrdinaryEnumMemberGroups& groups, GetStem stem,
                          IsOccupied is_occupied) {
  std::set<std::string> occupied;
  std::set<std::string> preserved;
  for (const auto& [name, members] : groups) {
    if (members.size() == 1 && !is_occupied(name)) {
      occupied.insert(name);
      preserved.insert(name);
    }
  }
  std::vector<std::pair<OrdinaryEnumMember, std::string>> result;
  for (auto& [name, members] : groups) {
    std::sort(members.begin(), members.end(),
              [](const OrdinaryEnumMember& a, const OrdinaryEnumMember& b) {
                return std::make_pair(SourceName(*a.definition,
                                                 a.definition->identifier()),
                                      a.definition->GetMemberName(a.index)) <
                       std::make_pair(SourceName(*b.definition,
                                                 b.definition->identifier()),
                                      b.definition->GetMemberName(b.index));
              });
    for (const OrdinaryEnumMember& member : members) {
      std::string unique = name;
      if (!preserved.contains(name)) {
        std::string base = verilog::SanitizeVerilogIdentifier(
            absl::StrCat(stem(member.definition), "_",
                         member.definition->GetMemberName(member.index)));
        unique = base;
        for (int64_t suffix = 1;
             occupied.contains(unique) || is_occupied(unique); ++suffix) {
          unique = absl::StrCat(base, "__", suffix);
        }
        occupied.insert(unique);
      }
      result.emplace_back(member, std::move(unique));
    }
  }
  return result;
}

enum class AggregateNamePolicy { kLegacy, kSumPayload };

enum class TypeReferenceScope { kPackedMember, kFunctionFormal };

using TypeUsePositions = std::map<std::string, int64_t>;

// Records the unqualified typedef references a prior declaration can hide.
// A packed member cannot hide references inside an anonymous nested struct;
// a function formal can hide those references in a later formal's type.
// An array does not introduce a scope, and a typedef prints only its own name.
void RecordTypeUsePositions(const verilog::DataType* type,
                            TypeReferenceScope scope, int64_t position,
                            TypeUsePositions& positions) {
  if (auto* reference = dynamic_cast<const verilog::TypedefType*>(type)) {
    positions[reference->type_def()->GetName()] = position;
  } else if (auto* array = dynamic_cast<const verilog::ArrayTypeBase*>(type)) {
    RecordTypeUsePositions(array->element_type(), scope, position, positions);
  } else if (auto* record = dynamic_cast<const verilog::Struct*>(type);
             record != nullptr &&
             scope == TypeReferenceScope::kFunctionFormal) {
    for (const verilog::Def* field : record->members()) {
      RecordTypeUsePositions(field->data_type(), scope, position, positions);
    }
  }
}

// A member enters scope after its type is parsed and can hide references only
// in later declarations. Keeping just the last position per referenced name
// avoids storing a growing copy of the following names for each field.
TypeUsePositions LastTypeUsePositions(
    absl::Span<const std::pair<std::string, verilog::DataType*>> fields,
    TypeReferenceScope scope) {
  TypeUsePositions positions;
  for (int64_t i = 0; i < fields.size(); ++i) {
    RecordTypeUsePositions(fields[i].second, scope, i, positions);
  }
  return positions;
}

bool IsTypeUsedAfter(const TypeUsePositions& positions, const std::string& name,
                     int64_t position) {
  auto it = positions.find(name);
  return it != positions.end() && it->second > position;
}

// Aggregates used by a sum preserve usable source names, then allocate names
// for the remaining fields in source-name order without changing member or bit
// order.
std::map<std::string, std::string> AggregateMemberNames(
    absl::Span<const std::pair<std::string, verilog::DataType*>> fields) {
  std::map<std::string, std::string> identifiers;
  const auto last_type_uses =
      LastTypeUsePositions(fields, TypeReferenceScope::kPackedMember);
  std::map<std::string, int64_t> field_positions;
  for (int64_t i = 0; i < fields.size(); ++i) {
    field_positions.emplace(fields[i].first, i);
  }
  std::set<std::string> unchanged;
  for (const auto& [source, position] : field_positions) {
    if (verilog::SanitizeVerilogIdentifier(source) == source &&
        !IsTypeUsedAfter(last_type_uses, source, position)) {
      unchanged.insert(source);
    }
  }
  NameUniquer names("__");
  for (const auto& [source, position] : field_positions) {
    std::string name = source;
    if (!unchanged.contains(source)) {
      do {
        name = MemberName(names, source);
      } while (unchanged.contains(name) ||
               IsTypeUsedAfter(last_type_uses, name, position));
    }
    identifiers.emplace(source, std::move(name));
  }
  return identifiers;
}

// Both projections preserve aggregate declaration order, omit zero-width
// children, and retain original tuple indexes. The ordinary projection
// preserves historical field spelling unless this nominal is used by a
// translated sum.
template <typename Aggregate, typename Name, typename Convert, typename Make>
absl::StatusOr<std::vector<verilog::Def*>> AggregateMembers(
    const Aggregate& aggregate, AggregateNamePolicy policy, Name name,
    Convert convert, Make make) {
  std::vector<std::pair<std::string, verilog::DataType*>> visible;
  for (int64_t i = 0; i < aggregate.size(); ++i) {
    const Type& member = aggregate.GetMemberType(i);
    XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(member));
    if (width > 0) {
      XLS_ASSIGN_OR_RETURN(verilog::DataType * type, convert(i, member));
      visible.emplace_back(name(i), type);
    }
  }
  const auto identifiers = policy == AggregateNamePolicy::kSumPayload
                               ? AggregateMemberNames(visible)
                               : std::map<std::string, std::string>{};
  std::vector<verilog::Def*> members;
  for (const auto& [source, type] : visible) {
    members.push_back(make(policy == AggregateNamePolicy::kSumPayload
                               ? identifiers.at(source)
                               : source,
                           type));
  }
  return members;
}

}  // namespace

absl::StatusOr<std::string> ExportDslxTypesToVerilog(
    std::string_view package_name, ImportData* import_data,
    absl::Span<const VerilogExportRequest> requests,
    const VerilogExportOptions& options) {
  struct ResolvedExport {
    Type* type;
    std::variant<TypeDefinition, TypeAnnotation*> source;
    std::string name;
    bool sum_alias;
    Module* module;
    TypeInfo* type_info;
  };
  DslxTypeToVerilogManager manager(package_name);
  if (!options.public_nominal_owners.empty()) {
    manager.sum_identities_.SetPublicNominalOwner(
        [owners = options.public_nominal_owners](
            const AstNode& node) -> std::string_view {
          auto entry = owners.find(&node);
          return entry == owners.end() ? node.owner()->name() : entry->second;
        });
  }
  std::vector<ResolvedExport> resolved;
  resolved.reserve(requests.size());
  for (const VerilogExportRequest& request : requests) {
    auto resolve_function =
        [&](Function* function, TypeAnnotation* annotation,
            std::string_view suffix) -> absl::StatusOr<ResolvedExport> {
      if (!function->parametric_bindings().empty()) {
        return absl::UnimplementedError(absl::StrFormat(
            "Unable to convert function %s with parametric bindings",
            function->identifier()));
      }
      XLS_ASSIGN_OR_RETURN(TypeInfo * info,
                           import_data->GetRootTypeInfoForNode(function));
      XLS_ASSIGN_OR_RETURN(Type * type,
                           GetActualType(annotation, info, import_data));
      std::string name = request.verilog_type_name.has_value()
                             ? *request.verilog_type_name
                             : GetVerilogTypedefIdentifier(
                                   annotation, function->identifier(), suffix);
      std::optional<AstNode*> definition = GetTypeDefinition(annotation);
      bool source_alias = definition.has_value() &&
                          dynamic_cast<TypeAlias*>(*definition) != nullptr;
      bool alias = UnboxMetaTypes(type)->IsSum() &&
                   (request.verilog_type_name.has_value() || source_alias ||
                    name != annotation->ToString());
      return ResolvedExport{type,  annotation,        std::move(name),
                            alias, function->owner(), info};
    };
    XLS_ASSIGN_OR_RETURN(
        ResolvedExport item,
        absl::visit(
            Visitor{
                [&](const TypeDefinition& definition)
                    -> absl::StatusOr<ResolvedExport> {
                  AstNode* node = TypeDefinitionToAstNode(definition);
                  XLS_ASSIGN_OR_RETURN(
                      TypeInfo * info,
                      import_data->GetRootTypeInfoForNode(node));
                  XLS_ASSIGN_OR_RETURN(Type * type,
                                       GetActualType(node, info, import_data));
                  std::string name = request.verilog_type_name.value_or("");
                  if (name.empty() && (!request.verilog_type_name.has_value() ||
                                       !UnboxMetaTypes(type)->IsSum())) {
                    name = TypeDefinitionName(definition);
                  }
                  bool alias = false;
                  if (UnboxMetaTypes(type)->IsSum()) {
                    XLS_ASSIGN_OR_RETURN(
                        const TypeInfo::TypeSource source,
                        info->ResolveTypeDefinition(definition));
                    alias =
                        request.verilog_type_name.has_value() ||
                        !std::holds_alternative<SumDef*>(source.definition) ||
                        name != UnboxMetaTypes(type)
                                    ->AsSum()
                                    .nominal_type()
                                    .identifier();
                  }
                  return ResolvedExport{type,  definition,    std::move(name),
                                        alias, node->owner(), info};
                },
                [&](const VerilogFunctionParamExport& param)
                    -> absl::StatusOr<ResolvedExport> {
                  XLS_ASSIGN_OR_RETURN(
                      Param * p,
                      param.function->GetParamByName(param.param_name));
                  return resolve_function(param.function, p->type_annotation(),
                                          p->identifier());
                },
                [&](const VerilogFunctionOutputExport& output)
                    -> absl::StatusOr<ResolvedExport> {
                  if (output.function->return_type() == nullptr) {
                    return absl::UnimplementedError(
                        "Zero sized interface type () not supported.");
                  }
                  return resolve_function(
                      output.function, output.function->return_type(), "out");
                }},
            request.source));
    resolved.push_back(std::move(item));
  }
  manager.PrepareForModules(options.roots);
  for (const ResolvedExport& item : resolved) {
    const std::pair<Module*, TypeInfo*> root = {item.module, item.type_info};
    manager.PrepareForModules(absl::MakeConstSpan(&root, 1));
    if (const auto* definition = std::get_if<TypeDefinition>(&item.source)) {
      XLS_RETURN_IF_ERROR(manager.AddTypeToVerilogPackage(
          item.type, *definition, import_data, item.name, item.sum_alias));
    } else {
      XLS_RETURN_IF_ERROR(manager.AddTypeToVerilogPackage(
          item.type, std::get<TypeAnnotation*>(item.source), import_data,
          item.name, item.sum_alias));
    }
  }
  return manager.file_->Emit();
}

void DslxTypeToVerilogManager::PrepareForModules(
    absl::Span<const std::pair<Module*, TypeInfo*>> modules) {
  std::map<std::string, std::vector<AstNode*>> definitions;
  std::map<std::string, std::set<std::string>> module_qualifiers;
  std::vector<SumDef*> sums;
  std::map<const SumDef*, const SumType*> concrete_sums;
  OrdinaryEnumMemberGroups enum_members;
  std::set<const Module*> visited;
  std::vector<std::pair<Module*, TypeInfo*>> pending(modules.begin(),
                                                     modules.end());
  while (!pending.empty()) {
    auto [current, current_info] = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second ||
        prepared_sum_modules_.contains(current)) {
      continue;
    }
    prepared_sum_modules_.insert(current);
    module_qualifiers[verilog::SanitizeVerilogIdentifier(current->name())]
        .insert(current->name());
    for (const TypeDefinition& definition : current->GetTypeDefinitions()) {
      AstNode* node = TypeDefinitionToAstNode(definition);
      AnyNameDef name = TypeDefinitionGetNameDef(definition);
      std::string identifier = absl::visit(
          [](const auto* value) { return value->identifier(); }, name);
      std::string sanitized = verilog::SanitizeVerilogIdentifier(identifier);
      definitions[sanitized].push_back(node);
      if (auto* sum = dynamic_cast<SumDef*>(node)) {
        sums.push_back(sum);
        std::optional<Type*> type = current_info->GetItem(sum);
        if (type.has_value() && UnboxMetaTypes(*type)->IsSum()) {
          concrete_sums.emplace(sum, &UnboxMetaTypes(*type)->AsSum());
        }
      } else {
        legacy_name_owners_[sanitized].push_back(node);
        // Function signatures infer this unqualified source spelling even
        // when nominal declarations in multiple modules share the name.
        legacy_package_names_.insert(sanitized);
      }
      if (auto* ordinary_enum = dynamic_cast<EnumDef*>(node)) {
        AddOrdinaryEnumMembers(enum_members, *ordinary_enum,
                               /*projected=*/true);
      }
    }
    for (const auto& [subject, imported] : current_info->GetRootImports()) {
      pending.emplace_back(imported.module, imported.type_info);
    }
  }
  for (auto& [name, nodes] : definitions) {
    std::sort(nodes.begin(), nodes.end(),
              [](const AstNode* a, const AstNode* b) {
                return a->owner()->name() < b->owner()->name();
              });
    for (AstNode* node : nodes) {
      std::string module =
          verilog::SanitizeVerilogIdentifier(node->owner()->name());
      if (module_qualifiers.at(module).size() != 1) {
        module = EscapeName(node->owner()->name());
      }
      std::string nominal = nodes.size() == 1
                                ? name
                                : verilog::SanitizeVerilogIdentifier(
                                      absl::StrCat(module, "_", name));
      nominal_names_.emplace(node, nominal);
      if (dynamic_cast<SumDef*>(node) == nullptr) {
        legacy_package_names_.insert(nominal);
      }
    }
  }

  // Keep generated sum names clear of the possible ordinary enum spellings.
  // This does not decide which enums occupy the package; only actually emitted
  // ordinary declarations participate in UpdateOrdinaryEnumNames().
  auto potential_names = AllocateOrdinaryEnumNames(
      enum_members,
      [&](const EnumDef* definition) -> const std::string& {
        return nominal_names_.at(definition);
      },
      [&](const std::string& name) {
        return legacy_package_names_.contains(name);
      });
  for (const auto& [member, name] : potential_names) {
    legacy_package_names_.insert(name);
  }
  for (const auto& [name, members] : enum_members) {
    for (const auto& [definition, index] : members) {
      const std::string& original = definition->GetMemberName(index);
      legacy_package_names_.insert(original);
      legacy_package_names_.insert(name);
      legacy_package_names_.insert(verilog::SanitizeVerilogIdentifier(
          absl::StrCat(nominal_names_.at(definition), "_", original)));
      legacy_package_names_.insert(verilog::SanitizeVerilogIdentifier(
          absl::StrCat(definition->identifier(), "_", original)));
    }
  }

  std::sort(sums.begin(), sums.end(), [](const SumDef* a, const SumDef* b) {
    return SourceName(*a, a->identifier()) < SourceName(*b, b->identifier());
  });
  for (SumDef* sum : sums) {
    nominal_sum_names_.emplace(sum, NewSumName(nominal_names_.at(sum)));
  }
  // Reserve every nonparametric family's public names in source identity order
  // before emitting any family. For example, A's getter and A_get's constructor
  // cannot exchange names when the export requests are reversed.
  for (SumDef* sum : sums) {
    if (sum->parametric_bindings().empty()) {
      auto type = concrete_sums.find(sum);
      bool has_payload = true;
      if (type != concrete_sums.end()) {
        auto payload_width = type->second->GetMaxPayloadBitCount();
        if (payload_width.ok()) {
          auto width = payload_width->GetAsInt64();
          if (width.ok()) {
            has_payload = *width > 0;
          }
        }
      }
      auto requests =
          FamilyNameRequests(*sum, nominal_sum_names_.at(sum), has_payload);
      if (type != concrete_sums.end()) {
        auto companions =
            CompanionNameRequests(*type->second, nominal_sum_names_.at(sum));
        if (companions.ok()) {
          requests.merge(*companions);
        }
      }
      for (auto& [key, name] : requests) {
        name = NewSumName(name);
      }
      nominal_sum_symbols_.emplace(sum, std::move(requests));
    }
  }
}

absl::Status DslxTypeToVerilogManager::RegisterOrdinaryEnum(
    const EnumDef& definition, bool projected) {
  auto current = ordinary_enum_projections_.find(&definition);
  if (current != ordinary_enum_projections_.end() &&
      (current->second || !projected)) {
    return absl::OkStatus();
  } else if (!projected) {
    std::vector<std::string> names;
    bool affects_projection = false;
    for (int64_t i = 0; i < definition.values().size(); ++i) {
      const std::string& name = definition.GetMemberName(i);
      XLS_RETURN_IF_ERROR(CheckOrdinaryNameAgainstSumAliases(name));
      affects_projection = affects_projection ||
                           projected_ordinary_enum_member_names_.contains(name);
      names.push_back(name);
    }
    if (!affects_projection) {
      // New standalone enums do not change earlier standalone spellings. A
      // package-wide rebuild is needed only if a sum already reuses that name.
      for (const std::string& name : names) {
        legacy_name_owners_[name].push_back(&definition);
      }
      ordinary_enum_projections_.emplace(&definition, false);
      legacy_enum_member_names_.emplace(&definition, std::move(names));
      return absl::OkStatus();
    }
  }
  auto next = ordinary_enum_projections_;
  next[&definition] = projected;
  return UpdateOrdinaryEnumNames(next);
}

std::vector<DslxTypeToVerilogManager::EmittedEnumMember>
DslxTypeToVerilogManager::ProjectSumEnumValues(
    verilog::Enum* enumeration, verilog::DataType* named,
    verilog::VerilogPackageSection* aliases) {
  absl::flat_hash_map<Bits, verilog::EnumMember*> first_members;
  std::vector<verilog::EnumMember*> native_members;
  std::vector<EmittedEnumMember> source_members;
  for (verilog::EnumMember* member : enumeration->members()) {
    const Bits& bits = member->rhs()->AsLiteralOrDie()->bits();
    auto [first, inserted] = first_members.emplace(bits, member);
    if (inserted) {
      native_members.push_back(member);
      source_members.emplace_back(member);
    } else {
      // A package parameter is implicitly local. Refer to the native member so
      // this stays enum-typed and follows any later package collision rename.
      verilog::EnumMemberRef* reference = file_->Make<verilog::EnumMemberRef>(
          member->loc(), enumeration, first->second);
      source_members.emplace_back(
          aliases
              ->AddParameter(MakeMember(member->GetName(), named), reference,
                             member->loc())
              ->parameter());
    }
  }
  if (native_members.size() != enumeration->members().size()) {
    // Keep the enum and surviving member objects: their existing references
    // must continue to refer to this exact nominal SystemVerilog type.
    *enumeration =
        verilog::Enum(enumeration->kind(), enumeration->BaseType(),
                      native_members, file_.get(), enumeration->loc());
  }
  return source_members;
}

void DslxTypeToVerilogManager::LegalizeOrdinaryEnumValues(
    const EnumDef& definition) {
  auto existing = ordinary_enum_emissions_.find(&definition);
  if (existing != ordinary_enum_emissions_.end() &&
      !existing->second.values_projected) {
    OrdinaryEnumEmission& emission = existing->second;
    emission.members = ProjectSumEnumValues(emission.enumeration,
                                            emission.named, emission.aliases);
    emission.values_projected = true;
  }
}

absl::StatusOr<DslxTypeToVerilogManager::OrdinaryEnumNames>
DslxTypeToVerilogManager::PlanOrdinaryEnumNames(
    const absl::flat_hash_map<const EnumDef*, bool>& projections,
    const std::set<std::string>& ordinary_names,
    const std::set<std::string>& sum_names) const {
  OrdinaryEnumNames result;
  OrdinaryEnumMemberGroups groups;
  std::map<std::string, std::vector<const EnumDef*>> definitions;
  std::map<std::string, std::set<std::string>> module_qualifiers;
  std::set<std::string> unprojected_names;
  std::vector<std::pair<const EnumDef*, bool>> ordered(projections.begin(),
                                                       projections.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    return SourceName(*a.first, a.first->identifier()) <
           SourceName(*b.first, b.first->identifier());
  });
  for (const auto& [definition, projected] : ordered) {
    auto& names = result.members[definition];
    names.resize(definition->values().size());
    if (projected) {
      definitions[verilog::SanitizeVerilogIdentifier(definition->identifier())]
          .push_back(definition);
      module_qualifiers[verilog::SanitizeVerilogIdentifier(
                            definition->owner()->name())]
          .insert(definition->owner()->name());
      AddOrdinaryEnumMembers(groups, *definition, /*projected=*/true);
    } else {
      // Only enum types reused as sum views need legal, unique names. Preserve
      // the existing standalone spelling, including collisions between ordinary
      // declarations, and make projected names avoid it.
      for (int64_t i = 0; i < definition->values().size(); ++i) {
        const std::string& name = definition->GetMemberName(i);
        XLS_RETURN_IF_ERROR(CheckOrdinaryNameAgainstSumAliases(name));
        names[i] = name;
        unprojected_names.insert(name);
      }
    }
  }
  absl::flat_hash_map<const EnumDef*, std::string> qualification_stems;
  for (const auto& [name, owners] : definitions) {
    for (const EnumDef* owner : owners) {
      std::string module =
          verilog::SanitizeVerilogIdentifier(owner->owner()->name());
      if (module_qualifiers.at(module).size() != 1) {
        module = EscapeName(owner->owner()->name());
      }
      qualification_stems.emplace(
          owner, owners.size() == 1 ? name
                                    : verilog::SanitizeVerilogIdentifier(
                                          absl::StrCat(module, "_", name)));
    }
  }
  auto allocated = AllocateOrdinaryEnumNames(
      groups,
      [&](const EnumDef* definition) -> const std::string& {
        return qualification_stems.at(definition);
      },
      [&](const std::string& name) {
        return unprojected_names.contains(name) ||
               ordinary_names.contains(name) || sum_names.contains(name);
      });
  for (auto& [member, name] : allocated) {
    // Explicit aliases reject conflicting later exports instead of silently
    // displacing their ordinary enum member. Canonical generated symbols, in
    // contrast, are already included in the allocator's occupied names above.
    XLS_RETURN_IF_ERROR(CheckOrdinaryNameAgainstSumAliases(name));
    result.projected.insert(name);
    result.members.at(member.definition)[member.index] = std::move(name);
  }
  return result;
}

absl::Status DslxTypeToVerilogManager::UpdateOrdinaryEnumNames(
    const absl::flat_hash_map<const EnumDef*, bool>& projections) {
  if (projections.empty()) {
    return absl::OkStatus();
  }
  XLS_ASSIGN_OR_RETURN(
      OrdinaryEnumNames names,
      PlanOrdinaryEnumNames(projections, emitted_ordinary_type_names_,
                            emitted_sum_names_));

  // Remove only the ownership entries previously added for actual literals;
  // an identically named typedef can have a separate entry for the same node.
  for (const auto& [definition, names] : legacy_enum_member_names_) {
    for (const std::string& name : names) {
      auto owner = legacy_name_owners_.find(name);
      XLS_RET_CHECK(owner != legacy_name_owners_.end());
      auto entry =
          std::find(owner->second.begin(), owner->second.end(), definition);
      XLS_RET_CHECK(entry != owner->second.end());
      owner->second.erase(entry);
      if (owner->second.empty()) {
        legacy_name_owners_.erase(owner);
      }
    }
  }
  for (const auto& [definition, planned_members] : names.members) {
    for (const std::string& name : planned_members) {
      legacy_name_owners_[name].push_back(definition);
    }
    if (projections.at(definition)) {
      LegalizeOrdinaryEnumValues(*definition);
    }
    auto existing = ordinary_enum_emissions_.find(definition);
    if (existing != ordinary_enum_emissions_.end()) {
      const std::vector<EmittedEnumMember>& emitted_members =
          existing->second.members;
      XLS_RET_CHECK_EQ(emitted_members.size(), planned_members.size());
      for (int64_t i = 0; i < planned_members.size(); ++i) {
        if (auto* native =
                std::get_if<verilog::EnumMember*>(&emitted_members[i])) {
          verilog::EnumMember* member = *native;
          if (member->GetName() != planned_members[i]) {
            *member = verilog::EnumMember(planned_members[i], member->rhs(),
                                          file_.get(), member->loc());
          }
        } else {
          auto* alias = std::get<verilog::Parameter*>(emitted_members[i]);
          if (alias->GetName() != planned_members[i]) {
            *alias = verilog::Parameter(
                MakeMember(planned_members[i], alias->def()->data_type()),
                alias->rhs(), file_.get(), alias->loc());
          }
        }
      }
    }
  }
  ordinary_enum_projections_ = projections;
  legacy_enum_member_names_ = std::move(names.members);
  projected_ordinary_enum_member_names_ = std::move(names.projected);
  return absl::OkStatus();
}

bool DslxTypeToVerilogManager::IsOrdinaryEnumMemberName(
    std::string_view name, const AstNode* excluded_owner) const {
  if (ordinary_enum_projections_.empty()) {
    return false;
  } else {
    auto owners = legacy_name_owners_.find(name);
    return owners != legacy_name_owners_.end() &&
           std::any_of(
               owners->second.begin(), owners->second.end(),
               [&](const AstNode* owner) {
                 const auto* definition = dynamic_cast<const EnumDef*>(owner);
                 auto names = legacy_enum_member_names_.find(definition);
                 return owner != excluded_owner &&
                        names != legacy_enum_member_names_.end() &&
                        std::find(names->second.begin(), names->second.end(),
                                  name) != names->second.end();
               });
  }
}

absl::Status DslxTypeToVerilogManager::CheckOrdinaryNameAgainstSumAliases(
    std::string_view name) const {
  auto alias = sum_aliases_.find(name);
  if (alias == sum_aliases_.end()) {
    return absl::OkStatus();
  } else {
    auto* declaration = static_cast<verilog::TypedefType*>(alias->second);
    auto* canonical = static_cast<verilog::TypedefType*>(
        declaration->type_def()->data_type());
    return SumAliasConflict(name, canonical->type_def()->GetName());
  }
}

std::string DslxTypeToVerilogManager::NominalName(
    const AstNode& node, std::string_view identifier) const {
  auto known = nominal_names_.find(&node);
  return known == nominal_names_.end()
             ? verilog::SanitizeVerilogIdentifier(identifier)
             : known->second;
}

absl::StatusOr<std::string> DslxTypeToVerilogManager::SumPayloadNominalName(
    const AstNode& nominal) const {
  const auto* record = dynamic_cast<const StructDef*>(&nominal);
  const auto* enumeration = dynamic_cast<const EnumDef*>(&nominal);
  XLS_RET_CHECK(record != nullptr || enumeration != nullptr);
  return NominalName(nominal, record != nullptr ? record->identifier()
                                                : enumeration->identifier());
}

std::string DslxTypeToVerilogManager::OrdinaryTypeNameCandidate(
    const AstNode& node, std::string_view identifier, bool is_sum_payload,
    bool use_nominal_name) const {
  if (is_sum_payload && use_nominal_name) {
    return NominalName(node, identifier);
  } else if (is_sum_payload) {
    return verilog::SanitizeVerilogIdentifier(identifier);
  } else {
    return std::string(identifier);
  }
}

absl::StatusOr<std::optional<std::string>>
DslxTypeToVerilogManager::ReserveOrdinaryDefinitionName(
    const AstNode& node, std::optional<std::string_view> declared_name,
    std::string_view requested_name, bool already_converted,
    bool is_sum_payload) {
  std::optional<std::string> reserved_name;
  if (already_converted) {
    if (!is_sum_payload) {
      // Ordinary exports historically advance the typedef name even when the
      // declaration was already emitted; sum families keep their own identity.
      AllocateOrdinaryTypeName(requested_name, /*already_converted=*/true,
                               *typedef_name_uniquer_, allocated_package_names_,
                               ephemeral_ordinary_type_names_);
    }
  } else {
    bool use_nominal_name =
        IsOrdinarySourceTypeName(&node, declared_name, requested_name);
    std::string candidate = OrdinaryTypeNameCandidate(
        node, requested_name, is_sum_payload, use_nominal_name);
    if (!declared_name.has_value() || requested_name == *declared_name) {
      NameUniquer unoccupied("__");
      XLS_RETURN_IF_ERROR(CheckOrdinaryNameAgainstSumAliases(
          unoccupied.GetSanitizedUniqueName(candidate)));
    }
    if (use_nominal_name) {
      ordinary_source_named_types_.insert(&node);
    }
    reserved_name = AllocateOrdinaryTypeName(
        candidate, /*already_converted=*/false, *typedef_name_uniquer_,
        allocated_package_names_, ephemeral_ordinary_type_names_);
  }
  return reserved_name;
}

bool DslxTypeToVerilogManager::SumPayloadGraphs::Contains(
    const SumType& sum) const {
  auto graphs =
      graphs_.find({&sum.nominal_type(), sum.parametric_arguments_hash()});
  return graphs != graphs_.end() &&
         std::any_of(graphs->second.begin(), graphs->second.end(),
                     [&](const Arguments& arguments) {
                       return sum.HasSameSpecializationArguments(*arguments);
                     });
}

void DslxTypeToVerilogManager::SumPayloadGraphs::Add(const SumType& sum) {
  graphs_[{&sum.nominal_type(), sum.parametric_arguments_hash()}].push_back(
      sum.shared_specialization_arguments());
}

void DslxTypeToVerilogManager::SumPayloadGraphs::Merge(
    SumPayloadGraphs&& additions) {
  for (auto& [key, arguments] : additions.graphs_) {
    auto& destination = graphs_[key];
    for (Arguments& argument : arguments) {
      destination.push_back(std::move(argument));
    }
  }
}

absl::StatusOr<std::vector<const AstNode*>>
DslxTypeToVerilogManager::CollectSumPayloadNominals(
    const SumType& sum, const std::set<const AstNode*>& known,
    SumPayloadGraphs& additions, std::set<const EnumDef*>* signed_enums) const {
  std::set<const Type*> visited;
  std::set<const AstNode*> pending;
  std::vector<const AstNode*> nominals;
  std::function<absl::Status(const Type&)> visit;
  visit = [&](const Type& type) -> absl::Status {
    if (type.IsSum() && (sum_payload_graphs_.Contains(type.AsSum()) ||
                         additions.Contains(type.AsSum()))) {
      return absl::OkStatus();
    }
    XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(type));
    if (width == 0 || !visited.insert(&type).second) {
      return absl::OkStatus();
    } else if (type.IsEnum()) {
      const EnumType& enumeration = type.AsEnum();
      const EnumDef& nominal = enumeration.nominal_type();
      if (!enumeration.is_signed()) {
        // Only unsigned payloads directly reuse the ordinary declaration.
        if (!known.contains(&nominal) && pending.insert(&nominal).second) {
          nominals.push_back(&nominal);
        }
      } else if (signed_enums != nullptr) {
        // The signed companion is separate, but an explicitly exported legacy
        // enum in the same package must also have legal native member values.
        signed_enums->insert(&nominal);
      }
    } else if (type.IsStruct()) {
      const StructType& record = type.AsStruct();
      const StructDef& nominal = record.nominal_type();
      XLS_ASSIGN_OR_RETURN(bool uses_ordinary, UsesOrdinaryStructInSum(record));
      if (uses_ordinary && !known.contains(&nominal) &&
          pending.insert(&nominal).second) {
        nominals.push_back(&nominal);
      }
      for (const std::unique_ptr<Type>& member : record.members()) {
        XLS_RETURN_IF_ERROR(visit(*member));
      }
    } else if (type.IsSum()) {
      additions.Add(type.AsSum());
      for (const SumTypeVariant& variant : type.AsSum().variants()) {
        for (int64_t i = 0; i < variant.size(); ++i) {
          XLS_RETURN_IF_ERROR(visit(variant.GetMemberType(i)));
        }
      }
    } else if (type.IsArray() && !GetBitsLike(type).has_value()) {
      XLS_RETURN_IF_ERROR(visit(type.AsArray().element_type()));
    } else if (type.IsTuple()) {
      for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
        XLS_RETURN_IF_ERROR(visit(*member));
      }
    }
    return absl::OkStatus();
  };
  XLS_RETURN_IF_ERROR(visit(sum));
  return nominals;
}

absl::Status DslxTypeToVerilogManager::MarkSumPayloadNominals(
    const SumType& sum, bool newly_emitted_names_displace_enum_members) {
  SumPayloadGraphs graph_additions;
  std::set<const EnumDef*> signed_enums;
  XLS_ASSIGN_OR_RETURN(
      std::vector<const AstNode*> nominals,
      CollectSumPayloadNominals(sum, sum_payload_nominals_, graph_additions,
                                &signed_enums));
  auto legalize_signed_enums = [&]() {
    for (const EnumDef* definition : signed_enums) {
      if (signed_sum_payload_enums_.insert(definition).second) {
        LegalizeOrdinaryEnumValues(*definition);
      }
    }
  };

  if (nominals.empty()) {
    if (newly_emitted_names_displace_enum_members) {
      XLS_RETURN_IF_ERROR(UpdateOrdinaryEnumNames(ordinary_enum_projections_));
    }
    legalize_signed_enums();
    sum_payload_graphs_.Merge(std::move(graph_additions));
    return absl::OkStatus();
  }

  // Compute the final occupied names together so enum projections see every
  // typedef rename, including a parent and child reached by the same sum.
  bool update_enums = newly_emitted_names_displace_enum_members;
  std::optional<absl::flat_hash_map<const EnumDef*, bool>> projections;
  for (const AstNode* nominal : nominals) {
    if (auto* enumeration = dynamic_cast<const EnumDef*>(nominal)) {
      auto known = ordinary_enum_projections_.find(enumeration);
      if (known == ordinary_enum_projections_.end() || !known->second) {
        if (!projections.has_value()) {
          projections.emplace(ordinary_enum_projections_);
        }
        (*projections)[enumeration] = true;
        update_enums = true;
      }
    }
  }
  std::optional<std::set<std::string>> next_allocated_names;
  std::optional<std::set<std::string>> next_ordinary_names;
  for (const AstNode* nominal : nominals) {
    auto known = converted_types_.find(const_cast<AstNode*>(nominal));
    if (known == converted_types_.end() ||
        !ordinary_source_named_types_.contains(nominal)) {
      continue;
    }
    auto* reference = dynamic_cast<verilog::TypedefType*>(known->second);
    XLS_RET_CHECK(reference != nullptr);
    std::string previous = reference->type_def()->GetName();
    XLS_ASSIGN_OR_RETURN(std::string repaired, SumPayloadNominalName(*nominal));
    if (previous != repaired) {
      if (!next_allocated_names.has_value()) {
        next_allocated_names.emplace(allocated_package_names_);
        next_ordinary_names.emplace(emitted_ordinary_type_names_);
      }
      if (next_allocated_names->contains(repaired)) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "SystemVerilog sum payload type `%s` conflicts with an existing "
            "package symbol",
            repaired));
      }
      if (!ordinary_function_type_names_.contains(previous)) {
        next_allocated_names->erase(previous);
        next_ordinary_names->erase(previous);
      }
      next_allocated_names->insert(repaired);
      next_ordinary_names->insert(repaired);
      // Releasing the old spelling can also let a projected enum move back to
      // its preferred name, even if the new spelling did not collide.
      update_enums = true;
    }
  }
  if (next_ordinary_names.has_value()) {
    emitted_ordinary_type_names_ = std::move(*next_ordinary_names);
  }
  if (update_enums) {
    XLS_RETURN_IF_ERROR(UpdateOrdinaryEnumNames(
        projections.has_value() ? *projections : ordinary_enum_projections_));
  }
  for (const AstNode* nominal : nominals) {
    XLS_RETURN_IF_ERROR(ReprojectSumPayloadNominal(*nominal));
  }
  // Struct members must see every final typedef name. A previously emitted
  // parent may precede its child in the graph even when a later sum causes
  // both declarations to switch to their canonical package spellings.
  for (const AstNode* nominal : nominals) {
    if (dynamic_cast<const StructDef*>(nominal) != nullptr) {
      auto known = converted_types_.find(const_cast<AstNode*>(nominal));
      if (known != converted_types_.end()) {
        auto* reference = dynamic_cast<verilog::TypedefType*>(known->second);
        XLS_RET_CHECK(reference != nullptr);
        auto* record =
            dynamic_cast<verilog::Struct*>(reference->type_def()->data_type());
        XLS_RET_CHECK(record != nullptr);
        ProjectOrdinarySumStructMemberNames(record->members());
      }
    }
  }
  sum_payload_nominals_.insert(nominals.begin(), nominals.end());
  legalize_signed_enums();
  sum_payload_graphs_.Merge(std::move(graph_additions));
  return absl::OkStatus();
}

absl::Status DslxTypeToVerilogManager::ReprojectSumPayloadNominal(
    const AstNode& nominal) {
  auto known = converted_types_.find(const_cast<AstNode*>(&nominal));
  if (known == converted_types_.end()) {
    return absl::OkStatus();
  }
  auto* reference = dynamic_cast<verilog::TypedefType*>(known->second);
  XLS_RET_CHECK(reference != nullptr);
  verilog::Typedef* declaration = reference->type_def();
  const auto* record_definition = dynamic_cast<const StructDef*>(&nominal);
  const auto* enum_definition = dynamic_cast<const EnumDef*>(&nominal);
  XLS_RET_CHECK(record_definition != nullptr || enum_definition != nullptr);
  if (enum_definition != nullptr) {
    XLS_RETURN_IF_ERROR(
        RegisterOrdinaryEnum(*enum_definition, /*projected=*/true));
  }
  if (ordinary_source_named_types_.contains(&nominal)) {
    XLS_ASSIGN_OR_RETURN(std::string repaired, SumPayloadNominalName(nominal));
    if (repaired != declaration->GetName()) {
      const std::string previous = declaration->GetName();
      std::string allocated = ClaimVisibleName(repaired, *typedef_name_uniquer_,
                                               ephemeral_ordinary_type_names_);
      if (allocated != repaired) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "SystemVerilog sum payload type `%s` conflicts with an existing "
            "package symbol",
            repaired));
      }
      if (!ordinary_function_type_names_.contains(previous)) {
        emitted_ordinary_type_names_.erase(previous);
      }
      emitted_ordinary_type_names_.insert(repaired);
      XLS_RETURN_IF_ERROR(UpdateOrdinaryEnumNames(ordinary_enum_projections_));
      XLS_RETURN_IF_ERROR(typedef_name_uniquer_->ReleaseIdentifier(previous));
      if (!ordinary_function_type_names_.contains(previous)) {
        allocated_package_names_.erase(previous);
      }
      allocated_package_names_.insert(repaired);
      // TypedefType users retain this exact node; changing its declaration
      // updates standalone aliases and sum views without creating two types.
      *declaration =
          verilog::Typedef(MakeMember(repaired, declaration->data_type()),
                           file_.get(), declaration->loc());
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::map<std::string, std::string>>
DslxTypeToVerilogManager::CompanionNameRequests(const SumType& sum,
                                                std::string_view family) {
  std::map<std::string, std::string> names;
  std::set<std::string> visited_structs;
  std::function<absl::Status(const Type&)> visit;
  visit = [&](const Type& type) -> absl::Status {
    if (type.IsSum()) {
      // Nested sums own and allocate their own companion families.
      return absl::OkStatus();
    } else if (type.IsEnum()) {
      const EnumType& enumeration = type.AsEnum();
      if (enumeration.is_signed()) {
        const EnumDef& definition = enumeration.nominal_type();
        std::string key = EnumCompanionKey(definition);
        std::string stem = absl::StrCat(
            family, "_", NominalName(definition, definition.identifier()));
        names.emplace(absl::StrCat(key, ":type"),
                      absl::StrCat(stem, "_value_t"));
        for (int64_t i = 0; i < definition.values().size(); ++i) {
          std::string_view member = definition.GetMemberName(i);
          names.emplace(absl::StrCat(key, ":literal:", member),
                        absl::StrCat(stem, "_enum_", member));
        }
      }
    } else if (type.IsArray() && !GetBitsLike(type).has_value()) {
      const Type& element = type.AsArray().element_type();
      if (std::optional<BitsLikeProperties> bits = GetBitsLike(element);
          bits.has_value()) {
        XLS_ASSIGN_OR_RETURN(bool is_signed, bits->is_signed.GetAsBool());
        if (is_signed) {
          XLS_ASSIGN_OR_RETURN(int64_t width, bits->size.GetAsInt64());
          names.emplace(SignedArrayCompanionKey(width),
                        absl::StrCat(family, "_s", width, "_value_t"));
        }
      } else {
        XLS_RETURN_IF_ERROR(visit(element));
      }
    } else if (type.IsStruct()) {
      const StructType& record = type.AsStruct();
      XLS_ASSIGN_OR_RETURN(std::string identity,
                           sum_identities_.TypeIdentity(type));
      if (visited_structs.insert(identity).second) {
        XLS_ASSIGN_OR_RETURN(bool uses_ordinary,
                             UsesOrdinaryStructInSum(record));
        if (!uses_ordinary) {
          XLS_ASSIGN_OR_RETURN(
              std::string specialization,
              sum_identities_.StructSpecializationName(record));
          names.emplace(
              StructCompanionKey(identity),
              absl::StrCat(family, "_",
                           NominalName(record.nominal_type(),
                                       record.nominal_type().identifier()),
                           specialization, "_value_t"));
        }
        for (const std::unique_ptr<Type>& member : record.members()) {
          XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(*member));
          if (width > 0) {
            XLS_RETURN_IF_ERROR(visit(*member));
          }
        }
      }
    } else if (type.IsTuple()) {
      for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
        XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(*member));
        if (width > 0) {
          XLS_RETURN_IF_ERROR(visit(*member));
        }
      }
    }
    return absl::OkStatus();
  };
  for (const SumTypeVariant& variant : sum.variants()) {
    for (int64_t i = 0; i < variant.size(); ++i) {
      const Type& member = variant.GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(member));
      if (width > 0) {
        XLS_RETURN_IF_ERROR(visit(member));
      }
    }
  }
  return names;
}

std::string DslxTypeToVerilogManager::NewSumName(std::string_view identifier) {
  return AllocateSumName(identifier, *typedef_name_uniquer_,
                         legacy_package_names_, allocated_package_names_);
}

verilog::DataType* DslxTypeToVerilogManager::MakeBits(int64_t width,
                                                      bool is_signed) {
  return file_->BitVectorType(width, SourceInfo(), is_signed);
}

verilog::Def* DslxTypeToVerilogManager::MakeMember(std::string_view identifier,
                                                   verilog::DataType* type) {
  return file_->Make<verilog::Def>(SourceInfo(), identifier,
                                   type->IsUserDefined()
                                       ? verilog::DataKind::kUser
                                       : verilog::DataKind::kLogic,
                                   type);
}

verilog::DataType* DslxTypeToVerilogManager::AddNamedType(
    std::string_view identifier, verilog::DataType* type) {
  verilog::Typedef* type_def = top_pkg_->Add<verilog::Typedef>(
      SourceInfo(), MakeMember(identifier, type));
  return file_->Make<verilog::TypedefType>(SourceInfo(), type_def);
}

void DslxTypeToVerilogManager::ProjectSumAggregateMemberNames(
    absl::Span<verilog::Def* const> fields) {
  std::vector<std::pair<std::string, verilog::DataType*>> declarations;
  for (const verilog::Def* field : fields) {
    declarations.emplace_back(field->GetName(), field->data_type());
  }
  const auto identifiers = AggregateMemberNames(declarations);
  for (verilog::Def* field : fields) {
    const std::string& name = identifiers.at(field->GetName());
    if (field->GetName() != name) {
      *field = verilog::Def(name, field->data_kind(), field->data_type(),
                            file_.get(), field->loc());
    }
  }
}

void DslxTypeToVerilogManager::ProjectOrdinarySumStructMemberNames(
    absl::Span<verilog::Def* const> fields) {
  ProjectSumAggregateMemberNames(fields);
  std::function<void(verilog::DataType*)> protect_tuples =
      [&](verilog::DataType* type) {
        if (auto* array = dynamic_cast<verilog::PackedArrayType*>(type)) {
          protect_tuples(array->element_type());
        } else if (auto* tuple = dynamic_cast<verilog::Struct*>(type)) {
          // Ordinary annotations emit only tuples as anonymous structs. A
          // typedef starts a separate declaration and is projected by its
          // owner.
          for (verilog::Def* field : tuple->members()) {
            protect_tuples(field->data_type());
          }
          ProtectFixedSumMemberNames(tuple->members());
        }
      };
  for (verilog::Def* field : fields) {
    protect_tuples(field->data_type());
  }
}

verilog::DataType* DslxTypeToVerilogManager::QualifySumTypeNames(
    verilog::DataType* type, const std::set<std::string>& fixed_names) {
  if (auto* reference = dynamic_cast<verilog::TypedefType*>(type)) {
    if (fixed_names.contains(reference->type_def()->GetName())) {
      return file_->Make<verilog::ExternType>(SourceInfo(), top_pkg_->name(),
                                              reference->type_def()->GetName());
    }
  } else if (auto* array = dynamic_cast<verilog::PackedArrayType*>(type)) {
    verilog::DataType* element =
        QualifySumTypeNames(array->element_type(), fixed_names);
    if (element != array->element_type()) {
      return file_->Make<verilog::PackedArrayType>(
          array->loc(), element, array->dims(), array->dims_are_max());
    }
  } else if (auto* record = dynamic_cast<verilog::Struct*>(type)) {
    std::vector<verilog::Def*> fields;
    bool changed = false;
    for (verilog::Def* field : record->members()) {
      verilog::DataType* member =
          QualifySumTypeNames(field->data_type(), fixed_names);
      changed |= member != field->data_type();
      fields.push_back(member == field->data_type()
                           ? field
                           : MakeMember(field->GetName(), member));
    }
    if (changed) {
      return file_->Make<verilog::Struct>(record->loc(), fields);
    }
  }
  return type;
}

void DslxTypeToVerilogManager::ProtectFixedSumMemberNames(
    absl::Span<verilog::Def* const> fields) {
  std::set<std::string> fixed_names;
  for (verilog::Def* field : fields) {
    verilog::DataType* type =
        QualifySumTypeNames(field->data_type(), fixed_names);
    if (type != field->data_type()) {
      *field = verilog::Def(field->GetName(), field->data_kind(), type,
                            file_.get(), field->loc());
    }
    fixed_names.insert(field->GetName());
  }
}

absl::StatusOr<verilog::DataType*>
DslxTypeToVerilogManager::SumMemberToVastType(const Type& type,
                                              SumFamily& family,
                                              ImportData* import_data) {
  if (std::optional<BitsLikeProperties> bits = GetBitsLike(type);
      bits.has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t width, bits->size.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits->is_signed.GetAsBool());
    return MakeBits(width, is_signed);
  } else if (type.IsSum()) {
    return SumToVastType(type.AsSum(), import_data);
  } else if (type.IsEnum()) {
    const EnumType& enum_type = type.AsEnum();
    if (!enum_type.is_signed()) {
      // The normal exporter already preserves unsigned enum semantics. Reusing
      // its nominal type keeps ordinary enum assignments valid without casts.
      return TypeDefinitionToVastType(
          const_cast<EnumDef*>(&enum_type.nominal_type()), import_data);
    }
    auto known = family.enums.find(&enum_type.nominal_type());
    if (known != family.enums.end()) {
      return known->second;
    } else {
      XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(type));
      std::string key = EnumCompanionKey(enum_type.nominal_type());
      auto* definition =
          file_->Make<verilog::Enum>(SourceInfo(), verilog::DataKind::kLogic,
                                     MakeBits(width, enum_type.is_signed()));
      for (int64_t i = 0; i < enum_type.members().size(); ++i) {
        XLS_ASSIGN_OR_RETURN(Bits bits, enum_type.members()[i].GetBits());
        definition->AddMember(
            family.symbols.at(absl::StrCat(
                key, ":literal:", enum_type.nominal_type().GetMemberName(i))),
            file_->Literal(bits, SourceInfo()), SourceInfo());
      }
      verilog::DataType* named = AddNamedType(
          family.symbols.at(absl::StrCat(key, ":type")), definition);
      auto* aliases =
          top_pkg_->Add<verilog::VerilogPackageSection>(SourceInfo());
      ProjectSumEnumValues(definition, named, aliases);
      family.enums.emplace(&enum_type.nominal_type(), named);
      return named;
    }
  } else if (type.IsArray()) {
    std::vector<int64_t> dimensions;
    const Type* base = &type;
    while (base->IsArray() && !GetBitsLike(*base).has_value()) {
      const ArrayType& array = base->AsArray();
      XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
      dimensions.push_back(size);
      base = &array.element_type();
    }
    XLS_ASSIGN_OR_RETURN(verilog::DataType * element,
                         SumMemberToVastType(*base, family, import_data));
    // A packed dimension placed directly on signed logic makes the aggregate
    // signed, but indexing that dimension produces an unsigned vector. A named
    // signed element retains its type when an SV user selects one array item.
    if (std::optional<BitsLikeProperties> bits = GetBitsLike(*base);
        bits.has_value()) {
      XLS_ASSIGN_OR_RETURN(bool is_signed, bits->is_signed.GetAsBool());
      XLS_ASSIGN_OR_RETURN(int64_t width, bits->size.GetAsInt64());
      if (is_signed) {
        auto existing = family.signed_array_elements.find(width);
        if (existing == family.signed_array_elements.end()) {
          element = AddNamedType(
              family.symbols.at(SignedArrayCompanionKey(width)), element);
          family.signed_array_elements.emplace(width, element);
        } else {
          element = existing->second;
        }
      } else {
        // An unsigned bits base shares the same VAST node with the outer
        // dimensions, so VAST prints them in DSLX indexing order.
        element = file_->Make<verilog::ScalarType>(SourceInfo());
        if (width > 1) {
          dimensions.push_back(width);
        }
      }
    }
    return file_->Make<verilog::PackedArrayType>(SourceInfo(), element,
                                                 dimensions, false);
  } else if (type.IsStruct() || type.IsTuple()) {
    bool is_struct = type.IsStruct();
    std::optional<std::string> semantic_struct_key;
    if (is_struct) {
      const StructType& record = type.AsStruct();
      XLS_ASSIGN_OR_RETURN(bool uses_ordinary, UsesOrdinaryStructInSum(record));
      if (uses_ordinary) {
        return TypeDefinitionToVastType(
            const_cast<StructDef*>(&record.nominal_type()), import_data);
      } else {
        XLS_ASSIGN_OR_RETURN(semantic_struct_key,
                             sum_identities_.TypeIdentity(type));
        auto known = family.structs.find(*semantic_struct_key);
        if (known != family.structs.end()) {
          return known->second;
        }
      }
    }
    auto convert = [&](int64_t, const Type& member) {
      return SumMemberToVastType(member, family, import_data);
    };
    auto make = [&](std::string_view name, verilog::DataType* member) {
      return MakeMember(name, member);
    };
    XLS_ASSIGN_OR_RETURN(
        std::vector<verilog::Def*> members,
        is_struct ? AggregateMembers(
                        type.AsStruct(), AggregateNamePolicy::kSumPayload,
                        [&](int64_t i) {
                          return std::string(type.AsStruct().GetMemberName(i));
                        },
                        convert, make)
                  : AggregateMembers(
                        type.AsTuple(), AggregateNamePolicy::kLegacy,
                        [](int64_t i) { return absl::StrCat("index_", i); },
                        convert, make));
    if (!is_struct) {
      ProtectFixedSumMemberNames(members);
    }
    verilog::DataType* aggregate =
        file_->Make<verilog::Struct>(SourceInfo(), members);
    if (semantic_struct_key.has_value()) {
      aggregate = AddNamedType(
          family.symbols.at(StructCompanionKey(*semantic_struct_key)),
          aggregate);
      family.structs.emplace(*semantic_struct_key, aggregate);
    }
    return aggregate;
  } else {
    return absl::UnimplementedError(absl::StrFormat(
        "Unsupported DSLX sum payload type for SystemVerilog: %s",
        type.ToString()));
  }
}

absl::StatusOr<std::string> DslxTypeToVerilogManager::ClaimSumSpecialization(
    const SumType& sum, std::string_view family) {
  std::string suffix;
  if (!sum.specialization_arguments().empty()) {
    XLS_ASSIGN_OR_RETURN(suffix, sum_identities_.SpecializationName(sum));
    XLS_ASSIGN_OR_RETURN(std::string identity,
                         sum_identities_.TypeIdentity(sum));
    auto [existing, inserted] =
        sum_specialization_owners_[&sum.nominal_type()].emplace(suffix,
                                                                identity);
    if (!inserted && existing->second != identity) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Different specializations of DSLX sum `%s` have the same "
          "SystemVerilog spelling `%s%s`",
          sum.nominal_type().identifier(), family, suffix));
    }
  }
  return suffix;
}

absl::Status DslxTypeToVerilogManager::PlanSumFamilyNames(
    const SumType& sum, std::string_view specialization, int64_t payload_width,
    SumFamily& family) {
  if (!specialization.empty()) {
    family.name = NewSumName(absl::StrCat(family.name, specialization));
  }
  auto static_symbols = nominal_sum_symbols_.find(&sum.nominal_type());
  XLS_ASSIGN_OR_RETURN(auto companions,
                       CompanionNameRequests(sum, family.name));
  if (static_symbols != nominal_sum_symbols_.end()) {
    family.symbols = static_symbols->second;
    for (const auto& [key, name] : companions) {
      if (!family.symbols.contains(key)) {
        family.symbols.emplace(key, NewSumName(name));
      }
    }
  } else {
    family.symbols =
        FamilyNameRequests(sum.nominal_type(), family.name, payload_width > 0);
    family.symbols.merge(companions);
    for (auto& [key, name] : family.symbols) {
      name = NewSumName(name);
    }
  }
  return absl::OkStatus();
}

const DslxTypeToVerilogManager::SumFamily*
DslxTypeToVerilogManager::FindSumFamily(const SumType& sum) const {
  auto families = sum_families_.find(&sum.nominal_type());
  if (families != sum_families_.end()) {
    for (const auto& family : families->second) {
      if (family->type->AsSum().HasSameSpecializationArguments(
              sum.specialization_arguments())) {
        return family.get();
      }
    }
  }
  return nullptr;
}

absl::Status DslxTypeToVerilogManager::CheckOrdinaryTypeName(
    std::string_view identifier) const {
  std::string name(identifier);
  XLS_RETURN_IF_ERROR(CheckOrdinaryNameAgainstSumAliases(name));
  // A generic nominal's stem stays allocated to stabilize its specializations,
  // but the stem itself is not a declaration. Actual family symbols still win.
  auto is_only_generic_stem = [&] {
    bool is_generic_stem = false;
    for (const auto& [definition, stem] : nominal_sum_names_) {
      if (stem == name) {
        if (definition->parametric_bindings().empty()) {
          return false;
        } else {
          is_generic_stem = true;
        }
      }
    }
    if (!is_generic_stem) {
      return false;
    }
    auto contains_name = [&](const auto& symbols) {
      return std::any_of(
          symbols.begin(), symbols.end(),
          [&](const auto& entry) { return entry.second == name; });
    };
    for (const auto& [definition, symbols] : nominal_sum_symbols_) {
      if (contains_name(symbols)) {
        return false;
      }
    }
    for (const auto& [definition, families] : sum_families_) {
      for (const auto& family : families) {
        if (family->name == name || contains_name(family->symbols)) {
          return false;
        }
      }
    }
    return true;
  };
  if (emitted_sum_names_.contains(name) ||
      (allocated_package_names_.contains(name) &&
       !emitted_ordinary_type_names_.contains(name) &&
       !is_only_generic_stem())) {
    return OrdinaryTypeNameConflict(name);
  }
  return absl::OkStatus();
}

absl::StatusOr<verilog::DataType*> DslxTypeToVerilogManager::SumToVastType(
    const SumType& sum, ImportData* import_data) {
  if (const SumFamily* existing = FindSumFamily(sum); existing != nullptr) {
    XLS_RET_CHECK(existing->envelope != nullptr)
        << "Recursive SystemVerilog sum: " << sum.nominal_type().identifier();
    return existing->envelope;
  }
  auto& nominal_families = sum_families_[&sum.nominal_type()];
  XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(sum));
  XLS_RET_CHECK_GT(width, 0);
  XLS_ASSIGN_OR_RETURN(int64_t tag_width, sum.tag_bit_count().GetAsInt64());
  XLS_ASSIGN_OR_RETURN(TypeDim payload_dim, sum.GetMaxPayloadBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t payload_width, payload_dim.GetAsInt64());

  auto owner = std::make_unique<SumFamily>();
  owner->type = sum.CloneToUnique();
  auto nominal = nominal_sum_names_.find(&sum.nominal_type());
  if (nominal == nominal_sum_names_.end()) {
    owner->name = NewSumName(sum.nominal_type().identifier());
    nominal_sum_names_.emplace(&sum.nominal_type(), owner->name);
  } else {
    owner->name = nominal->second;
  }
  XLS_ASSIGN_OR_RETURN(std::string specialization,
                       ClaimSumSpecialization(sum, owner->name));
  XLS_RETURN_IF_ERROR(
      PlanSumFamilyNames(sum, specialization, payload_width, *owner));
  bool displaces_enum_members = false;
  auto mark_emitted = [&](const std::string& name) {
    if (emitted_sum_names_.insert(name).second) {
      displaces_enum_members |=
          projected_ordinary_enum_member_names_.contains(name);
    }
  };
  mark_emitted(owner->name);
  for (const auto& [key, name] : owner->symbols) {
    mark_emitted(name);
  }
  XLS_RETURN_IF_ERROR(MarkSumPayloadNominals(sum, displaces_enum_members));
  SumFamily& family = *owner;
  nominal_families.push_back(std::move(owner));

  // Records the visible fields and leading padding shared by a variant's
  // packed view and constructor.
  struct VariantProjection {
    std::string suffix;
    std::string constructor;
    bool has_named_fields;
    int64_t padding_width = 0;
    std::vector<verilog::Def*> fields;
  };
  std::vector<VariantProjection> projections;
  const auto variant_names = VariantSuffixes(sum.nominal_type());
  for (const SumTypeVariant& variant : sum.variants()) {
    VariantProjection projection;
    projection.has_named_fields = variant.is_struct();
    // Prefixes make a keyword like Byte safe without distorting the spelling
    // exposed after as_ or make_. Distinct source spellings can normalize
    // alike.
    projection.suffix = variant_names.at(variant.variant().identifier());
    projection.constructor = family.symbols.at(
        absl::StrCat("constructor:", variant.variant().identifier()));
    XLS_ASSIGN_OR_RETURN(TypeDim variant_dim, variant.GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t variant_width, variant_dim.GetAsInt64());
    projection.padding_width = payload_width - variant_width;
    XLS_ASSIGN_OR_RETURN(
        projection.fields,
        AggregateMembers(
            variant, AggregateNamePolicy::kLegacy,
            [&](int64_t i) {
              return variant.is_struct()
                         ? std::string(variant.GetMemberName(i))
                         : (variant.size() == 1 ? "value"
                                                : absl::StrCat("index_", i));
            },
            [&](int64_t, const Type& member) {
              return SumMemberToVastType(member, family, import_data);
            },
            [&](std::string_view name, verilog::DataType* type) {
              return MakeMember(name, type);
            }));
    // Obtain every variant's member types before building its view: nested
    // nominal declarations are emitted before all of this family's views.
    projections.push_back(std::move(projection));
  }

  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(), absl::StrCat("DSLX Type: ", sum.nominal_type().ToString()));
  bool signed_tag = tag_width > 0 && !sum.variants().empty() &&
                    sum.GetDiscriminant(0).IsSBits();
  // SystemVerilog requires a nonzero enum width. A zero-width DSLX tag gets an
  // unsigned one-bit enum and a getter for its only value, but no envelope
  // bits.
  auto* tag_definition = file_->Make<verilog::Enum>(
      SourceInfo(), verilog::DataKind::kLogic,
      MakeBits(std::max<int64_t>(1, tag_width), signed_tag));
  std::vector<verilog::EnumMemberRef*> tag_values;
  for (int64_t i = 0; i < sum.variant_count(); ++i) {
    XLS_ASSIGN_OR_RETURN(Bits tag_bits, sum.GetDiscriminant(i).GetBits());
    if (tag_width == 0) {
      tag_bits = UBits(0, 1);
    }
    tag_values.push_back(tag_definition->AddMember(
        family.symbols.at(
            absl::StrCat("tag:", sum.variants()[i].variant().identifier())),
        file_->Literal(tag_bits, SourceInfo()), SourceInfo()));
  }
  verilog::DataType* tag_type =
      AddNamedType(family.symbols.at("tag"), tag_definition);

  std::vector<verilog::Def*> envelope_members;
  if (tag_width > 0) {
    envelope_members.push_back(MakeMember("tag", tag_type));
  }
  if (payload_width > 0) {
    std::vector<verilog::Def*> union_members{
        MakeMember("bits", MakeBits(payload_width))};
    for (int64_t i = 0; i < projections.size(); ++i) {
      VariantProjection& projection = projections[i];
      if (projection.has_named_fields) {
        ProjectSumAggregateMemberNames(projection.fields);
      } else {
        ProtectFixedSumMemberNames(projection.fields);
      }
      std::vector<verilog::Def*> fields;
      if (projection.padding_width > 0) {
        std::set<std::string> field_names;
        TypeUsePositions referenced_types;
        for (const verilog::Def* field : projection.fields) {
          field_names.insert(field->GetName());
          RecordTypeUsePositions(field->data_type(),
                                 TypeReferenceScope::kPackedMember, 0,
                                 referenced_types);
        }
        NameUniquer padding_names("__");
        std::string padding;
        do {
          padding = MemberName(padding_names, "xls_padding");
        } while (field_names.contains(padding) ||
                 referenced_types.contains(padding));
        fields.push_back(
            MakeMember(padding, MakeBits(projection.padding_width)));
      }
      fields.insert(fields.end(), projection.fields.begin(),
                    projection.fields.end());
      verilog::DataType* view =
          AddNamedType(family.symbols.at(absl::StrCat(
                           "view:", sum.variants()[i].variant().identifier())),
                       file_->Make<verilog::Struct>(SourceInfo(), fields));
      union_members.push_back(
          MakeMember(absl::StrCat("as_", projection.suffix), view));
    }
    ProtectFixedSumMemberNames(union_members);
    verilog::DataType* payload_type =
        AddNamedType(family.symbols.at("payload"),
                     file_->Make<verilog::Union>(SourceInfo(), union_members));
    envelope_members.push_back(MakeMember("payload", payload_type));
  }
  family.envelope = AddNamedType(
      family.name,
      file_->Make<verilog::Struct>(SourceInfo(), envelope_members));

  for (int64_t i = 0; i < projections.size(); ++i) {
    const VariantProjection& projection = projections[i];
    top_pkg_->Add<verilog::BlankLine>(SourceInfo());
    auto* constructor = top_pkg_->Add<verilog::VerilogFunction>(
        SourceInfo(), projection.constructor, family.envelope);
    std::vector<verilog::Expression*> parts;
    if (tag_width > 0) {
      parts.push_back(tag_values[i]->Duplicate());
    }
    // Keep word-sized literals readable without allocating a bitmap for wide
    // zeros in every constructor.
    if (projection.padding_width > 64) {
      parts.push_back(file_->Concat(projection.padding_width,
                                    {file_->Literal1(0, SourceInfo())},
                                    SourceInfo()));
    } else if (projection.padding_width > 0) {
      parts.push_back(
          file_->Literal(UBits(0, projection.padding_width), SourceInfo()));
    }
    // Formals share scope with the implicit function result and can hide
    // following input types, or package names used in the function body.
    std::set<std::string> body_names{projection.constructor, family.name};
    if (tag_width > 0) {
      body_names.insert(tag_values[i]->member()->GetName());
    }
    TypeUsePositions last_type_uses;
    auto is_protected = [&](int64_t position, const std::string& name) {
      return body_names.contains(name) ||
             IsTypeUsedAfter(last_type_uses, name, position);
    };
    std::set<std::string> unchanged_formals;
    if (projection.has_named_fields) {
      std::vector<std::pair<std::string, verilog::DataType*>> formals;
      for (const verilog::Def* field : projection.fields) {
        formals.emplace_back(field->GetName(), field->data_type());
      }
      last_type_uses =
          LastTypeUsePositions(formals, TypeReferenceScope::kFunctionFormal);
      for (int64_t j = 0; j < projection.fields.size(); ++j) {
        const std::string& name = projection.fields[j]->GetName();
        if (!is_protected(j, name)) {
          unchanged_formals.insert(name);
        }
      }
    }
    NameUniquer formal_names("__");
    for (int64_t j = 0; j < projection.fields.size(); ++j) {
      const verilog::Def* field = projection.fields[j];
      std::string name = field->GetName();
      if (projection.has_named_fields && is_protected(j, name)) {
        do {
          name = MemberName(formal_names, field->GetName());
        } while (unchanged_formals.contains(name) || is_protected(j, name));
      }
      parts.push_back(constructor->AddArgument(
          MakeMember(name, field->data_type()), SourceInfo()));
    }
    XLS_RET_CHECK(!parts.empty());
    verilog::Expression* bits =
        parts.size() == 1 ? parts.front() : file_->Concat(parts, SourceInfo());
    verilog::DataType* cast_type = family.envelope;
    if (!projection.has_named_fields) {
      std::set<std::string> fixed_names;
      for (const verilog::Def* field : projection.fields) {
        fixed_names.insert(field->GetName());
      }
      cast_type = QualifySumTypeNames(cast_type, fixed_names);
    }
    constructor->AddStatement<verilog::BlockingAssignment>(
        SourceInfo(), constructor->return_value_ref(),
        file_->Make<verilog::TypeCast>(SourceInfo(), cast_type, bits));
  }
  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  auto* getter = top_pkg_->Add<verilog::VerilogFunction>(
      SourceInfo(), family.symbols.at("getter"), tag_type);
  verilog::LogicRef* argument =
      getter->AddArgument(MakeMember("value", family.envelope), SourceInfo());
  verilog::Expression* result;
  if (tag_width == 0) {
    XLS_RET_CHECK_EQ(tag_values.size(), 1);
    result = tag_values.front()->Duplicate();
  } else {
    result = file_->Make<verilog::TypeCast>(
        SourceInfo(), tag_type,
        file_->Slice(argument, width - 1, width - tag_width, SourceInfo()));
  }
  getter->AddStatement<verilog::BlockingAssignment>(
      SourceInfo(), getter->return_value_ref(), result);
  return family.envelope;
}

absl::StatusOr<verilog::DataType*> DslxTypeToVerilogManager::AddSumAlias(
    const SumType& sum, std::string_view identifier, ImportData* import_data,
    const TypeAlias* source_alias) {
  std::string requested = verilog::SanitizeVerilogIdentifier(identifier);
  XLS_ASSIGN_OR_RETURN(verilog::DataType * canonical,
                       SumToVastType(sum, import_data));
  std::string canonical_name =
      static_cast<verilog::TypedefType*>(canonical)->type_def()->GetName();
  auto known = sum_aliases_.find(requested);
  if (requested == canonical_name) {
    return canonical;
  } else if (known != sum_aliases_.end() &&
             static_cast<verilog::TypedefType*>(known->second)
                     ->type_def()
                     ->data_type() == canonical) {
    return known->second;
  } else if (known != sum_aliases_.end()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "SystemVerilog alias `%s` already names a different sum family; "
        "cannot use it for `%s`",
        requested, canonical_name));
  } else {
    if (allocated_package_names_.contains(requested) ||
        emitted_ordinary_type_names_.contains(requested) ||
        IsOrdinaryEnumMemberName(requested, source_alias)) {
      return SumAliasConflict(requested, canonical_name);
    }
    std::string reserved = ClaimVisibleName(requested, *typedef_name_uniquer_,
                                            ephemeral_ordinary_type_names_);
    if (reserved != requested) {
      CHECK_OK(typedef_name_uniquer_->ReleaseIdentifier(reserved));
      return SumAliasConflict(requested, canonical_name);
    }
    allocated_package_names_.insert(requested);
    top_pkg_->Add<verilog::BlankLine>(SourceInfo());
    top_pkg_->Add<verilog::Comment>(
        SourceInfo(),
        absl::StrCat("DSLX Type: ", sum.ToString(),
                     "; SystemVerilog sum family: ", canonical_name));
    verilog::DataType* alias = AddNamedType(requested, canonical);
    sum_aliases_.emplace(requested, alias);
    return alias;
  }
}

absl::StatusOr<std::pair<std::vector<int64_t>, verilog::DataType*>>
DslxTypeToVerilogManager::GetArrayDimsAndBaseType(
    const Type* type, const ArrayTypeAnnotation* array_type_annotation,
    ImportData* import_data) {
  type = UnboxMetaTypes(type);

  // Check if this "array" is actually a bits type.
  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(*type);
      bits_like.has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t size, bits_like->size.GetAsInt64());

    verilog::DataType* base_type =
        file_->Make<verilog::ScalarType>(SourceInfo());
    std::vector<int64_t> dims;
    if (size > 1) {
      dims.push_back(size);
    }
    return std::make_pair(dims, base_type);
  }

  const ArrayType& array_type = type->AsArray();

  // Get size.
  XLS_ASSIGN_OR_RETURN(int64_t size, array_type.size().GetAsInt64());

  // Check if element type is a bits type.
  if (std::optional<BitsLikeProperties> bits_like =
          GetBitsLike(array_type.element_type());
      bits_like.has_value()) {
    verilog::DataType* base_type =
        file_->Make<verilog::ScalarType>(SourceInfo());
    XLS_ASSIGN_OR_RETURN(int64_t bits_size, bits_like->size.GetAsInt64());
    return std::make_pair(std::vector<int64_t>{size, bits_size}, base_type);
  }

  // Check if element type is an array type as well
  if (auto element_type_annotation = dynamic_cast<const ArrayTypeAnnotation*>(
          array_type_annotation->element_type())) {
    XLS_ASSIGN_OR_RETURN(auto ret, GetArrayDimsAndBaseType(
                                       &array_type.element_type(),
                                       element_type_annotation, import_data));

    ret.first.insert(ret.first.begin(), size);

    return ret;
  }

  // This is a "single" dimension array with element of non-bits type.
  XLS_ASSIGN_OR_RETURN(verilog::DataType * element_type,
                       TypeAnnotationToVastType(
                           &array_type.element_type(),
                           array_type_annotation->element_type(), import_data));

  std::vector<int64_t> dims{size};
  return std::make_pair(dims, element_type);
}

absl::StatusOr<verilog::DataType*>
DslxTypeToVerilogManager::TypeAnnotationToVastType(
    const Type* type, const TypeAnnotation* type_annotation,
    ImportData* import_data) {
  VLOG(3) << "Converting TypeAnnotation " << type_annotation->ToString()
          << " with concrete Type to Verilog: " << *type;

  if (const Type* concrete = UnboxMetaTypes(type); concrete->IsSum()) {
    return SumToVastType(concrete->AsSum(), import_data);
  }

  if (auto ta = dynamic_cast<const BuiltinTypeAnnotation*>(type_annotation)) {
    int64_t size = ta->GetBitCount();

    if (size == 1) {
      return file_->Make<verilog::ScalarType>(SourceInfo());
    }

    return file_->Make<verilog::BitVectorType>(SourceInfo(), size, false);
  }

  if (auto ta = dynamic_cast<const ArrayTypeAnnotation*>(type_annotation)) {
    XLS_ASSIGN_OR_RETURN((auto [dims, data_type]),
                         GetArrayDimsAndBaseType(type, ta, import_data));

    if (dims.empty()) {
      return data_type;
    }

    // Unpacked arrays of unpacked arrays are not supported.
    if (dynamic_cast<verilog::UnpackedArrayType*>(data_type) != nullptr) {
      return absl::UnimplementedError(
          absl::StrFormat("DslxTypeToVerilogManager: Unpacked array of "
                          "unpacked array not supported, type annotation: %s",
                          type_annotation->ToString()));
    }

    return file_->Make<verilog::PackedArrayType>(SourceInfo(), data_type, dims,
                                                 /*dims_are_max=*/false);
  }

  if (auto ta = dynamic_cast<const TupleTypeAnnotation*>(type_annotation)) {
    const TupleType& tuple_type = type->AsTuple();

    // Tuple types in DSLX are converted to verilog structs.
    XLS_ASSIGN_OR_RETURN(
        std::vector<verilog::Def*> struct_members,
        AggregateMembers(
            tuple_type, AggregateNamePolicy::kLegacy,
            [](int64_t i) { return absl::StrCat("index_", i); },
            [&](int64_t i, const Type& element) {
              return TypeAnnotationToVastType(&element, ta->members().at(i),
                                              import_data);
            },
            [&](std::string_view name, verilog::DataType* member) {
              return MakeMember(name, member);
            }));

    return file_->Make<verilog::Struct>(SourceInfo(), struct_members);
  }

  if (auto ta = dynamic_cast<const TypeRefTypeAnnotation*>(type_annotation)) {
    const TypeDefinition& definition = ta->type_ref()->type_definition();
    return TypeDefinitionToVastType(definition, import_data,
                                    TypeDefinitionName(definition));
  }

  return absl::InternalError(absl::StrFormat(
      "TypeAnnotation Misc %s not supported by DslxTypeToVerilogManager",
      type_annotation->ToString()));
}

absl::StatusOr<verilog::DataType*>
DslxTypeToVerilogManager::TypeDefinitionToVastType(
    const TypeDefinition& type_definition, ImportData* import_data,
    std::optional<std::string_view> identifier, std::optional<bool> sum_alias) {
  AstNode* type_definition_node = TypeDefinitionToAstNode(type_definition);
  XLS_ASSIGN_OR_RETURN(
      TypeInfo * type_info,
      import_data->GetRootTypeInfoForNode(type_definition_node));
  XLS_ASSIGN_OR_RETURN(
      Type * type, GetActualType(type_definition_node, type_info, import_data));
  XLS_ASSIGN_OR_RETURN(
      const TypeInfo::TypeSource resolved_type_definition_source,
      type_info->ResolveTypeDefinition(type_definition));

  if (const Type* concrete = UnboxMetaTypes(type); concrete->IsSum()) {
    const SumType& sum = concrete->AsSum();
    bool is_nominal = std::holds_alternative<SumDef*>(
        resolved_type_definition_source.definition);
    verilog::DataType* result;
    bool alias = sum_alias.value_or(
        !is_nominal || (identifier.has_value() &&
                        *identifier != sum.nominal_type().identifier()));
    if (!alias) {
      XLS_ASSIGN_OR_RETURN(result, SumToVastType(sum, import_data));
    } else {
      std::optional<std::string_view> requested =
          identifier.has_value()
              ? identifier
              : TypeDefinitionIdentifier(resolved_type_definition_source);
      XLS_RET_CHECK(requested.has_value());
      XLS_ASSIGN_OR_RETURN(
          result, AddSumAlias(sum, *requested, import_data,
                              dynamic_cast<TypeAlias*>(type_definition_node)));
    }
    converted_types_.insert({type_definition_node, result});
    return result;
  }

  std::optional<std::string_view> type_definition_name =
      TypeDefinitionIdentifier(resolved_type_definition_source);
  if (!identifier.has_value()) {
    identifier = type_definition_name;
  }
  XLS_RET_CHECK(identifier.has_value());

  auto iter = converted_types_.find(type_definition_node);
  bool already_converted =
      type_definition_name.has_value() && iter != converted_types_.end();
  XLS_ASSIGN_OR_RETURN(
      std::optional<std::string> reserved_name,
      ReserveOrdinaryDefinitionName(
          *type_definition_node, type_definition_name, *identifier,
          already_converted,
          sum_payload_nominals_.contains(type_definition_node)));
  if (!reserved_name.has_value()) {
    return iter->second;
  }
  std::string typedef_identifier = std::move(*reserved_name);
  emitted_ordinary_type_names_.insert(typedef_identifier);
  if (projected_ordinary_enum_member_names_.contains(typedef_identifier)) {
    XLS_RETURN_IF_ERROR(UpdateOrdinaryEnumNames(ordinary_enum_projections_));
  }

  VLOG(3) << "Converting TypeDefinition " << type_definition_node->ToString()
          << " with concrete Type to Verilog: " << *type;

  const EnumDef* emitted_enum = nullptr;
  absl::StatusOr<verilog::DataType*> data_type_or = absl::visit(
      Visitor{
          [&](TypeAlias* alias) -> absl::StatusOr<verilog::DataType*> {
            XLS_ASSIGN_OR_RETURN(TypeInfo * alias_type_info,
                                 import_data->GetRootTypeInfoForNode(alias));
            std::optional<Type*> alias_type = alias_type_info->GetItem(alias);
            XLS_RET_CHECK(alias_type.has_value()) << absl::StrFormat(
                "Unable to locate concrete type for alias %s in %s",
                alias->name_def().identifier(),
                alias->GetSpan()
                    .value_or(FakeSpan())
                    .ToString(import_data->file_table()));
            return TypeAnnotationToVastType(
                *alias_type, &alias->type_annotation(), import_data);
          },
          [&](StructDef* struct_def) -> absl::StatusOr<verilog::DataType*> {
            XLS_RET_CHECK(type->IsStruct());
            const StructType& struct_type = type->AsStruct();

            VLOG(3) << "Converting struct type to Verilog: " << struct_type
                    << " size " << struct_type.size();

            XLS_ASSIGN_OR_RETURN(
                std::vector<verilog::Def*> vast_struct_members,
                AggregateMembers(
                    struct_type, AggregateNamePolicy::kLegacy,
                    [&](int64_t i) {
                      return std::string(struct_type.GetMemberName(i));
                    },
                    [&](int64_t i, const Type& member) {
                      return TypeAnnotationToVastType(
                          &member, struct_def->members().at(i)->type(),
                          import_data);
                    },
                    [&](std::string_view name, verilog::DataType* member) {
                      return MakeMember(name, member);
                    }));
            if (sum_payload_nominals_.contains(struct_def)) {
              ProjectOrdinarySumStructMemberNames(vast_struct_members);
            }

            return file_->Make<verilog::Struct>(SourceInfo(),
                                                vast_struct_members);
          },
          [&](ProcDef* proc_def) -> absl::StatusOr<verilog::DataType*> {
            return absl::InternalError(
                absl::StrFormat("TypeAnnotation ProcDef %s not supported by "
                                "DslxTypeToVerilogManager",
                                proc_def->ToString()));
          },
          [&](UseTreeEntry* use_tree_entry)
              -> absl::StatusOr<verilog::DataType*> {
            return absl::UnimplementedError(absl::StrFormat(
                "TypeAnnotation UseTreeEntry %s not supported by "
                "DslxTypeToVerilogManager",
                use_tree_entry->ToString()));
          },
          [&](EnumDef* enum_def) -> absl::StatusOr<verilog::DataType*> {
            XLS_RET_CHECK(type->IsEnum());
            const EnumType& enum_type = type->AsEnum();
            auto it = converted_types_.find(&enum_type.nominal_type());
            if (it != converted_types_.end()) {
              return it->second;
            }

            XLS_ASSIGN_OR_RETURN(TypeDim dim, enum_type.GetTotalBitCount());
            XLS_ASSIGN_OR_RETURN(int64_t size, dim.GetAsInt64());
            XLS_RETURN_IF_ERROR(RegisterOrdinaryEnum(
                *enum_def, sum_payload_nominals_.contains(enum_def)));

            verilog::DataType* vast_enum_data_type;
            if (size == 1) {
              vast_enum_data_type =
                  file_->Make<verilog::ScalarType>(SourceInfo());
            } else {
              vast_enum_data_type = file_->Make<verilog::BitVectorType>(
                  SourceInfo(), size, false);
            }

            verilog::Enum* vast_enum_def = file_->Make<verilog::Enum>(
                SourceInfo(), verilog::DataKind::kLogic, vast_enum_data_type);

            for (int64_t i = 0; i < enum_def->values().size(); ++i) {
              const std::string& member_name =
                  legacy_enum_member_names_.at(enum_def)[i];
              const InterpValue& member_val = enum_type.members().at(i);

              XLS_ASSIGN_OR_RETURN(Bits member_val_as_bits,
                                   member_val.GetBits());
              verilog::Literal* vast_literal =
                  file_->Literal(member_val_as_bits, SourceInfo());
              vast_enum_def->AddMember(member_name, vast_literal, SourceInfo());
            }

            emitted_enum = enum_def;
            return vast_enum_def;
          },
          [&](SumDef*) -> absl::StatusOr<verilog::DataType*> {
            return absl::InternalError("Sum type was not resolved as a sum");
          },
      },
      resolved_type_definition_source.definition);
  XLS_ASSIGN_OR_RETURN(verilog::DataType * data_type, std::move(data_type_or));

  // Add typedef to the verilog file.
  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(),
      absl::StrFormat("DSLX Type: %s",
                      TypeDefinitionToAstNode(type_definition)->ToString()));

  verilog::DataType* typedef_type = AddNamedType(typedef_identifier, data_type);
  converted_types_.insert({type_definition_node, typedef_type});
  if (emitted_enum != nullptr) {
    auto* enumeration = static_cast<verilog::Enum*>(data_type);
    auto* aliases = top_pkg_->Add<verilog::VerilogPackageSection>(SourceInfo());
    std::vector<EmittedEnumMember> members(enumeration->members().begin(),
                                           enumeration->members().end());
    ordinary_enum_emissions_.emplace(
        emitted_enum, OrdinaryEnumEmission{enumeration, typedef_type, aliases,
                                           std::move(members)});
    if (ordinary_enum_projections_.at(emitted_enum) ||
        signed_sum_payload_enums_.contains(emitted_enum)) {
      LegalizeOrdinaryEnumValues(*emitted_enum);
    }
  }
  return typedef_type;
}

DslxTypeToVerilogManager::DslxTypeToVerilogManager(
    std::string_view package_name)
    : file_(std::make_unique<verilog::VerilogFile>(
          verilog::FileType::kSystemVerilog)),
      typedef_name_uniquer_(std::make_unique<NameUniquer>("__")) {
  top_pkg_ = file_->AddVerilogPackage(package_name, SourceInfo());
}

absl::Status DslxTypeToVerilogManager::AddTypeToVerilogPackage(
    Type* type, const TypeDefinition& type_definition, ImportData* import_data,
    std::string_view typedef_identifier, bool sum_alias) {
  // Filter out unsupported interface types.
  if (type->HasToken()) {
    return absl::UnimplementedError(
        absl::StrFormat("Interface type %s containing tokens not supported.",
                        type->ToString()));
  }

  XLS_ASSIGN_OR_RETURN(TypeDim type_dim, type->GetTotalBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t type_bit_count, type_dim.GetAsInt64());
  if (type_bit_count == 0) {
    return absl::UnimplementedError(absl::StrFormat(
        "Zero sized interface type %s not supported.", type->ToString()));
  }

  return TypeDefinitionToVastType(type_definition, import_data,
                                  typedef_identifier, sum_alias)
      .status();
}

absl::Status DslxTypeToVerilogManager::AddTypeToVerilogPackage(
    Type* type, TypeAnnotation* type_annotation, ImportData* import_data,
    std::string_view typedef_identifier, bool sum_alias) {
  // Filter out unsupported interface types.
  if (type->HasToken()) {
    return absl::UnimplementedError(
        absl::StrFormat("Interface type %s containing tokens not supported.",
                        type->ToString()));
  }

  XLS_ASSIGN_OR_RETURN(TypeDim type_dim, type->GetTotalBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t type_bit_count, type_dim.GetAsInt64());
  if (type_bit_count == 0) {
    return absl::UnimplementedError(absl::StrFormat(
        "Zero sized interface type %s not supported.", type->ToString()));
  }

  if (type->IsSum()) {
    std::optional<AstNode*> definition = GetTypeDefinition(type_annotation);
    TypeAlias* source_alias = definition.has_value()
                                  ? dynamic_cast<TypeAlias*>(*definition)
                                  : nullptr;
    if (!sum_alias) {
      return SumToVastType(type->AsSum(), import_data).status();
    }
    return AddSumAlias(type->AsSum(), typedef_identifier, import_data,
                       source_alias)
        .status();
  }

  XLS_RETURN_IF_ERROR(CheckOrdinaryTypeName(typedef_identifier));

  // Add typedef to the verilog file.
  XLS_ASSIGN_OR_RETURN(
      verilog::DataType * data_type,
      TypeAnnotationToVastType(type, type_annotation, import_data));
  // A dependency may allocate a concrete generic family's symbols here.
  XLS_RETURN_IF_ERROR(CheckOrdinaryTypeName(typedef_identifier));

  // Check after converting to a VAST type because converting to VAST may create
  // a new typedef.
  if (std::optional<AstNode*> node = GetTypeDefinition(type_annotation);
      node.has_value()) {
    auto iter = converted_types_.find(*node);
    // If there's an existing typedef with the same name, return early.
    // We need to check the name of the typedef because the typedef identifier
    // can get a user-defined name.
    if (iter != converted_types_.end() &&
        static_cast<verilog::TypedefType*>(iter->second)
                ->type_def()
                ->GetName() == typedef_identifier) {
      return absl::OkStatus();
    }
  }
  // Keep ordinary function aliases visible to sums without changing the
  // existing behavior of ordinary aliases that repeat a package name.
  std::string name(typedef_identifier);
  emitted_ordinary_type_names_.insert(name);
  if (projected_ordinary_enum_member_names_.contains(name)) {
    XLS_RETURN_IF_ERROR(UpdateOrdinaryEnumNames(ordinary_enum_projections_));
  }
  ordinary_function_type_names_.insert(name);
  allocated_package_names_.insert(name);

  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(), absl::StrFormat("DSLX Type: %s", type->ToString()));
  verilog::DataType* typedef_type = AddNamedType(typedef_identifier, data_type);
  converted_types_.insert({type_annotation, typedef_type});

  return absl::OkStatus();
}

}  // namespace xls::dslx
