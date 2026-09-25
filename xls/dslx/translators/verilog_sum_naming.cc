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

#include "xls/dslx/translators/verilog_sum_naming.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "openssl/sha.h"
#include "xls/codegen/vast/vast.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system_v2/type_annotation_utils.h"
#include "xls/ir/bits.h"
#include "xls/ir/name_uniquer.h"

namespace xls::dslx::verilog_sum {
namespace {

// Produces a readable suffix for variant views and constructors; the caller
// handles collisions between source spellings that produce the same suffix.
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

// The bound is for the complete suffix after Verilog escaping. Each
// intermediate readable node is also bounded so nested aliases cannot build
// expanded text.
constexpr size_t kMaxSpecializationSuffixSize = 96;

// The printable legacy grammar uses lengths around source identifiers and
// scalar values, which may contain punctuation.
std::string IdentityAtom(std::string_view text) {
  return absl::StrCat(text.size(), ":", text);
}

void AppendReadable(std::optional<std::string>& output, std::string_view text) {
  if (output.has_value()) {
    if (text.size() <= kMaxSpecializationSuffixSize - output->size()) {
      output->append(text);
    } else {
      output.reset();
    }
  }
}

void AppendReadableChild(std::optional<std::string>& output,
                         const std::optional<std::string>& child) {
  if (child.has_value()) {
    AppendReadable(output, *child);
  } else {
    output.reset();
  }
}

std::optional<std::string> NominalReadable(std::string_view kind,
                                           std::string_view owner,
                                           std::string_view identifier) {
  std::optional<std::string> result = std::string(kind);
  if (owner.size() <= kMaxSpecializationSuffixSize &&
      identifier.size() <= kMaxSpecializationSuffixSize) {
    AppendReadable(result, IdentityAtom(owner));
    AppendReadable(result, IdentityAtom(identifier));
  } else {
    result.reset();
  }
  return result;
}

const SpecializationArgument* ReferencedArgument(
    const NameRef& reference,
    absl::Span<const SpecializationArgument> arguments,
    absl::Span<ParametricBinding* const> bindings) {
  const auto definition = reference.name_def();
  if (const auto* name = std::get_if<const NameDef*>(&definition)) {
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (bindings[i]->name_def() == *name) {
        return &arguments[i];
      }
    }
  }
  return nullptr;
}

struct DescriptorBits {
  const TypeDim* signedness;
  const TypeDim* size;
};

std::optional<DescriptorBits> BitsLike(const SpecializationType& type) {
  using Kind = SpecializationType::Kind;
  const SpecializationType::Description& description = type.description();
  if (description.kind == Kind::kBits) {
    return DescriptorBits{.signedness = &*description.signedness,
                          .size = &*description.size};
  } else if (description.kind == Kind::kArray &&
             description.children.front()->description().kind ==
                 Kind::kBitsConstructor) {
    return DescriptorBits{
        .signedness = &*description.children.front()->description().signedness,
        .size = &*description.size};
  } else {
    return std::nullopt;
  }
}

// Module types have no specialization descriptor. Stop at any unsupported
// compiler type before conversion so even one nested inside a structural root
// returns the existing naming diagnostic instead of reaching the descriptor
// constructor. Nominal arguments already have compact descriptions.
absl::Status ValidateRootType(const Type& type) {
  if (type.IsFunction() || type.IsMeta() || IsBitsConstructor(type) ||
      type.IsModule()) {
    return absl::UnimplementedError(absl::StrFormat(
        "Unsupported SystemVerilog sum specialization argument: %s",
        type.GetDebugTypeName()));
  } else if (type.IsArray() && !IsBitsLike(type)) {
    return ValidateRootType(type.AsArray().element_type());
  } else if (type.IsTuple()) {
    for (const std::unique_ptr<Type>& member : type.AsTuple().members()) {
      XLS_RETURN_IF_ERROR(ValidateRootType(*member));
    }
  } else if (type.IsChannel()) {
    return ValidateRootType(type.AsChannel().payload_type());
  }
  return absl::OkStatus();
}

// Numeric values compare by width and bits, regardless of their runtime tag.
// Decide readability using only the binding and earlier resolved arguments so
// raw bits and enum-tagged values always produce the same public name.
std::optional<bool> BindingSignedness(
    const ParametricBinding& binding,
    absl::Span<const SpecializationArgument> preceding_arguments,
    absl::Span<ParametricBinding* const> preceding_bindings) {
  const TypeAnnotation* annotation = binding.type_annotation();
  while (const auto* ref =
             dynamic_cast<const TypeRefTypeAnnotation*>(annotation)) {
    if (const auto* alias =
            std::get_if<TypeAlias*>(&ref->type_ref()->type_definition())) {
      annotation = &(*alias)->type_annotation();
    } else {
      return std::nullopt;
    }
  }

  if (const auto* variable =
          dynamic_cast<const TypeVariableTypeAnnotation*>(annotation)) {
    const auto* argument = ReferencedArgument(
        *variable->type_variable(), preceding_arguments, preceding_bindings);
    if (argument != nullptr) {
      if (const auto* type = std::get_if<SpecializationTypePtr>(argument)) {
        if (std::optional<DescriptorBits> bits = BitsLike(**type)) {
          if (absl::StatusOr<bool> sign = bits->signedness->GetAsBool();
              sign.ok()) {
            return *sign;
          }
        }
      }
    }
    return std::nullopt;
  }

  absl::StatusOr<SignednessAndBitCountResult> bits =
      GetSignednessAndBitCount(annotation);
  if (!bits.ok()) {
    return std::nullopt;
  } else if (const auto* sign = std::get_if<bool>(&bits->signedness)) {
    return *sign;
  } else {
    const Expr* sign_expr = std::get<const Expr*>(bits->signedness);
    if (const auto* number = dynamic_cast<const Number*>(sign_expr)) {
      if (absl::StatusOr<uint64_t> literal =
              number->GetAsUint64(*number->owner()->file_table());
          literal.ok() && *literal <= 1) {
        return *literal != 0;
      }
    } else if (const auto* reference =
                   dynamic_cast<const NameRef*>(sign_expr)) {
      const auto* argument = ReferencedArgument(*reference, preceding_arguments,
                                                preceding_bindings);
      if (argument != nullptr) {
        if (const auto* value = std::get_if<InterpValue>(argument);
            value != nullptr && (value->IsBits() || value->IsEnum()) &&
            value->GetBitsOrDie().bit_count() == 1) {
          return !value->GetBitsOrDie().IsZero();
        }
      }
    }
    return std::nullopt;
  }
}

}  // namespace

class IdentityBuilder::Impl {
 public:
  explicit Impl(NominalOwner owner = {}) : owner_(std::move(owner)) {}

  struct Node {
    std::string digest;
    std::optional<std::string> readable;
    // Populated for nominal nodes; other kinds have no suffix.
    std::string public_suffix;
  };

  absl::StatusOr<Node> TypeNode(const Type& type);
  absl::StatusOr<std::string> SumName(const SumType& sum);
  absl::StatusOr<std::string> StructName(const StructType& type);
  size_t sum_identity_computations() const {
    return sum_identity_computations_;
  }
  size_t struct_identity_computations() const {
    return struct_identity_computations_;
  }
  size_t value_identity_computations() const {
    return value_identity_computations_;
  }

 private:
  using Kind = SpecializationType::Kind;

  std::string_view NominalOwnerName(const AstNode& definition) const {
    return owner_ ? owner_(definition)
                  : std::string_view(definition.owner()->name());
  }

  // Every field is length-prefixed; version, node kind and ordered child
  // fingerprints are therefore unambiguous even for arbitrary source bytes.
  // Streaming keeps an aggregate from retaining all of its child fingerprints.
  class Frame {
   public:
    explicit Frame(std::string_view kind) {
      SHA256_Init(&state_);
      Add("xls-dslx-sv-sum-identity-v1");
      Add(kind);
    }
    void Add(std::string_view field) {
      std::string prefix = absl::StrCat(field.size(), ":");
      SHA256_Update(&state_, prefix.data(), prefix.size());
      if (!field.empty()) {
        SHA256_Update(&state_, field.data(), field.size());
      }
    }
    std::string Digest() && {
      std::array<char, SHA256_DIGEST_LENGTH> hash;
      SHA256_Final(reinterpret_cast<uint8_t*>(hash.data()), &state_);
      return absl::BytesToHexString({hash.data(), hash.size()});
    }

   private:
    SHA256_CTX state_;
  };

  Node Finish(Frame frame, std::optional<std::string> readable);
  absl::StatusOr<Node> TypeNode(const SpecializationTypePtr& type);
  absl::StatusOr<Node> UncachedTypeNode(const SpecializationType& type);
  absl::StatusOr<Node> ValueNode(const InterpValue& value);
  absl::StatusOr<std::vector<Node>> Arguments(
      absl::Span<const SpecializationArgument> arguments,
      absl::Span<ParametricBinding* const> bindings, bool is_struct);
  absl::StatusOr<Node> ArgumentNode(const SpecializationArgument& argument,
                                    const ParametricBinding& binding,
                                    std::optional<bool> is_signed,
                                    bool is_struct);
  absl::StatusOr<Node> NominalTypeNode(
      Kind kind, const AstNode& definition,
      absl::Span<const SpecializationArgument> specialization_arguments,
      bool arguments_known);
  Node NominalNode(std::string_view kind, const AstNode& definition,
                   std::string_view identifier,
                   const std::vector<Node>& arguments);
  std::string PublicSuffix(const Node& type,
                           const std::vector<Node>& arguments);

  struct MemoizedType {
    SpecializationTypePtr owner;
    Node node;
  };
  struct MemoizedRoot {
    std::shared_ptr<const std::vector<SpecializationArgument>> owner;
    Node node;
  };
  // Own every pointer used as a key so transient compiler Type wrappers cannot
  // cause the cache to mistake a reused address for an existing description.
  std::map<const SpecializationType*, MemoizedType> types_;
  // Separate descriptors can represent clones of the same nominal DAG node.
  // Their precomputed semantic hashes avoid rebuilding that node's identity.
  std::map<size_t, std::vector<MemoizedType>> nominal_types_;
  std::map<const AstNode*,
           std::map<const std::vector<SpecializationArgument>*, MemoizedRoot>>
      nominal_roots_;
  // A symbolic array is completely determined by its length and first value;
  // the empty array has no first value. Sharing the array digest avoids walking
  // the same constant for different nominal specializations in this package.
  std::map<std::pair<int64_t, std::string>, Node> symbolic_range_nodes_;
  NominalOwner owner_;
  size_t sum_identity_computations_ = 0;
  size_t struct_identity_computations_ = 0;
  size_t value_identity_computations_ = 0;
};

IdentityBuilder::Impl::Node IdentityBuilder::Impl::Finish(
    Frame frame, std::optional<std::string> readable) {
  return Node{.digest = std::move(frame).Digest(),
              .readable = std::move(readable),
              .public_suffix = ""};
}

absl::StatusOr<IdentityBuilder::Impl::Node> IdentityBuilder::Impl::ValueNode(
    const InterpValue& value) {
  if (const auto range = value.GetRangeData(); range.has_value()) {
    XLS_ASSIGN_OR_RETURN(int64_t length, value.GetLength());
    std::pair<int64_t, std::string> key{length, ""};
    if (length != 0) {
      XLS_ASSIGN_OR_RETURN(Node first, ValueNode((*range)->start));
      key.second = std::move(first.digest);
    }
    if (auto cached = symbolic_range_nodes_.find(key);
        cached != symbolic_range_nodes_.end()) {
      return cached->second;
    }
    ++value_identity_computations_;
    Frame frame("value-array");
    frame.Add(absl::StrCat(length));
    for (int64_t i = 0; i < length; ++i) {
      XLS_ASSIGN_OR_RETURN(InterpValue element, value.Index(i));
      XLS_ASSIGN_OR_RETURN(Node child, ValueNode(element));
      frame.Add(child.digest);
    }
    Node node = Finish(std::move(frame), std::nullopt);
    symbolic_range_nodes_.emplace(std::move(key), node);
    return node;
  } else {
    ++value_identity_computations_;
    if (value.IsBits() || value.IsEnum()) {
      const Bits& bits = value.GetBitsOrDie();
      Frame frame("value-bits");
      frame.Add(absl::StrCat(bits.bit_count()));
      std::vector<uint8_t> bytes = bits.ToBytes();
      if (bytes.empty()) {
        frame.Add("");
      } else {
        frame.Add(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                   bytes.size()));
      }
      return Finish(std::move(frame), std::nullopt);
    } else if (value.IsTuple() || value.IsArray()) {
      Frame frame(value.IsTuple() ? "value-tuple" : "value-array");
      XLS_ASSIGN_OR_RETURN(int64_t length, value.GetLength());
      frame.Add(absl::StrCat(length));
      for (const InterpValue& element : value.GetValuesOrDie()) {
        XLS_ASSIGN_OR_RETURN(Node child, ValueNode(element));
        frame.Add(child.digest);
      }
      return Finish(std::move(frame), std::nullopt);
    } else if (value.tag() == InterpValueTag::kToken) {
      return absl::InvalidArgumentError(
          "Cannot export a SystemVerilog sum specialization whose value "
          "argument contains a token: tokens do not have a stable generated "
          "name");
    } else {
      return absl::UnimplementedError(absl::StrFormat(
          "Unsupported SystemVerilog sum specialization value: %s",
          TagToString(value.tag())));
    }
  }
}

absl::StatusOr<IdentityBuilder::Impl::Node> IdentityBuilder::Impl::ArgumentNode(
    const SpecializationArgument& argument, const ParametricBinding& binding,
    std::optional<bool> is_signed, bool is_struct) {
  if (const auto* value = std::get_if<InterpValue>(&argument);
      value != nullptr) {
    XLS_ASSIGN_OR_RETURN(Node child, ValueNode(*value));
    Frame frame("argument-value");
    frame.Add(child.digest);
    std::optional<std::string> readable;
    if (is_signed.has_value() && (value->IsBits() || value->IsEnum()) &&
        value->GetBitsOrDie().bit_count() <= 128) {
      if (is_struct) {
        // Struct dimension names have historically displayed raw decimal bits.
        std::string text =
            InterpValue::MakeUnsigned(value->GetBitsOrDie()).ToHumanString();
        const std::string& identifier = binding.name_def()->identifier();
        if (identifier.size() <= kMaxSpecializationSuffixSize) {
          readable = "binding:";
          AppendReadable(readable, IdentityAtom(identifier));
          AppendReadable(readable, IdentityAtom(text));
        }
      } else {
        std::string text =
            InterpValue::MakeBits(*is_signed, value->GetBitsOrDie()).ToString();
        readable = "value:";
        AppendReadable(readable, IdentityAtom(text));
      }
    }
    return Finish(std::move(frame), std::move(readable));
  } else {
    const auto& type = std::get<SpecializationTypePtr>(argument);
    XLS_ASSIGN_OR_RETURN(Node child, TypeNode(type));
    Frame frame("argument-type");
    frame.Add(child.digest);
    std::optional<std::string> readable = "type:";
    AppendReadableChild(readable, child.readable);
    return Finish(std::move(frame), std::move(readable));
  }
}

absl::StatusOr<std::vector<IdentityBuilder::Impl::Node>>
IdentityBuilder::Impl::Arguments(
    absl::Span<const SpecializationArgument> arguments,
    absl::Span<ParametricBinding* const> bindings, bool is_struct) {
  if (arguments.size() != bindings.size()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "SystemVerilog sum identity requires all %d resolved parametric "
        "arguments; got %d",
        bindings.size(), arguments.size()));
  }
  std::vector<Node> result;
  result.reserve(arguments.size());
  for (size_t i = 0; i < arguments.size(); ++i) {
    std::optional<bool> is_signed;
    if (std::holds_alternative<InterpValue>(arguments[i])) {
      is_signed = BindingSignedness(*bindings[i], arguments.first(i),
                                    bindings.first(i));
    }
    XLS_ASSIGN_OR_RETURN(Node node, ArgumentNode(arguments[i], *bindings[i],
                                                 is_signed, is_struct));
    result.push_back(std::move(node));
  }
  return result;
}

IdentityBuilder::Impl::Node IdentityBuilder::Impl::NominalNode(
    std::string_view kind, const AstNode& definition,
    std::string_view identifier, const std::vector<Node>& arguments) {
  Frame frame(kind);
  std::string_view owner = NominalOwnerName(definition);
  frame.Add(owner);
  frame.Add(identifier);
  frame.Add(absl::StrCat(arguments.size()));
  std::optional<std::string> readable =
      NominalReadable(absl::StrCat(kind, ":"), owner, identifier);
  AppendReadable(readable, "[");
  for (const Node& argument : arguments) {
    frame.Add(argument.digest);
    AppendReadableChild(readable, argument.readable);
    AppendReadable(readable, ";");
  }
  AppendReadable(readable, "]");
  Node result = Finish(std::move(frame), std::move(readable));
  result.public_suffix = PublicSuffix(result, arguments);
  return result;
}

absl::StatusOr<IdentityBuilder::Impl::Node> IdentityBuilder::Impl::TypeNode(
    const Type& type) {
  XLS_RETURN_IF_ERROR(ValidateRootType(type));
  const AstNode* nominal = nullptr;
  std::shared_ptr<const std::vector<SpecializationArgument>> arguments;
  if (type.IsSum()) {
    nominal = &type.AsSum().nominal_type();
    arguments = type.AsSum().shared_specialization_arguments();
  } else if (type.IsStruct()) {
    nominal = &type.AsStruct().nominal_type();
    arguments = type.AsStruct().shared_specialization_arguments();
  } else if (type.IsProc()) {
    nominal = &type.AsProc().nominal_type();
    arguments = type.AsProc().shared_specialization_arguments();
  }

  if (arguments != nullptr) {
    auto& roots = nominal_roots_[nominal];
    if (auto cached = roots.find(arguments.get()); cached != roots.end()) {
      return cached->second.node;
    }
  }
  XLS_ASSIGN_OR_RETURN(Node node, TypeNode(SpecializationType::FromType(type)));
  if (arguments != nullptr) {
    const auto* key = arguments.get();
    nominal_roots_[nominal].emplace(
        key, MemoizedRoot{.owner = std::move(arguments), .node = node});
  }
  return node;
}

absl::StatusOr<IdentityBuilder::Impl::Node> IdentityBuilder::Impl::TypeNode(
    const SpecializationTypePtr& type) {
  if (auto cached = types_.find(type.get()); cached != types_.end()) {
    return cached->second.node;
  }
  const Kind kind = type->description().kind;
  const bool is_nominal =
      kind == Kind::kStruct || kind == Kind::kProc || kind == Kind::kSum;
  if (is_nominal) {
    for (const MemoizedType& cached : nominal_types_[type->hash()]) {
      if (type->SemanticEquals(*cached.owner)) {
        Node node = cached.node;
        types_.emplace(type.get(), MemoizedType{.owner = type, .node = node});
        return node;
      }
    }
  }

  XLS_ASSIGN_OR_RETURN(Node node, UncachedTypeNode(*type));
  types_.emplace(type.get(), MemoizedType{.owner = type, .node = node});
  if (is_nominal) {
    nominal_types_[type->hash()].push_back(
        MemoizedType{.owner = type, .node = node});
  }
  return node;
}

absl::StatusOr<IdentityBuilder::Impl::Node>
IdentityBuilder::Impl::NominalTypeNode(
    Kind kind, const AstNode& definition,
    absl::Span<const SpecializationArgument> specialization_arguments,
    bool arguments_known) {
  std::string_view kind_name;
  std::string_view identifier;
  absl::Span<ParametricBinding* const> bindings;
  if (kind == Kind::kStruct) {
    ++struct_identity_computations_;
    const auto& record = static_cast<const StructDef&>(definition);
    kind_name = "struct";
    identifier = record.identifier();
    bindings = record.parametric_bindings();
  } else if (kind == Kind::kProc) {
    const auto& proc = static_cast<const ProcDef&>(definition);
    kind_name = "proc";
    identifier = proc.identifier();
    bindings = proc.parametric_bindings();
  } else {
    ++sum_identity_computations_;
    const auto& sum = static_cast<const SumDef&>(definition);
    kind_name = "sum";
    identifier = sum.identifier();
    bindings = sum.parametric_bindings();
  }
  if (kind != Kind::kSum && !arguments_known && !bindings.empty()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "SystemVerilog sum identity requires resolved parametric arguments "
        "for %s %s",
        kind_name, identifier));
  }
  XLS_ASSIGN_OR_RETURN(
      std::vector<Node> arguments,
      Arguments(specialization_arguments, bindings, kind == Kind::kStruct));
  return NominalNode(kind_name, definition, identifier, arguments);
}

absl::StatusOr<IdentityBuilder::Impl::Node>
IdentityBuilder::Impl::UncachedTypeNode(const SpecializationType& type) {
  const SpecializationType::Description& description = type.description();
  if (std::optional<DescriptorBits> bits = BitsLike(type); bits.has_value()) {
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits->signedness->GetAsBool());
    XLS_ASSIGN_OR_RETURN(int64_t width, bits->size->GetAsInt64());
    Frame frame("bits");
    frame.Add(is_signed ? "signed" : "unsigned");
    frame.Add(absl::StrCat(width));
    return Finish(std::move(frame), absl::StrCat(is_signed ? "s" : "u", width));
  }
  switch (description.kind) {
    case Kind::kEnum: {
      const auto& definition =
          static_cast<const EnumDef&>(*description.nominal);
      Frame frame("enum");
      std::string_view owner = NominalOwnerName(definition);
      frame.Add(owner);
      frame.Add(definition.identifier());
      return Finish(std::move(frame),
                    NominalReadable("enum:", owner, definition.identifier()));
    }
    case Kind::kStruct:
    case Kind::kProc:
    case Kind::kSum:
      return NominalTypeNode(description.kind, *description.nominal,
                             description.arguments,
                             description.nominal_arguments_known);
    case Kind::kArray: {
      XLS_ASSIGN_OR_RETURN(int64_t size, description.size->GetAsInt64());
      XLS_ASSIGN_OR_RETURN(Node element,
                           TypeNode(description.children.front()));
      Frame frame("array");
      frame.Add(absl::StrCat(size));
      frame.Add(element.digest);
      std::optional<std::string> readable = absl::StrCat("array:", size, "[");
      AppendReadableChild(readable, element.readable);
      AppendReadable(readable, "]");
      return Finish(std::move(frame), std::move(readable));
    }
    case Kind::kTuple: {
      Frame frame("tuple");
      frame.Add(absl::StrCat(description.children.size()));
      std::optional<std::string> readable = "tuple[";
      for (const SpecializationTypePtr& member : description.children) {
        XLS_ASSIGN_OR_RETURN(Node child, TypeNode(member));
        frame.Add(child.digest);
        AppendReadableChild(readable, child.readable);
        AppendReadable(readable, ";");
      }
      AppendReadable(readable, "]");
      return Finish(std::move(frame), std::move(readable));
    }
    case Kind::kChannel: {
      XLS_ASSIGN_OR_RETURN(Node payload,
                           TypeNode(description.children.front()));
      Frame frame("channel");
      frame.Add(ChannelDirectionToString(*description.direction));
      frame.Add(payload.digest);
      // This is only an argument identity: a channel itself is not an
      // exportable physical sum payload. No legacy readable spelling exists.
      return Finish(std::move(frame), std::nullopt);
    }
    case Kind::kToken:
      return Finish(Frame("token"), "token");
    case Kind::kFunction:
      return absl::UnimplementedError(
          "Unsupported SystemVerilog sum specialization argument: function");
    case Kind::kMeta:
      return absl::UnimplementedError(
          "Unsupported SystemVerilog sum specialization argument: meta-type");
    case Kind::kBitsConstructor:
      return absl::UnimplementedError(
          "Unsupported SystemVerilog sum specialization argument: "
          "bits-constructor");
    case Kind::kBits:
      return absl::InternalError("Bits descriptor has no bits-like properties");
  }
  return absl::InternalError("Unknown SystemVerilog specialization type kind");
}

std::string IdentityBuilder::Impl::PublicSuffix(
    const Node& type, const std::vector<Node>& arguments) {
  std::optional<std::string> readable = "";
  for (const Node& argument : arguments) {
    if (argument.readable.has_value()) {
      AppendReadable(readable, "__");
      AppendReadable(readable, EscapeName(*argument.readable));
    } else {
      readable.reset();
    }
  }
  return readable.value_or(absl::StrCat("__h", type.digest));
}

absl::StatusOr<std::string> IdentityBuilder::Impl::SumName(const SumType& sum) {
  XLS_ASSIGN_OR_RETURN(Node type, TypeNode(sum));
  return std::move(type.public_suffix);
}

absl::StatusOr<std::string> IdentityBuilder::Impl::StructName(
    const StructType& type) {
  XLS_ASSIGN_OR_RETURN(Node node, TypeNode(type));
  return std::move(node.public_suffix);
}

IdentityBuilder::IdentityBuilder() : impl_(std::make_unique<Impl>()) {}
IdentityBuilder::~IdentityBuilder() = default;
IdentityBuilder::IdentityBuilder(IdentityBuilder&&) noexcept = default;
IdentityBuilder& IdentityBuilder::operator=(IdentityBuilder&&) noexcept =
    default;

absl::StatusOr<std::string> IdentityBuilder::TypeIdentity(const Type& type) {
  XLS_ASSIGN_OR_RETURN(Impl::Node node, impl_->TypeNode(type));
  return node.digest;
}

void IdentityBuilder::SetPublicNominalOwner(NominalOwner owner) {
  public_impl_ = std::make_unique<Impl>(std::move(owner));
}

absl::StatusOr<std::string> IdentityBuilder::SpecializationName(
    const SumType& sum) {
  return (public_impl_ == nullptr ? impl_ : public_impl_)->SumName(sum);
}

absl::StatusOr<std::string> IdentityBuilder::StructSpecializationName(
    const StructType& type) {
  return (public_impl_ == nullptr ? impl_ : public_impl_)->StructName(type);
}

size_t IdentityBuilder::sum_identity_computations_for_testing() const {
  return impl_->sum_identity_computations() +
         (public_impl_ == nullptr ? 0
                                  : public_impl_->sum_identity_computations());
}

size_t IdentityBuilder::struct_identity_computations_for_testing() const {
  return impl_->struct_identity_computations() +
         (public_impl_ == nullptr
              ? 0
              : public_impl_->struct_identity_computations());
}

size_t IdentityBuilder::value_identity_computations_for_testing() const {
  return impl_->value_identity_computations() +
         (public_impl_ == nullptr
              ? 0
              : public_impl_->value_identity_computations());
}

std::string MemberName(NameUniquer& names, std::string_view name) {
  return names.GetSanitizedUniqueName(verilog::SanitizeVerilogIdentifier(name));
}

std::string SourceName(const AstNode& node, std::string_view identifier) {
  return absl::StrCat(node.owner()->name(), ":", identifier);
}

std::string EscapeName(std::string_view part) {
  std::string result;
  for (unsigned char c : part) {
    if (absl::ascii_isalnum(c)) {
      result.push_back(c);
    } else {
      absl::StrAppendFormat(&result, "_%02x_", c);
    }
  }
  return result;
}

std::string EnumCompanionKey(const EnumDef& definition) {
  return absl::StrCat("companion:enum:",
                      SourceName(definition, definition.identifier()));
}

std::string StructCompanionKey(std::string_view identity) {
  return absl::StrCat("companion:struct:", identity);
}

std::string SignedArrayCompanionKey(int64_t width) {
  return absl::StrCat("companion:bits:", width);
}

absl::StatusOr<std::string> SpecializationName(const SumType& sum) {
  return IdentityBuilder().SpecializationName(sum);
}

absl::StatusOr<std::string> StructSpecializationName(const StructType& type) {
  return IdentityBuilder().StructSpecializationName(type);
}

absl::StatusOr<std::string> TypeIdentity(const Type& type) {
  return IdentityBuilder().TypeIdentity(type);
}

std::map<std::string, std::string> VariantSuffixes(const SumDef& sum) {
  std::map<std::string, std::string> result;
  for (const SumVariant* variant : sum.variants()) {
    result.emplace(variant->identifier(), SnakeCase(variant->identifier()));
  }
  NameUniquer names("__");
  for (auto& [source, suffix] : result) {
    suffix = names.GetSanitizedUniqueName(suffix);
  }
  return result;
}

std::map<std::string, std::string> FamilyNameRequests(const SumDef& sum,
                                                      std::string_view family,
                                                      bool has_payload) {
  std::map<std::string, std::string> names{
      {"getter", absl::StrCat(family, "_get_tag")},
      {"tag", absl::StrCat(family, "_tag_t")}};
  if (has_payload) {
    names.emplace("payload", absl::StrCat(family, "_payload_t"));
  }
  for (const auto& [source, suffix] : VariantSuffixes(sum)) {
    names.emplace(absl::StrCat("constructor:", source),
                  absl::StrCat(family, "_make_", suffix));
    names.emplace(absl::StrCat("tag:", source),
                  absl::StrCat(family, "_tag_", source));
    if (has_payload) {
      names.emplace(absl::StrCat("view:", source),
                    absl::StrCat(family, "_", suffix, "_view_t"));
    }
  }
  return names;
}

}  // namespace xls::dslx::verilog_sum
