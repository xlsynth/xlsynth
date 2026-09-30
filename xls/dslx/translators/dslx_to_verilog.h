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
#include "xls/dslx/translators/verilog_sum_naming.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/name_uniquer.h"

namespace xls::dslx {

// Responsible for managing the conversion of DSLX types to compatible
// Verilog types within a Verilog package.
//
// AddTypeFor* uses a supplied verilog_type_name as the requested typedef
// spelling. An inferred nonparametric sum name is normally its bare DSLX
// declaration name. Colliding declarations are module-qualified when their
// modules are prepared together, and concrete sum specializations get distinct
// names. A DSLX alias of a sum exported as a definition uses its declared alias.
// For sum exports, verilog_type_name is fixed after SystemVerilog
// identifier sanitization. Fixed aliases for ordinary function parameters and
// outputs can also conflict with names planned for actual SystemVerilog sum
// declarations or their generated companions, even before they are emitted.
// A fixed-name conflict returns an error without changing the emitted package.
// Ordinary types otherwise retain their existing naming and alias behavior.
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
  // If verilog_type_name is omitted, anonymous types use
  // <function_name>_<parameter_name>_t and type references normally use their
  // DSLX name. A supplied name is fixed even for an ordinary type. If it
  // conflicts with a name planned for an actual SystemVerilog sum declaration
  // or its generated companion, the call returns an error without changing the
  // emitted package. See the class comment for sum naming.
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
  // If verilog_type_name is omitted, anonymous types use <function_name>_out_t
  // and type references normally use their DSLX name. A supplied name is fixed
  // even for an ordinary type. If it conflicts with a name planned for an
  // actual SystemVerilog sum declaration or its generated companion, the call
  // returns an error without changing the emitted package. See the class
  // comment for sum naming.
  //
  // Note: func should already be type-checked and type information should
  // be contained in import_data.
  absl::Status AddTypeForFunctionOutput(
      dslx::Function* func, dslx::ImportData* import_data,
      std::optional<std::string_view> verilog_type_name = std::nullopt);

  // Adds a typedef for a type definition to the VerilogFile.
  //
  // If verilog_type_name is omitted, the typedef normally uses the DSLX
  // definition's name, including source type aliases. Ordinary type overrides
  // may be uniquified; see the class comment for fixed sum aliases.
  //
  // Note: def should already be type-checked and type information should be
  // contained in import_data.
  absl::Status AddTypeForTypeDefinition(
      const dslx::TypeDefinition& def, dslx::ImportData* import_data,
      std::optional<std::string_view> verilog_type_name = std::nullopt);

  // Registers root modules and their transitive imports without emitting
  // exports. When a package has multiple independent roots, call this with all
  // of them before any AddTypeFor* operation. The complete graph lets the
  // manager reserve ordinary type and enum-member names and module-qualify
  // colliding inferred sum names independently of root input order. Unambiguous
  // nonparametric sum names normally stay bare. AddTypeFor* prepares a single
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

  // An explicit alias must be honored or rejected. An inferred spelling may
  // yield to the canonical name of the generated family.
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

  struct SumFamily {
    std::unique_ptr<Type> type;
    std::string name;
    // Stable symbol-role keys mapped to reserved package names.
    std::map<std::string, std::string> symbols;
    verilog::DataType* envelope = nullptr;
  };
  enum class SumNameState { kNoSums, kAllEmitted, kHasUnemitted };

  void PrepareSumNames(Module* module, TypeInfo* type_info);
  const SumFamily* FindSumFamily(const SumType& sum) const;
  SumNameState GetSumNameState(const Type& type) const;
  absl::Status PlanSumFamilyNames(
      const SumType& sum, std::string_view specialization,
      int64_t payload_width, SumFamily& family,
      const std::function<std::string(std::string_view)>& allocate);
  absl::Status CheckOrdinaryTypeName(const Type& type,
                                     const TypeAnnotation* annotation,
                                     ImportData* import_data,
                                     std::string_view name);
  absl::StatusOr<verilog::DataType*> SumToVastType(
      const SumType& sum, ImportData* import_data,
      std::optional<std::string_view> requested_alias = std::nullopt);
  absl::StatusOr<verilog::DataType*> AddSumAlias(
      const SumType& sum, std::string_view identifier, ImportData* import_data,
      const TypeAlias* source_alias = nullptr);
  verilog::DataType* AddNamedType(std::string_view identifier,
                                  verilog::DataType* type);
  verilog::Def* MakeMember(std::string_view identifier,
                           verilog::DataType* type);
  std::string NewSumName(std::string_view identifier);
  std::string NominalName(const AstNode& node,
                          std::string_view identifier) const;
  absl::StatusOr<std::map<std::string, std::string>> CompanionNameRequests(
      const SumType& sum, std::string_view family);
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
  verilog_sum::IdentityBuilder sum_identities_;
  std::set<const Module*> prepared_sum_modules_;
  // Possible ordinary names and planned generated names are kept distinct from
  // declarations actually emitted. A reservation never changes ordinary output.
  std::set<std::string> legacy_package_names_;
  std::set<std::string> allocated_package_names_;
  std::set<std::string> emitted_sum_names_;
  absl::flat_hash_map<std::string, std::vector<const AstNode*>>
      legacy_name_owners_;
  absl::flat_hash_map<const AstNode*, std::string> nominal_names_;
  std::set<std::string> emitted_ordinary_type_names_;
  absl::flat_hash_map<const EnumDef*, std::vector<std::string>>
      legacy_enum_member_names_;
  absl::flat_hash_map<const SumDef*, std::string> nominal_sum_names_;
  absl::flat_hash_map<const SumDef*, std::map<std::string, std::string>>
      nominal_sum_symbols_;
  absl::flat_hash_map<const SumDef*, std::map<std::string, std::string>>
      sum_specialization_owners_;
  absl::flat_hash_map<const SumDef*, std::vector<std::unique_ptr<SumFamily>>>
      sum_families_;
  absl::flat_hash_map<std::string, verilog::DataType*> sum_aliases_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_TRANSLATORS_DSLX_TO_VERILOG_H_
