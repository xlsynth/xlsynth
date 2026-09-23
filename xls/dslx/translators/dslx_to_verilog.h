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

#ifndef XLS_DSLX_TRANSLATORS_DSLX_TO_VERILOG_H_
#define XLS_DSLX_TRANSLATORS_DSLX_TO_VERILOG_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/codegen/vast/vast.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/name_uniquer.h"

namespace xls::dslx {

// Converts DSLX types to SystemVerilog types in a single package. A DSLX sum
// declaration and its concrete parametric arguments identify one generated
// family: its packed envelope, tag, payload views, and helper functions.
// Aliases refer to that envelope; they do not generate a second family.
// Repeating an alias for the same family is harmless; requesting an existing
// package name for a different family returns an error instead of renaming it.
// Ordinary struct fields and enum literals retain their standalone spelling
// unless an emitted sum reuses the declaration. Reused struct fields are legal
// and distinct within their struct; enum literals in a reused declaration avoid
// package collisions. A separately generated signed companion does not by
// itself alter ordinary spellings.
class DslxTypeToVerilogManager {
 public:
  // Creates an instance of this manager.
  //
  // package_name will be the name of the Verilog package created via Emit().
  inline static absl::StatusOr<DslxTypeToVerilogManager> Create(
      std::string_view package_name,
      verilog::FileType verilog_type = verilog::FileType::kSystemVerilog) {
    if (verilog_type != verilog::FileType::kSystemVerilog) {
      return absl::UnimplementedError(
          "Verilog file type should be SystemVerilog for DslxTypeToVerilog "
          "conversion");
    }

    return DslxTypeToVerilogManager(package_name);
  }

  // Adds a typedef for the type associated with function func's parameter
  // param_name to the VerilogFile.
  //
  // The type will be named
  //  A. verilog_type_name if given
  //  B. else
  //    1. <function_name>_<parameter_name>_t for anonymous types.
  //    2. <dslx_type_name> for DSLX type references.
  //
  // Note: func should already be type-checked and type information should be
  // contained in import_data.
  absl::Status AddTypeForFunctionParam(
      dslx::Function* func, dslx::ImportData* import_data,
      std::string_view param_name,
      std::optional<std::string_view> verilog_type_name = std::nullopt);

  // Adds a typedef for the type associated with function func's output to
  // the VerilogFile.
  //
  // The type will be named
  //  A. verilog_type_name if given
  //  B. else
  //    1. <function_name>_out_t for anonymous types.
  //    2. <dslx_type_name> for DSLX type references.
  //
  // Note: func should already be type-checked and type information should
  // be contained in import_data.
  absl::Status AddTypeForFunctionOutput(
      dslx::Function* func, dslx::ImportData* import_data,
      std::optional<std::string_view> verilog_type_name = std::nullopt);

  // Adds a typedef for a type definition to the VerilogFile.
  //
  // The type will be named
  //  A. verilog_type_name if given
  //  B. else
  //    1. <function_name>_out_t for anonymous types.
  //    2. <dslx_type_name> for DSLX type references.
  //
  // Note: def should already be type-checked and type information should be
  // contained in import_data.
  absl::Status AddTypeForTypeDefinition(
      const dslx::TypeDefinition& def, dslx::ImportData* import_data,
      std::optional<std::string_view> verilog_type_name = std::nullopt);

  // Registers root modules and their transitive imports without emitting
  // exports. When a package has multiple independent roots, call this with all
  // of them before any AddTypeFor* operation. The complete graph lets the
  // manager reserve ordinary type and enum-member names and assign colliding
  // sum names independently of root input order. AddTypeFor* prepares a single
  // root on demand; roots registered separately or after adding exports do not
  // have the order-independent naming guarantee.
  //
  // Each pair must contain a typechecked module and its root TypeInfo. The
  // manager borrows them; keep the modules and their owning ImportData alive
  // until all exports have been added. Use one ImportData for roots that can
  // reach the same source module, so its declarations each have one identity.
  void PrepareForModules(
      absl::Span<const std::pair<Module*, TypeInfo*>> modules);

  // Emits added DSLX types as a verilog package.
  std::string Emit() const { return file_->Emit(); }

 private:
  explicit DslxTypeToVerilogManager(std::string_view package_name);

  enum class TypeNameOrigin { kInferred, kExplicit };

  // Adds a typedef for the type associated with type_annotation to the
  // VerilogFile.
  //
  // Note: type_annotation should already be type-checked and type information
  // should be contained in import_data.
  absl::Status AddTypeToVerilogPackage(Type* type,
                                       TypeAnnotation* type_annotation,
                                       TypeInfo* type_info,
                                       ImportData* import_data,
                                       std::string_view typedef_identifier,
                                       TypeNameOrigin name_origin);

  // Adds a typedef for the type associated with type_definition to the
  // VerilogFile.
  //
  // Note: type_definition should already be type-checked and type information
  // should be contained in import_data.
  absl::Status AddTypeToVerilogPackage(Type* type,
                                       const TypeDefinition& type_definition,
                                       ImportData* import_data,
                                       std::string_view typedef_identifier,
                                       TypeNameOrigin name_origin);

  // Get Array Bounds and base type.
  //
  // Ordering of the vector is outer-most bound to inner-most. For
  // example, given array type 'bits[32][4][5]' yields {5, 4} as dims and
  // bits[32] as the base type.
  absl::StatusOr<std::pair<std::vector<int64_t>, verilog::DataType*>>
  GetArrayDimsAndBaseType(const Type* type,
                          const ArrayTypeAnnotation* array_type_annotation,
                          ImportData* import_data);

  // Converts a TypeAnnotation to a VAST Verilog type.
  absl::StatusOr<verilog::DataType*> TypeAnnotationToVastType(
      const Type* type, const TypeAnnotation* type_annotation,
      ImportData* import_data);
  // Converts a TypeDefinition to a VAST Verilog type.
  absl::StatusOr<verilog::DataType*> TypeDefinitionToVastType(
      const TypeDefinition& type_definition, ImportData* import_data,
      std::optional<std::string_view> identifier = std::nullopt,
      TypeNameOrigin name_origin = TypeNameOrigin::kInferred);

  // Cached state for one source sum declaration and concrete parametric
  // argument set. The cloned type retains the nominal source declaration. The
  // manager inserts the family before constructing its envelope; encountering a
  // null envelope reports recursive use. Signed enum, struct, and signed array
  // element types are cached within this family.
  struct SumFamily {
    std::unique_ptr<Type> type;
    std::string name;
    // Stable symbol-role keys mapped to reserved package names.
    std::map<std::string, std::string> symbols;
    verilog::DataType* envelope = nullptr;
    absl::flat_hash_map<const EnumDef*, verilog::DataType*> enums;
    absl::flat_hash_map<std::string, verilog::DataType*> structs;
    absl::flat_hash_map<int64_t, verilog::DataType*> signed_array_elements;
  };

  // Prepares one root and its imports on demand for the AddTypeFor* entry
  // points.
  void PrepareSumNames(Module* module, TypeInfo* type_info);

  // Marks the ordinary struct and enum declarations reused by this emitted sum
  // for legal spelling; separately generated companions leave their ordinary
  // declarations intact. Updates previously emitted reused declarations.
  absl::Status MarkSumPayloadNominals(
      const SumType& sum, std::string_view family_name,
      std::optional<std::string_view> pending_alias,
      bool newly_emitted_names_displace_enum_members);

  // Updates a cached ordinary typedef's name in place so existing references
  // see the same declaration. Conflicting typedef names are errors.
  // If no typedef was cached for this struct or enum, there is no update.
  absl::Status ReprojectSumPayloadNominal(const AstNode& nominal);

  // Returns the canonical packed envelope, emitting its family once. Referenced
  // sum families are emitted first; recursive family construction returns an
  // error instead of exposing an unfinished envelope. A requested alias is
  // checked before a new family can change an existing ordinary declaration.
  absl::StatusOr<verilog::DataType*> SumToVastType(
      const SumType& sum, ImportData* import_data,
      std::optional<std::string_view> requested_alias = std::nullopt);

  // Returns a packed payload type that preserves DSLX signedness when selected
  // through a variant view. Reuses ordinary unsigned enums and nonparametric
  // structs with no signed descendant that needs a different exported type.
  absl::StatusOr<verilog::DataType*> SumMemberToVastType(
      const Type& type, SumFamily& family, ImportData* import_data);

  // Adds a typedef for the canonical envelope, or reuses the requested alias
  // when it names that same envelope. A conflicting alias is rejected; the
  // caller cannot otherwise know the actual name of its requested alias.
  absl::StatusOr<verilog::DataType*> AddSumAlias(
      const SumType& sum, std::string_view identifier, ImportData* import_data,
      const TypeAlias* source_alias = nullptr);

  // Adds a package typedef and returns a type that refers to it by name.
  verilog::DataType* AddNamedType(std::string_view identifier,
                                  verilog::DataType* type);

  // Creates a member declaration, using kUser for user-defined types and kLogic
  // otherwise.
  verilog::Def* MakeMember(std::string_view identifier,
                           verilog::DataType* type);

  // Disambiguates source member names only against typedefs later members
  // reference in the same packed scope. Positional members keep their fixed
  // spelling and qualify a subsequent type reference when the fixed member
  // would otherwise hide it in a generated declaration.
  void ProjectSumAggregateMemberNames(absl::Span<verilog::Def* const> fields);
  void ProtectFixedSumMemberNames(absl::Span<verilog::Def* const> fields);
  verilog::DataType* QualifySumTypeNames(
      verilog::DataType* type, const std::set<std::string>& fixed_names);

  // Builds signed or unsigned logic of the requested positive width.
  verilog::DataType* MakeBits(int64_t width, bool is_signed = false);

  // Reserves an available package-level name without colliding with ordinary
  // type or enum-member names.
  std::string NewSumName(std::string_view identifier);

  // Returns the source-qualified stem when another module declares the same
  // source spelling. Unambiguous declarations retain their legacy spelling.
  std::string NominalName(const AstNode& node,
                          std::string_view identifier) const;

  // Collects preferred names for generated types and enum literals needed by
  // nonzero-width payloads so the caller can reserve them in stable order.
  // Nested sums reserve their own names.
  absl::StatusOr<std::map<std::string, std::string>> CompanionNameRequests(
      const SumType& sum, std::string_view family) const;

  // Registers an enum only when its ordinary declaration is emitted; becoming
  // a sum payload can later require the legal projection of the same members.
  absl::Status RegisterOrdinaryEnum(const EnumDef& definition, bool projected);
  // Updates enum declarations reused by sums in place when their projected
  // members would collide. If supplied, a new sum alias is checked against the
  // emitted enum names before applying any change.
  absl::Status UpdateOrdinaryEnumNames(
      const absl::flat_hash_map<const EnumDef*, bool>& projections,
      std::optional<std::string_view> pending_alias = std::nullopt,
      std::string_view family_name = {});
  // Reports names that currently belong to an emitted enum. A new ordinary
  // typedef can displace the name only if a sum reuses the enum.
  bool IsOrdinaryEnumMemberName(std::string_view name,
                                const AstNode* excluded_owner = nullptr) const;
  absl::Status CheckOrdinaryNameAgainstSumAliases(std::string_view name) const;

  // Vast package that contains typedefs for DSLX types.
  verilog::VerilogPackage* top_pkg_;

  // Vast file containing a single package.
  std::unique_ptr<verilog::VerilogFile> file_;

  // Set of already-visited named types. Keys are the nominal type name.
  // Note that this is not the same as the typedef identifier which can have a
  // user-provided override.
  absl::flat_hash_map<AstNode*, verilog::DataType*> converted_types_;

  std::unique_ptr<NameUniquer> typedef_name_uniquer_;
  // Source modules already scanned for declarations and ordinary names.
  std::set<const Module*> prepared_sum_modules_;
  // Potential ordinary typedef and enum-member names found during module
  // preparation that generated names must avoid, regardless of export order.
  std::set<std::string> legacy_package_names_;
  // Names actually allocated to package declarations, including generated ones.
  std::set<std::string> allocated_package_names_;
  // Names of emitted canonical sum families that reused enum literals must
  // avoid. Explicit sum aliases are checked separately because a collision must
  // reject the export; names reserved for un-emitted families do not displace
  // literals.
  std::set<std::string> emitted_sum_names_;
  absl::flat_hash_map<std::string, std::vector<const AstNode*>>
      legacy_name_owners_;
  absl::flat_hash_map<const AstNode*, std::string> nominal_names_;
  std::set<std::string> emitted_ordinary_type_names_;
  absl::flat_hash_map<const EnumDef*, bool> ordinary_enum_projections_;
  absl::flat_hash_map<const EnumDef*, std::vector<std::string>>
      legacy_enum_member_names_;
  // Names actually occupied by enum members in declarations reused by sums.
  std::set<std::string> projected_ordinary_enum_member_names_;
  // Ordinary nominals reached by an actually emitted sum.
  std::set<const AstNode*> sum_payload_nominals_;
  // Ordinary declarations exported under their own source typedef name.
  std::set<const AstNode*> ordinary_source_named_types_;
  // The base name reserved for each source sum, shared by its specializations.
  absl::flat_hash_map<const SumDef*, std::string> nominal_sum_names_;
  absl::flat_hash_map<const SumDef*, std::map<std::string, std::string>>
      nominal_sum_symbols_;
  // One family per source declaration and concrete parametric argument set.
  absl::flat_hash_map<const SumDef*, std::vector<std::unique_ptr<SumFamily>>>
      sum_families_;
  // The alias emitted for each sanitized request. Reuse checks that it names
  // the same canonical envelope; a different owner is an error.
  absl::flat_hash_map<std::string, verilog::DataType*> sum_aliases_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_TRANSLATORS_DSLX_TO_VERILOG_H_
