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

#ifndef XLS_DSLX_TRANSLATORS_VERILOG_SUM_NAMING_H_
#define XLS_DSLX_TRANSLATORS_VERILOG_SUM_NAMING_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/name_uniquer.h"

namespace xls::dslx::verilog_sum {

// Deterministic names and identities used by the SystemVerilog sum translator.
// Package-wide collision handling and declaration registration stay with the
// manager; only MemberName reserves a spelling, in the caller-provided scope.
std::string MemberName(NameUniquer& names, std::string_view name);

// Qualifies the original DSLX identifier with its module name for diagnostics
// and declaration identity, without converting it to a Verilog identifier.
std::string SourceName(const AstNode& node, std::string_view identifier);

// Reversibly escapes an arbitrary component into Verilog identifier characters.
std::string EscapeName(std::string_view part);

// Returns the internal ownership key for an enum declaration's companion.
std::string EnumCompanionKey(const EnumDef& definition);
// Returns the ownership key for a struct's resolved TypeIdentity.
std::string StructCompanionKey(std::string_view identity);
// Returns the ownership key for a signed array-element type of this bit width.
std::string SignedArrayCompanionKey(int64_t width);

// Builds stable concrete-type keys and bounded public specialization suffixes.
// Keep one instance per output package so shared compact nominal descriptions
// are visited once, without expanding phantom arguments into physical Types.
// Unresolved or unsupported arguments return an error.
// A token type can identify an unused type argument; a value argument
// containing a token has no stable identity and is rejected, even inside a
// tuple or array.
class IdentityBuilder {
 public:
  using NominalOwner = std::function<std::string_view(const AstNode&)>;

  IdentityBuilder();
  ~IdentityBuilder();
  IdentityBuilder(IdentityBuilder&&) noexcept;
  IdentityBuilder& operator=(IdentityBuilder&&) noexcept;
  IdentityBuilder(const IdentityBuilder&) = delete;
  IdentityBuilder& operator=(const IdentityBuilder&) = delete;

  // The key is a fixed-length semantic fingerprint, not a public spelling.
  absl::StatusOr<std::string> TypeIdentity(const Type& type);

  // Uses these nominal source-owner spellings for readable and hashed public
  // suffixes only; TypeIdentity continues to use compiler module identities.
  // Configure before requesting public names. The callback must keep any
  // returned view alive until it is called again.
  void SetPublicNominalOwner(NominalOwner owner);

  // An unambiguous short specialization keeps its readable suffix; otherwise
  // the suffix contains a complete fingerprint using public nominal owners.
  // Empty arguments have an empty suffix. Callers prepend the declaration's
  // public name.
  absl::StatusOr<std::string> SpecializationName(const SumType& sum);
  absl::StatusOr<std::string> StructSpecializationName(const StructType& type);

  // Counts fingerprint computations attempted, excluding cache hits for
  // equivalent records and sums. Allows regression checks for repeated
  // generic type graphs and aggregate argument values.
  size_t sum_identity_computations_for_testing() const;
  size_t struct_identity_computations_for_testing() const;
  size_t value_identity_computations_for_testing() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Impl> public_impl_;
};

// Convenience wrappers for a single independent query. Package generation
// should use one IdentityBuilder instead.
absl::StatusOr<std::string> TypeIdentity(const Type& type);
absl::StatusOr<std::string> SpecializationName(const SumType& sum);
absl::StatusOr<std::string> StructSpecializationName(const StructType& type);

// Assigns variant suffixes in source-name order so declaration order cannot
// decide collisions between spellings that normalize to the same suffix.
std::map<std::string, std::string> VariantSuffixes(const SumDef& sum);

// Maps stable symbol-role keys to preferred public names. The package allocator
// resolves collisions in key order. Tag-only sums omit payload and view names.
std::map<std::string, std::string> FamilyNameRequests(const SumDef& sum,
                                                      std::string_view family,
                                                      bool has_payload);

}  // namespace xls::dslx::verilog_sum

#endif  // XLS_DSLX_TRANSLATORS_VERILOG_SUM_NAMING_H_
