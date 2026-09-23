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
#include "absl/strings/ascii.h"
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
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/bits.h"
#include "xls/ir/name_uniquer.h"
#include "xls/ir/source_location.h"

namespace xls::dslx {

namespace {

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

absl::StatusOr<int64_t> BitCount(const Type& type) {
  XLS_ASSIGN_OR_RETURN(TypeDim bits, type.GetTotalBitCount());
  return bits.GetAsInt64();
}

absl::StatusOr<bool> NeedsSemanticProjection(const Type& type) {
  if (std::optional<BitsLikeProperties> bits = GetBitsLike(type);
      bits.has_value()) {
    return bits->is_signed.GetAsBool();
  } else if (type.IsEnum()) {
    return type.AsEnum().is_signed();
  } else if (type.IsArray()) {
    return NeedsSemanticProjection(type.AsArray().element_type());
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

std::string SnakeCase(std::string_view identifier) {
  std::string result;
  for (int64_t i = 0; i < identifier.size(); ++i) {
    char c = identifier[i];
    if (i != 0 && absl::ascii_isupper(c) &&
        (absl::ascii_islower(identifier[i - 1]) ||
         absl::ascii_isdigit(identifier[i - 1]) ||
         (i + 1 < identifier.size() && absl::ascii_islower(identifier[i + 1]) &&
          identifier[i - 1] != '_'))) {
      result.push_back('_');
    }
    result.push_back(absl::ascii_tolower(c));
  }
  return result;
}

std::string MemberName(NameUniquer& names, std::string_view name) {
  return names.GetSanitizedUniqueName(verilog::SanitizeVerilogIdentifier(name));
}

std::string SpecializationName(const SumType& sum,
                               const FileTable& file_table) {
  std::string result;
  for (const SumType::ParametricArgument& argument :
       sum.parametric_arguments()) {
    std::string part = std::visit(
        Visitor{[](const InterpValue& value) { return value.ToString(); },
                [&](const std::unique_ptr<const Type>& type) {
                  return type->ToStringFullyQualified(file_table);
                }},
        argument);
    absl::StrAppend(&result, "__");
    for (unsigned char c : part) {
      if (absl::ascii_isalnum(c)) {
        result.push_back(c);
      } else {
        absl::StrAppendFormat(&result, "_%02x_", c);
      }
    }
  }
  return result;
}

}  // namespace

void DslxTypeToVerilogManager::PrepareSumNames(Module* module,
                                               TypeInfo* type_info) {
  if (!prepared_sum_modules_.contains(module)) {
    const std::pair<Module*, TypeInfo*> input{module, type_info};
    PrepareForModules(
        absl::Span<const std::pair<Module*, TypeInfo*>>(&input, 1));
  }
}

void DslxTypeToVerilogManager::PrepareForModules(
    absl::Span<const std::pair<Module*, TypeInfo*>> modules) {
  std::map<std::string, std::vector<SumDef*>> definitions;
  std::set<const Module*> visited;
  std::vector<std::pair<Module*, TypeInfo*>> pending(modules.begin(),
                                                     modules.end());
  while (!pending.empty()) {
    auto [current, current_info] = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    prepared_sum_modules_.insert(current);
    for (const TypeDefinition& definition : current->GetTypeDefinitions()) {
      AstNode* node = TypeDefinitionToAstNode(definition);
      if (auto* sum = dynamic_cast<SumDef*>(node);
          sum != nullptr && !nominal_sum_names_.contains(sum)) {
        definitions[verilog::SanitizeVerilogIdentifier(sum->identifier())]
            .push_back(sum);
      } else if (dynamic_cast<SumDef*>(node) == nullptr) {
        AnyNameDef name = TypeDefinitionGetNameDef(definition);
        std::string identifier = absl::visit(
            [](const auto* value) { return value->identifier(); }, name);
        legacy_package_names_.insert(
            verilog::SanitizeVerilogIdentifier(identifier));
        if (auto* ordinary_enum = dynamic_cast<EnumDef*>(node)) {
          for (int64_t i = 0; i < ordinary_enum->values().size(); ++i) {
            legacy_package_names_.insert(verilog::SanitizeVerilogIdentifier(
                ordinary_enum->GetMemberName(i)));
          }
        }
      }
    }
    for (const auto& [subject, imported] : current_info->GetRootImports()) {
      pending.emplace_back(imported.module, imported.type_info);
    }
  }
  for (auto& [name, sums] : definitions) {
    std::sort(sums.begin(), sums.end(), [](const SumDef* a, const SumDef* b) {
      return a->owner()->name() < b->owner()->name();
    });
    for (SumDef* sum : sums) {
      nominal_sum_names_.emplace(sum, NewSumName(name));
    }
  }
}

std::string DslxTypeToVerilogManager::NewSumName(std::string_view identifier) {
  std::string base = verilog::SanitizeVerilogIdentifier(identifier);
  std::string candidate = base;
  int64_t suffix = 0;
  while (true) {
    if (!legacy_package_names_.contains(candidate)) {
      std::string unique =
          typedef_name_uniquer_->GetSanitizedUniqueName(candidate);
      if (!legacy_package_names_.contains(unique)) {
        return unique;
      } else {
        CHECK_OK(typedef_name_uniquer_->ReleaseIdentifier(unique));
      }
    }
    candidate = absl::StrCat(base, "__", ++suffix);
  }
}

verilog::DataType* DslxTypeToVerilogManager::MakeBits(int64_t width,
                                                      bool is_signed) {
  if (width == 1) {
    return file_->Make<verilog::ScalarType>(SourceInfo(), is_signed);
  } else {
    return file_->Make<verilog::BitVectorType>(SourceInfo(), width, is_signed);
  }
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
      std::string prefix =
          absl::StrCat(family.name, "_", enum_type.nominal_type().identifier());
      auto* definition =
          file_->Make<verilog::Enum>(SourceInfo(), verilog::DataKind::kLogic,
                                     MakeBits(width, enum_type.is_signed()));
      for (int64_t i = 0; i < enum_type.members().size(); ++i) {
        XLS_ASSIGN_OR_RETURN(Bits bits, enum_type.members()[i].GetBits());
        definition->AddMember(
            NewSumName(absl::StrCat(prefix, "_enum_",
                                    enum_type.nominal_type().GetMemberName(i))),
            file_->Literal(bits, SourceInfo()), SourceInfo());
      }
      verilog::DataType* named = AddNamedType(
          NewSumName(absl::StrCat(prefix, "_value_t")), definition);
      family.enums.emplace(&enum_type.nominal_type(), named);
      return named;
    }
  } else if (type.IsArray()) {
    const ArrayType& array = type.AsArray();
    XLS_ASSIGN_OR_RETURN(int64_t size, array.size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(
        verilog::DataType * element,
        SumMemberToVastType(array.element_type(), family, import_data));
    // A packed dimension placed directly on signed logic makes the aggregate
    // signed, but indexing that dimension produces an unsigned vector. A named
    // signed element retains its type when an SV user selects one array item.
    if (std::optional<BitsLikeProperties> bits =
            GetBitsLike(array.element_type());
        bits.has_value()) {
      XLS_ASSIGN_OR_RETURN(bool is_signed, bits->is_signed.GetAsBool());
      if (is_signed) {
        XLS_ASSIGN_OR_RETURN(int64_t width, bits->size.GetAsInt64());
        auto existing = family.signed_array_elements.find(width);
        if (existing == family.signed_array_elements.end()) {
          element = AddNamedType(
              NewSumName(absl::StrCat(family.name, "_s", width, "_value_t")),
              element);
          family.signed_array_elements.emplace(width, element);
        } else {
          element = existing->second;
        }
      }
    }
    return file_->Make<verilog::PackedArrayType>(
        SourceInfo(), element, std::vector<int64_t>{size}, false);
  } else if (type.IsStruct() || type.IsTuple()) {
    bool is_struct = type.IsStruct();
    std::optional<std::string> semantic_struct_key;
    if (is_struct) {
      const StructType& record = type.AsStruct();
      XLS_ASSIGN_OR_RETURN(bool needs_projection,
                           NeedsSemanticProjection(type));
      if (!needs_projection &&
          record.nominal_type().parametric_bindings().empty()) {
        return TypeDefinitionToVastType(
            const_cast<StructDef*>(&record.nominal_type()), import_data);
      } else {
        semantic_struct_key =
            type.ToStringFullyQualified(import_data->file_table());
        auto known = family.structs.find(*semantic_struct_key);
        if (known != family.structs.end()) {
          return known->second;
        }
      }
    }
    int64_t count = is_struct ? type.AsStruct().size() : type.AsTuple().size();
    std::vector<verilog::Def*> members;
    NameUniquer names("__");
    for (int64_t i = 0; i < count; ++i) {
      const Type& member = is_struct ? type.AsStruct().GetMemberType(i)
                                     : type.AsTuple().GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(int64_t width, BitCount(member));
      if (width > 0) {
        std::string name = is_struct
                               ? std::string(type.AsStruct().GetMemberName(i))
                               : absl::StrCat("index_", i);
        XLS_ASSIGN_OR_RETURN(verilog::DataType * member_type,
                             SumMemberToVastType(member, family, import_data));
        members.push_back(MakeMember(MemberName(names, name), member_type));
      }
    }
    verilog::DataType* aggregate =
        file_->Make<verilog::Struct>(SourceInfo(), members);
    if (semantic_struct_key.has_value()) {
      std::string prefix =
          absl::StrCat(family.name, "_",
                       type.AsStruct().nominal_type().identifier(), "_value_t");
      aggregate = AddNamedType(NewSumName(prefix), aggregate);
      family.structs.emplace(*semantic_struct_key, aggregate);
    }
    return aggregate;
  } else {
    return absl::UnimplementedError(absl::StrFormat(
        "Unsupported DSLX sum payload type for SystemVerilog: %s",
        type.ToString()));
  }
}

absl::StatusOr<verilog::DataType*> DslxTypeToVerilogManager::SumToVastType(
    const SumType& sum, ImportData* import_data) {
  for (const std::unique_ptr<SumFamily>& existing : sum_families_) {
    const SumType& known = existing->type->AsSum();
    if (&known.nominal_type() == &sum.nominal_type() &&
        known.HasSameParametricArguments(sum.parametric_arguments())) {
      XLS_RET_CHECK(existing->envelope != nullptr)
          << "Recursive SystemVerilog sum: " << sum.nominal_type().identifier();
      return existing->envelope;
    }
  }
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
  if (!sum.parametric_arguments().empty()) {
    owner->name = NewSumName(absl::StrCat(
        owner->name, SpecializationName(sum, import_data->file_table())));
  }
  SumFamily& family = *owner;
  sum_families_.push_back(std::move(owner));

  struct VariantProjection {
    std::string suffix;
    std::string constructor;
    verilog::DataType* view = nullptr;
    int64_t padding_width = 0;
    std::vector<std::pair<std::string, verilog::DataType*>> fields;
  };
  std::vector<VariantProjection> projections;
  NameUniquer variant_names("__");
  for (const SumTypeVariant& variant : sum.variants()) {
    VariantProjection projection;
    // Prefixes make a keyword like Byte safe without distorting the spelling
    // exposed after as_ or make_. Distinct source spellings can normalize
    // alike.
    projection.suffix = variant_names.GetSanitizedUniqueName(
        SnakeCase(variant.variant().identifier()));
    projection.constructor =
        NewSumName(absl::StrCat(family.name, "_make_", projection.suffix));
    XLS_ASSIGN_OR_RETURN(TypeDim variant_dim, variant.GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t variant_width, variant_dim.GetAsInt64());
    projection.padding_width = payload_width - variant_width;
    NameUniquer field_names("__");
    for (int64_t i = 0; i < variant.size(); ++i) {
      const Type& member = variant.GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(int64_t member_width, BitCount(member));
      if (member_width > 0) {
        std::string member_name =
            variant.is_struct()
                ? std::string(variant.GetMemberName(i))
                : (variant.size() == 1 ? "value" : absl::StrCat("index_", i));
        XLS_ASSIGN_OR_RETURN(verilog::DataType * member_type,
                             SumMemberToVastType(member, family, import_data));
        projection.fields.emplace_back(MemberName(field_names, member_name),
                                       member_type);
      }
    }
    if (payload_width > 0) {
      std::vector<verilog::Def*> fields;
      if (projection.padding_width > 0) {
        fields.push_back(MakeMember(MemberName(field_names, "xls_padding"),
                                    MakeBits(projection.padding_width)));
      }
      for (const auto& [name, type] : projection.fields) {
        fields.push_back(MakeMember(name, type));
      }
      // The declarations themselves follow the tag below; obtaining member
      // types first allows nested nominal families to be emitted before us.
      projection.view = file_->Make<verilog::Struct>(SourceInfo(), fields);
    }
    projections.push_back(std::move(projection));
  }

  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(), absl::StrCat("DSLX Type: ", sum.nominal_type().ToString()));
  bool signed_tag = tag_width > 0 && !sum.variants().empty() &&
                    sum.GetDiscriminant(0).IsSBits();
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
        NewSumName(absl::StrCat(family.name, "_tag_",
                                sum.variants()[i].variant().identifier())),
        file_->Literal(tag_bits, SourceInfo()), SourceInfo()));
  }
  verilog::DataType* tag_type = AddNamedType(
      NewSumName(absl::StrCat(family.name, "_tag_t")), tag_definition);

  std::vector<verilog::Def*> envelope_members;
  if (tag_width > 0) {
    envelope_members.push_back(MakeMember("tag", tag_type));
  }
  if (payload_width > 0) {
    std::vector<verilog::Def*> union_members{
        MakeMember("bits", MakeBits(payload_width))};
    for (VariantProjection& projection : projections) {
      projection.view =
          AddNamedType(NewSumName(absl::StrCat(family.name, "_",
                                               projection.suffix, "_view_t")),
                       projection.view);
      union_members.push_back(
          MakeMember(absl::StrCat("as_", projection.suffix), projection.view));
    }
    verilog::DataType* payload_type =
        AddNamedType(NewSumName(absl::StrCat(family.name, "_payload_t")),
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
    if (projection.padding_width > 0) {
      parts.push_back(
          file_->Literal(UBits(0, projection.padding_width), SourceInfo()));
    }
    for (const auto& [name, type] : projection.fields) {
      parts.push_back(
          constructor->AddArgument(MakeMember(name, type), SourceInfo()));
    }
    XLS_RET_CHECK(!parts.empty());
    verilog::Expression* bits =
        parts.size() == 1 ? parts.front() : file_->Concat(parts, SourceInfo());
    constructor->AddStatement<verilog::BlockingAssignment>(
        SourceInfo(), constructor->return_value_ref(),
        file_->Make<verilog::TypeCast>(SourceInfo(), family.envelope, bits));
  }
  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  auto* getter = top_pkg_->Add<verilog::VerilogFunction>(
      SourceInfo(), NewSumName(absl::StrCat(family.name, "_get_tag")),
      tag_type);
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
    const SumType& sum, std::string_view identifier, ImportData* import_data) {
  XLS_ASSIGN_OR_RETURN(verilog::DataType * canonical,
                       SumToVastType(sum, import_data));
  std::string canonical_name =
      static_cast<verilog::TypedefType*>(canonical)->type_def()->GetName();
  std::string requested = verilog::SanitizeVerilogIdentifier(identifier);
  auto known = sum_aliases_.find(requested);
  if (requested == canonical_name) {
    return canonical;
  } else if (known != sum_aliases_.end() &&
             static_cast<verilog::TypedefType*>(known->second)
                     ->type_def()
                     ->data_type() == canonical) {
    return known->second;
  } else {
    top_pkg_->Add<verilog::BlankLine>(SourceInfo());
    top_pkg_->Add<verilog::Comment>(
        SourceInfo(),
        absl::StrCat("DSLX Type: ", sum.ToString(),
                     "; SystemVerilog sum family: ", canonical_name));
    std::string alias_name =
        legacy_package_names_.contains(requested)
            ? typedef_name_uniquer_->GetSanitizedUniqueName(requested)
            : NewSumName(requested);
    verilog::DataType* alias = AddNamedType(alias_name, canonical);
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
    std::vector<verilog::Def*> struct_members;

    for (int64_t i = 0; i < tuple_type.size(); ++i) {
      const TypeAnnotation* element_type_annotation = ta->members().at(i);
      const Type& element_type = tuple_type.GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(TypeDim element_bit_count,
                           element_type.GetTotalBitCount());
      XLS_ASSIGN_OR_RETURN(int64_t concrete_element_bit_count,
                           element_bit_count.GetAsInt64());
      if (concrete_element_bit_count == 0) {
        continue;
      }
      XLS_ASSIGN_OR_RETURN(
          verilog::DataType * element_data_type,
          TypeAnnotationToVastType(&element_type, element_type_annotation,
                                   import_data));

      struct_members.push_back(file_->Make<verilog::Def>(
          SourceInfo(), absl::StrFormat("index_%d", i),
          element_data_type->IsUserDefined() ? verilog::DataKind::kUser
                                             : verilog::DataKind::kLogic,
          element_data_type));
    }

    return file_->Make<verilog::Struct>(SourceInfo(), struct_members);
  }

  if (auto ta = dynamic_cast<const TypeRefTypeAnnotation*>(type_annotation)) {
    AnyNameDef name_def =
        TypeDefinitionGetNameDef(ta->type_ref()->type_definition());
    std::string identifier = absl::visit(
        Visitor{[](const NameDef* name_def) { return name_def->identifier(); },
                [](const BuiltinNameDef* name_def) {
                  return name_def->identifier();
                }},
        name_def);
    return TypeDefinitionToVastType(ta->type_ref()->type_definition(),
                                    import_data, identifier);
  }

  return absl::InternalError(absl::StrFormat(
      "TypeAnnotation Misc %s not supported by DslxTypeToVerilogManager",
      type_annotation->ToString()));
}

absl::StatusOr<verilog::DataType*>
DslxTypeToVerilogManager::TypeDefinitionToVastType(
    const TypeDefinition& type_definition, ImportData* import_data,
    std::optional<std::string_view> identifier) {
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
    if (is_nominal && (!identifier.has_value() ||
                       *identifier == sum.nominal_type().identifier())) {
      XLS_ASSIGN_OR_RETURN(result, SumToVastType(sum, import_data));
    } else {
      std::optional<std::string_view> requested =
          identifier.has_value()
              ? identifier
              : TypeDefinitionIdentifier(resolved_type_definition_source);
      XLS_RET_CHECK(requested.has_value());
      XLS_ASSIGN_OR_RETURN(result, AddSumAlias(sum, *requested, import_data));
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

  std::string typedef_identifier =
      typedef_name_uniquer_->GetSanitizedUniqueName(*identifier);

  auto iter = converted_types_.find(type_definition_node);
  if (type_definition_name.has_value() && iter != converted_types_.end()) {
    return iter->second;
  }

  VLOG(3) << "Converting TypeDefinition " << type_definition_node->ToString()
          << " with concrete Type to Verilog: " << *type;

  XLS_ASSIGN_OR_RETURN(
      verilog::DataType * data_type,
      absl::visit(
          Visitor{
              [&](TypeAlias* alias) -> absl::StatusOr<verilog::DataType*> {
                XLS_ASSIGN_OR_RETURN(
                    TypeInfo * alias_type_info,
                    import_data->GetRootTypeInfoForNode(alias));
                std::optional<Type*> alias_type =
                    alias_type_info->GetItem(alias);
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
                std::vector<verilog::Def*> vast_struct_members;

                XLS_RET_CHECK(type->IsStruct());
                const StructType& struct_type = type->AsStruct();

                VLOG(3) << "Converting struct type to Verilog: " << struct_type
                        << " size " << struct_type.size();

                for (int64_t i = 0; i < struct_type.size(); ++i) {
                  std::string_view member_name = struct_type.GetMemberName(i);
                  const TypeAnnotation* member_type_annotation =
                      struct_def->members().at(i)->type();
                  const Type& member_type = struct_type.GetMemberType(i);
                  XLS_ASSIGN_OR_RETURN(TypeDim member_bit_count,
                                       member_type.GetTotalBitCount());
                  XLS_ASSIGN_OR_RETURN(int64_t concrete_member_bit_count,
                                       member_bit_count.GetAsInt64());
                  if (concrete_member_bit_count == 0) {
                    continue;
                  }

                  XLS_ASSIGN_OR_RETURN(
                      verilog::DataType * element_data_type,
                      TypeAnnotationToVastType(
                          &member_type, member_type_annotation, import_data));

                  vast_struct_members.push_back(file_->Make<verilog::Def>(
                      SourceInfo(), member_name,
                      element_data_type->IsUserDefined()
                          ? verilog::DataKind::kUser
                          : verilog::DataKind::kLogic,
                      element_data_type));
                }

                return file_->Make<verilog::Struct>(SourceInfo(),
                                                    vast_struct_members);
              },
              [&](ProcDef* proc_def) -> absl::StatusOr<verilog::DataType*> {
                return absl::InternalError(absl::StrFormat(
                    "TypeAnnotation ProcDef %s not supported by "
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

                verilog::DataType* vast_enum_data_type;
                if (size == 1) {
                  vast_enum_data_type =
                      file_->Make<verilog::ScalarType>(SourceInfo());
                } else {
                  vast_enum_data_type = file_->Make<verilog::BitVectorType>(
                      SourceInfo(), size, false);
                }

                verilog::Enum* vast_enum_def = file_->Make<verilog::Enum>(
                    SourceInfo(), verilog::DataKind::kLogic,
                    vast_enum_data_type);

                for (int64_t i = 0; i < enum_def->values().size(); ++i) {
                  const std::string& member_name = enum_def->GetMemberName(i);
                  const InterpValue& member_val = enum_type.members().at(i);

                  XLS_ASSIGN_OR_RETURN(Bits member_val_as_bits,
                                       member_val.GetBits());
                  verilog::Literal* vast_literal =
                      file_->Literal(member_val_as_bits, SourceInfo());
                  vast_enum_def->AddMember(member_name, vast_literal,
                                           SourceInfo());
                }

                return vast_enum_def;
              },
              [&](SumDef*) -> absl::StatusOr<verilog::DataType*> {
                return absl::InternalError(
                    "Sum type was not resolved as a sum");
              },
          },
          resolved_type_definition_source.definition));

  // Add typedef to the verilog file.
  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(),
      absl::StrFormat("DSLX Type: %s",
                      TypeDefinitionToAstNode(type_definition)->ToString()));

  verilog::Typedef* typedef_ = top_pkg_->Add<verilog::Typedef>(
      SourceInfo(), file_->Make<verilog::Def>(SourceInfo(), typedef_identifier,
                                              data_type->IsUserDefined()
                                                  ? verilog::DataKind::kUser
                                                  : verilog::DataKind::kLogic,
                                              data_type));
  verilog::DataType* typedef_type =
      file_->Make<verilog::TypedefType>(SourceInfo(), typedef_);
  converted_types_.insert({type_definition_node, typedef_type});
  return typedef_type;
}

DslxTypeToVerilogManager::DslxTypeToVerilogManager(
    std::string_view package_name)
    : file_(std::make_unique<verilog::VerilogFile>(
          verilog::FileType::kSystemVerilog)),
      typedef_name_uniquer_(std::make_unique<NameUniquer>("__")) {
  top_pkg_ = file_->AddVerilogPackage(package_name, SourceInfo());
}

absl::Status DslxTypeToVerilogManager::AddTypeForFunctionParam(
    dslx::Function* func, dslx::ImportData* import_data,
    std::string_view param_name,
    std::optional<std::string_view> verilog_type_name) {
  if (!func->parametric_bindings().empty()) {
    return absl::UnimplementedError(absl::StrFormat(
        "Unable to convert function %s with parametric bindings",
        func->identifier()));
  }

  XLS_ASSIGN_OR_RETURN(TypeInfo * func_type_info,
                       import_data->GetRootTypeInfoForNode(func));

  PrepareSumNames(func->owner(), func_type_info);

  XLS_ASSIGN_OR_RETURN(Param * param, func->GetParamByName(param_name));

  TypeAnnotation* type_annotation = param->type_annotation();
  XLS_ASSIGN_OR_RETURN(
      Type * type, GetActualType(type_annotation, func_type_info, import_data));

  std::string typedef_identifier =
      verilog_type_name.has_value()
          ? std::string(*verilog_type_name)
          : GetVerilogTypedefIdentifier(type_annotation, func->identifier(),
                                        param->identifier());

  return AddTypeToVerilogPackage(type, type_annotation, func_type_info,
                                 import_data, typedef_identifier);
}

absl::Status DslxTypeToVerilogManager::AddTypeForFunctionOutput(
    dslx::Function* func, dslx::ImportData* import_data,
    std::optional<std::string_view> verilog_type_name) {
  if (!func->parametric_bindings().empty()) {
    return absl::UnimplementedError(absl::StrFormat(
        "Unable to convert function %s with parametric bindings",
        func->identifier()));
  }

  XLS_ASSIGN_OR_RETURN(TypeInfo * func_type_info,
                       import_data->GetRootTypeInfoForNode(func));

  PrepareSumNames(func->owner(), func_type_info);

  // Create a typedef for the return type, named
  //  1. <function_name>_out_t for anonymous types.
  //  2. <dslx_type_name> for DSLX type references.
  TypeAnnotation* return_type_annotation = func->return_type();
  XLS_ASSIGN_OR_RETURN(
      Type * return_type,
      GetActualType(return_type_annotation, func_type_info, import_data));

  std::string typedef_identifier =
      verilog_type_name.has_value()
          ? std::string(*verilog_type_name)
          : GetVerilogTypedefIdentifier(return_type_annotation,
                                        func->identifier(), "out");

  return AddTypeToVerilogPackage(return_type, return_type_annotation,
                                 func_type_info, import_data,
                                 typedef_identifier);
}

absl::Status DslxTypeToVerilogManager::AddTypeForTypeDefinition(
    const dslx::TypeDefinition& def, dslx::ImportData* import_data,
    std::optional<std::string_view> verilog_type_name) {
  AstNode* node = TypeDefinitionToAstNode(def);
  XLS_ASSIGN_OR_RETURN(TypeInfo * type_info,
                       import_data->GetRootTypeInfoForNode(node));
  PrepareSumNames(node->owner(), type_info);
  std::string identifier = std::string(verilog_type_name.value_or(""));
  if (identifier.empty()) {
    AnyNameDef name_def = TypeDefinitionGetNameDef(def);
    identifier = absl::visit(
        Visitor{[](const NameDef* name_def) { return name_def->identifier(); },
                [](const BuiltinNameDef* name_def) {
                  return name_def->identifier();
                }},
        name_def);
  }
  XLS_ASSIGN_OR_RETURN(Type * tpe, GetActualType(node, type_info, import_data));
  return AddTypeToVerilogPackage(tpe, def, import_data, identifier);
}

absl::Status DslxTypeToVerilogManager::AddTypeToVerilogPackage(
    Type* type, const TypeDefinition& type_definition, ImportData* import_data,
    std::string_view typedef_identifier) {
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
                                  typedef_identifier)
      .status();
}

absl::Status DslxTypeToVerilogManager::AddTypeToVerilogPackage(
    Type* type, TypeAnnotation* type_annotation, TypeInfo* type_info,
    ImportData* import_data, std::string_view typedef_identifier) {
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
    bool is_source_name = typedef_identifier == type_annotation->ToString();
    bool is_alias = definition.has_value() &&
                    dynamic_cast<TypeAlias*>(*definition) != nullptr;
    if (is_source_name && !is_alias) {
      return SumToVastType(type->AsSum(), import_data).status();
    } else {
      return AddSumAlias(type->AsSum(), typedef_identifier, import_data)
          .status();
    }
  }

  // Add typedef to the verilog file.
  XLS_ASSIGN_OR_RETURN(
      verilog::DataType * data_type,
      TypeAnnotationToVastType(type, type_annotation, import_data));

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

  top_pkg_->Add<verilog::BlankLine>(SourceInfo());
  top_pkg_->Add<verilog::Comment>(
      SourceInfo(), absl::StrFormat("DSLX Type: %s", type->ToString()));
  verilog::Typedef* typedef_ = top_pkg_->Add<verilog::Typedef>(
      SourceInfo(), file_->Make<verilog::Def>(SourceInfo(), typedef_identifier,
                                              data_type->IsUserDefined()
                                                  ? verilog::DataKind::kUser
                                                  : verilog::DataKind::kLogic,
                                              data_type));

  verilog::DataType* typedef_type =
      file_->Make<verilog::TypedefType>(SourceInfo(), typedef_);
  converted_types_.insert({type_annotation, typedef_type});

  return absl::OkStatus();
}

}  // namespace xls::dslx
