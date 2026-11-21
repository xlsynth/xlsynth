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

#include "xls/dslx/mangle.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_specialization.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {
namespace {

using type_system_internal::BindingKind;
using type_system_internal::NominalPart;
using type_system_internal::SpecializationKind;
using type_system_internal::SpecializationPart;

// MangleType grammar. N is a nonnegative decimal integer terminated by '_'.
// I is N followed by exactly that many escaped identifier bytes. [X] is a
// count N followed by that many Xs. T is one of:
//
//   u N | s N                    unsigned or signed bits
//   a N T | q [T]                array or tuple
//   e I I (u | s) N              enum (module, declaration, underlying bits)
//   (r | p) I I [I B]            struct or proc (bindings sorted by name)
//   z I I [B]                    sum (bindings in declaration order)
//   f [T] T                      function (parameters, return)
//   (ci | co) T | m T            channel or meta-type
//   d I | token                  module or token
//   b N                          reference to an earlier type containing a sum
//
// A nominal binding B is t T (type), v N H (bits or enum, width then
// unsigned hexadecimal digits terminated by '_'), q [B] (tuple), or a [B]
// (array). Each child ends where its grammar says, so it needs no length
// prefix. For example, u32[4] is a4_u32_.
//
// Within one MangleType result, number the first occurrences of sums and
// types containing them (including through bindings), starting at zero in
// preorder. Each subsequent occurrence uses b followed by its number.
// For example, (S, S) for sum S in module M is q2_z1_M1_S0_b1_.
std::string_view KindTag(SpecializationKind kind) {
  switch (kind) {
    case SpecializationKind::kUnsignedBits:
      return "u";
    case SpecializationKind::kSignedBits:
      return "s";
    case SpecializationKind::kEnum:
      return "e";
    case SpecializationKind::kProc:
      return "p";
    case SpecializationKind::kStruct:
      return "r";
    case SpecializationKind::kTypeBinding:
      return "t";
    case SpecializationKind::kSum:
      return "z";
    case SpecializationKind::kTuple:
      return "q";
    case SpecializationKind::kArray:
      return "a";
    case SpecializationKind::kFunction:
      return "f";
    case SpecializationKind::kChannelIn:
      return "ci";
    case SpecializationKind::kChannelOut:
      return "co";
    case SpecializationKind::kMeta:
      return "m";
    case SpecializationKind::kModule:
      return "d";
    case SpecializationKind::kToken:
      return "token";
  }
}

// LINT.IfChange
std::string Frame(std::string_view value) {
  return absl::StrCat(value.size(), "_", value);
}

// Each non-alphanumeric byte becomes _HH (uppercase hex), including '_'
// so a literal "_2E" remains distinct from ".".
std::string FrameIdentifier(std::string_view identifier) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string escaped;
  for (unsigned char c : identifier) {
    if (absl::ascii_isalnum(c)) {
      escaped.push_back(c);
    } else {
      escaped.push_back('_');
      escaped.push_back(kHex[c >> 4]);
      escaped.push_back(kHex[c & 0xf]);
    }
  }
  return Frame(escaped);
}
// LINT.ThenChange(//xls/build_rules/xls_ir_rules.bzl)

class CheckedTypeReferenceData final : public InterpValue::TypeReferenceData {
 public:
  explicit CheckedTypeReferenceData(std::unique_ptr<const Type> type)
      : type_(std::move(type)) {}

  const Type& type() const override { return *type_; }

  bool IsSameSpecialization(const TypeReferenceData& other) const override {
    return type_->IsSameSpecialization(other.type());
  }

  absl::StatusOr<std::string> GetSpecializationName() const override {
    return MangleType(*type_);
  }

 private:
  std::unique_ptr<const Type> type_;
};

// A single level of grammar text, split at its child types. Intern children
// first so pointer equality in this key means equality of their complete names.
struct TypeDescription {
  std::vector<std::pair<std::string, const TypeDescription*>> children;
  std::string suffix;
  bool contains_sum = false;

  bool operator==(const TypeDescription&) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const TypeDescription& d) {
    return H::combine(std::move(h), d.children, d.suffix);
  }
};

class TypeDescriptions {
 public:
  absl::StatusOr<const TypeDescription*> Intern(const Type& type);

 private:
  absl::Status AppendChild(TypeDescription& desc, const Type& child) {
    XLS_ASSIGN_OR_RETURN(const TypeDescription* canonical, Intern(child));
    desc.contains_sum |= canonical->contains_sum;
    desc.children.emplace_back(std::move(desc.suffix), canonical);
    desc.suffix.clear();
    return absl::OkStatus();
  }

  absl::Status AppendBinding(TypeDescription& desc, const InterpValue& value);

  // Keep stable addresses for descriptions used as children of other keys.
  absl::node_hash_set<TypeDescription> descriptions_;
  // Input identity only affects traversal cost. Independent copies with equal
  // descriptions also get the same canonical node.
  absl::flat_hash_map<const void*, const TypeDescription*> visited_;
};

absl::Status TypeDescriptions::AppendBinding(TypeDescription& desc,
                                             const InterpValue& value) {
  switch (type_system_internal::GetBindingKind(value)) {
    case BindingKind::kType: {
      XLS_ASSIGN_OR_RETURN(const auto* checked,
                           value.GetCheckedTypeReference());
      desc.suffix += "t";
      return AppendChild(desc, checked->type());
    }
    case BindingKind::kBits: {
      const auto& bits = value.GetBitsOrDie();
      absl::StrAppend(&desc.suffix, "v", bits.bit_count(), "_",
                      BitsToRawDigits(bits, FormatPreference::kPlainHex), "_");
      return absl::OkStatus();
    }
    case BindingKind::kTuple:
    case BindingKind::kArray: {
      XLS_ASSIGN_OR_RETURN(int64_t length, value.GetLength());
      absl::StrAppend(&desc.suffix, value.IsTuple() ? "q" : "a", length, "_");
      for (int64_t i = 0; i < length; ++i) {
        XLS_ASSIGN_OR_RETURN(InterpValue element, value.Index(i));
        XLS_RETURN_IF_ERROR(AppendBinding(desc, element));
      }
      return absl::OkStatus();
    }
    case BindingKind::kUnsupported:
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported nominal specialization binding: ",
                       TagToString(value.tag())));
  }
}

absl::StatusOr<const TypeDescription*> TypeDescriptions::Intern(
    const Type& type) {
  const auto* sum = dynamic_cast<const SumType*>(&type);
  const void* storage = &type;
  if (sum != nullptr) {
    storage = &sum->variants();
  }
  if (auto it = visited_.find(storage); it != visited_.end()) {
    return it->second;
  }
  TypeDescription desc;
  desc.contains_sum = sum != nullptr;
  XLS_ASSIGN_OR_RETURN(std::vector<SpecializationPart> parts,
                       type_system_internal::DescribeSpecialization(type));
  for (const SpecializationPart& part : parts) {
    if (const auto* kind = std::get_if<SpecializationKind>(&part)) {
      desc.suffix.append(KindTag(*kind));
    } else if (const auto* number = std::get_if<int64_t>(&part)) {
      if (*number < 0) {
        return absl::InvalidArgumentError("Negative specialization dimension");
      }
      absl::StrAppend(&desc.suffix, *number, "_");
    } else if (const auto* identifier = std::get_if<std::string_view>(&part)) {
      desc.suffix.append(FrameIdentifier(*identifier));
    } else if (const auto* nominal = std::get_if<NominalPart>(&part)) {
      desc.suffix.append(FrameIdentifier(nominal->module->name()));
      if (nominal->identifier.has_value()) {
        desc.suffix.append(FrameIdentifier(*nominal->identifier));
      }
    } else if (const auto* child = std::get_if<const Type*>(&part)) {
      XLS_RETURN_IF_ERROR(AppendChild(desc, **child));
    } else {
      XLS_RETURN_IF_ERROR(
          AppendBinding(desc, *std::get<const InterpValue*>(part)));
    }
  }
  const auto* canonical = &*descriptions_.insert(std::move(desc)).first;
  visited_.emplace(storage, canonical);
  return canonical;
}

void AppendType(const TypeDescription& desc,
                absl::flat_hash_map<const TypeDescription*, size_t>& ordinals,
                std::string& result) {
  if (desc.contains_sum) {
    auto [it, first] = ordinals.emplace(&desc, ordinals.size());
    if (!first) {
      absl::StrAppend(&result, "b", it->second, "_");
      return;
    }
  }
  for (const auto& [prefix, child] : desc.children) {
    result.append(prefix);
    AppendType(*child, ordinals, result);
  }
  result.append(desc.suffix);
}

// Converts the given InterpValue into a string form appropriate for name
// mangling: we need a means of disambiguating value- or struct-parameterized
// function names, and so we need to be able to represent instantiation values.
absl::StatusOr<std::string> MangleInterpValue(const InterpValue& value) {
  if (value.IsBits() || value.IsEnum()) {
    auto bits_value = InterpValue::MakeBits(/*is_signed=*/value.IsSigned(),
                                            value.GetBitsOrDie());
    std::string s = bits_value.ToString(/*humanize=*/true,
                                        FormatPreference::kUnsignedDecimal);
    // Negatives are not valid characters in IR symbols so replace leading '-'
    // with 'm'.
    return absl::StrReplaceAll(s, {{"-", "m"}});
  }

  if (value.IsTypeReference()) {
    return value.GetTypeReferenceSpecializationName();
  }

  CHECK(value.IsArray() || value.IsTuple())
      << "Only bits, enums, arrays, or tuples can be name-mangled.";
  std::vector<std::string> members;
  for (const auto& member : value.GetValuesOrDie()) {
    XLS_ASSIGN_OR_RETURN(std::string mangled_member, MangleInterpValue(member));
    members.push_back(std::move(mangled_member));
  }

  // Preserve the established spelling of aggregate value arguments.
  return absl::StrFormat("__%d%s__", members.size(),
                         absl::StrJoin(members, "_"));
}

}  // namespace

absl::StatusOr<std::string> MangleType(const Type& type) {
  TypeDescriptions descriptions;
  XLS_ASSIGN_OR_RETURN(const TypeDescription* root, descriptions.Intern(type));
  absl::flat_hash_map<const TypeDescription*, size_t> ordinals;
  std::string result;
  AppendType(*root, ordinals, result);
  return result;
}

InterpValue MakeCheckedTypeReference(const TypeAnnotation* annotation,
                                     std::unique_ptr<const Type> type) {
  CHECK(type != nullptr);
  return InterpValue::MakeTypeReference(
      annotation, std::make_shared<CheckedTypeReferenceData>(std::move(type)));
}

// LINT.IfChange
absl::StatusOr<std::string> MangleDslxName(
    std::string_view module_name, std::string_view function_name,
    CallingConvention convention, const absl::btree_set<std::string>& free_keys,
    const ParametricEnv& parametric_env, std::string_view scope) {
  absl::btree_set<std::string> parametric_env_keys;
  std::vector<std::string> parametric_env_values;
  bool has_type_parametric = absl::c_any_of(
      parametric_env.bindings(), [](const ParametricEnvItem& item) {
        return item.value.IsTypeReference();
      });
  for (const ParametricEnvItem& item : parametric_env.bindings()) {
    parametric_env_keys.insert(item.identifier);
    const InterpValue& value = item.value;
    XLS_ASSIGN_OR_RETURN(std::string mangled_value, MangleInterpValue(value));
    if (has_type_parametric) {
      mangled_value = absl::StrCat(value.IsTypeReference() ? "T" : "V",
                                   Frame(mangled_value));
    }
    parametric_env_values.push_back(std::move(mangled_value));
  }
  absl::btree_set<std::string> difference;
  absl::c_set_difference(free_keys, parametric_env_keys,
                         std::inserter(difference, difference.begin()));
  if (!difference.empty()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Not enough parametric bindings to convert function "
                        "'%s'; need {%s} got {%s}",
                        function_name, absl::StrJoin(free_keys, ", "),
                        absl::StrJoin(parametric_env_keys, ", ")));
  }

  std::string convention_str;
  if (convention == CallingConvention::kImplicitToken) {
    // Note that this is an implicit token threaded call target.
    convention_str = "itok__";
  }
  std::string suffix;
  if (!parametric_env_values.empty()) {
    suffix = absl::StrCat("__", absl::StrJoin(parametric_env_values, "_"));
  }

  if (has_type_parametric) {
    // _0T__ separates specializations with type arguments from the ordinary
    // __ namespace. Each component has a byte length, including an empty
    // scope. T and V introduce a mangled type and a mangled value respectively.
    if (convention == CallingConvention::kProcNext) {
      convention_str = "pnext__";
      suffix += "_next";
    }
    return absl::StrCat("_0T__", convention_str, FrameIdentifier(module_name),
                        "_", FrameIdentifier(scope), "_",
                        FrameIdentifier(function_name), suffix);
  }

  std::string module_name_str = absl::StrReplaceAll(module_name, {{".", "_"}});

  std::string scope_str = "";
  if (!scope.empty()) {
    scope_str = absl::StrCat(absl::StrReplaceAll(scope,
                                                 {
                                                     {">", "_"},
                                                     {"<", "_"},
                                                     {"/", "_"},
                                                     {":", "_"},
                                                     {"-", "_"},
                                                     {".", "_"},
                                                     {",", "__"},
                                                     {" ", ""},
                                                 }),
                             "__");
  }

  std::string mangled_name =
      absl::StrFormat("__%s%s__%s%s%s", convention_str, module_name_str,
                      scope_str, function_name, suffix);
  if (convention == CallingConvention::kProcNext) {
    // Note that the identifier for the next function of a proc is used to
    // identify the proc in the conversion. The config function of a proc is
    // absorbed in the conversion, as a result it is never referenced.
    // Thus, it does not need to be mangled.
    mangled_name = absl::StrCat(
        absl::StrReplaceAll(mangled_name, {{":", "_"}, {"->", "__"}}), "_next");
  }
  return mangled_name;
}
// LINT.ThenChange(//xls/build_rules/xls_ir_rules.bzl)

}  // namespace xls::dslx
