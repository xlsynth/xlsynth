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

#include "xls/public/c_api_dslx.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/node_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/attribute_data.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_cloner.h"
#include "xls/dslx/frontend/ast_node.h"
#include "xls/dslx/frontend/function_specializer.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_from_string.h"
#include "xls/dslx/make_value_format_descriptor.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/replace_invocations.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/unwrap_meta_type.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/warning_kind.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/value.h"
#include "xls/public/c_api_dslx_internal.h"
#include "xls/public/c_api_impl_helpers.h"

namespace {

// Tokens outlive their owning ImportData without retaining its AST. Equality
// is decided with DSLX Types while that owner is alive, including unused sum
// parameters; printed type names and format descriptors cannot establish it.
struct NominalTypeIdentity {};
using NominalTypeIdentityPtr = std::shared_ptr<const NominalTypeIdentity>;

struct BitsTypeIdentity {
  bool is_signed;
  int64_t bit_count;
  bool operator==(const BitsTypeIdentity&) const = default;
};

struct ValueTypeIdentity;
struct ArrayTypeIdentity {
  int64_t size;
  std::shared_ptr<const ValueTypeIdentity> element;
  bool operator==(const ArrayTypeIdentity& other) const;
};

struct ValueTypeIdentity {
  std::variant<BitsTypeIdentity, ArrayTypeIdentity,
               std::vector<ValueTypeIdentity>, NominalTypeIdentityPtr,
               std::monostate>
      shape;
  bool operator==(const ValueTypeIdentity&) const = default;
};

bool ArrayTypeIdentity::operator==(const ArrayTypeIdentity& other) const {
  return size == other.size && *element == *other.element;
}

struct ValueMetadata {
  // Raw C constructors can make values outside the DSLX type system, such as
  // heterogeneous arrays or undeclared enum patterns. Do not treat those as
  // wildcards when composing a later sum-bearing, semantically formatted value.
  std::optional<ValueTypeIdentity> type;
  xls::dslx::ValueFormatDescriptor descriptor;
  bool contains_sum;
};
using ValueMetadataPtr = std::shared_ptr<const ValueMetadata>;
using BindingMetadata = std::vector<ValueMetadataPtr>;

class ImportDataHandle {
 public:
  struct NominalMetadata {
    NominalTypeIdentityPtr identity =
        std::make_shared<const NominalTypeIdentity>();
    // Identity-only interning leaves the descriptor absent until requested.
    ValueMetadataPtr value_metadata;
  };

  explicit ImportDataHandle(xls::dslx::ImportData data)
      : data(std::move(data)) {}

  // Both lookup and insertion require mutex. Equality includes the nominal
  // declaration and concrete dimensions, not just the printed type name.
  const NominalMetadata* FindEnumType(const xls::dslx::EnumType& type) {
    if (enum_nominal_types_ == nullptr) {
      return nullptr;
    } else {
      auto existing = enum_nominal_types_->find(type);
      return existing == enum_nominal_types_->end() ? nullptr
                                                    : &existing->second;
    }
  }

  NominalMetadata& InternNominalType(const xls::dslx::EnumType& type) {
    if (enum_nominal_types_ == nullptr) {
      enum_nominal_types_ = std::make_unique<EnumTypeIndex>();
    }
    auto existing = enum_nominal_types_->find(type);
    if (existing != enum_nominal_types_->end()) {
      return existing->second;
    } else {
      return enum_nominal_types_
          ->try_emplace(EnumTypeKey{&type.nominal_type(), type.size().Clone(),
                                    type.is_signed()})
          .first->second;
    }
  }

  NominalMetadata& InternNominalType(const xls::dslx::SumType& type) {
    return sum_nominal_types_.try_emplace(type).first->second;
  }

  NominalTypeIdentityPtr InternStructType(const xls::dslx::StructType& type) {
    if (struct_nominal_types_ == nullptr) {
      struct_nominal_types_ = std::make_unique<StructTypeIndex>();
    }
    auto existing = struct_nominal_types_->find(type);
    if (existing != struct_nominal_types_->end()) {
      return existing->identity;
    } else {
      return struct_nominal_types_
          ->insert(
              StructTypeEntry{type.CloneToUnique(),
                              std::make_shared<const NominalTypeIdentity>()})
          .first->identity;
    }
  }

  // The first sum-bearing root keeps its completed descriptor graph without
  // publishing descendant formatting metadata. A second uncached metadata
  // build promotes that graph for owner reuse, even for the same aggregate
  // type; a cache hit on a direct sum does not count as a build.
  void PromoteSumDescriptorGraph();
  void RetainFirstSumDescriptorGraph(const xls::dslx::Type& type,
                                     ValueMetadataPtr metadata);
  bool has_pending_sum_descriptor_graph() const {
    return first_sum_descriptor_graph_.has_value();
  }
  bool shares_sum_descriptors() const { return shares_sum_descriptors_; }

  xls::dslx::ImportData data;
  // Serialize TypeInfo and nominal-metadata access with C parse/clone/import
  // operations on this owner. Ownership queries use ImportData's shorter lock.
  absl::Mutex mutex;
  // Tests install this before concurrent operations; it must not block.
  std::function<void()> metadata_lock_observer_for_testing;

 private:
  // These are exactly EnumType's equality fields. Member values feed the
  // descriptor builder, but are not part of the nominal identity key.
  struct EnumTypeKey {
    const xls::dslx::EnumDef* definition;
    xls::dslx::TypeDim size;
    bool is_signed;
  };

  struct EnumTypeHash {
    using is_transparent = void;
    static size_t Hash(const xls::dslx::EnumDef* definition,
                       const xls::dslx::TypeDim& size, bool is_signed) {
      // TypeDim equality compares InterpValue bit patterns, including their
      // width, but not their signed/unsigned value tag. Non-bits dimensions
      // conservatively collide and are still compared with full equality.
      const auto& value = size.value();
      return absl::HashOf(
          definition, is_signed,
          value.HasBits() ? absl::HashOf(value.GetBitsOrDie()) : 0);
    }
    size_t operator()(const EnumTypeKey& key) const {
      return Hash(key.definition, key.size, key.is_signed);
    }
    size_t operator()(const xls::dslx::EnumType& type) const {
      return Hash(&type.nominal_type(), type.size(), type.is_signed());
    }
  };

  struct EnumTypeEqual {
    using is_transparent = void;
    bool operator()(const EnumTypeKey& stored,
                    const xls::dslx::EnumType& lookup) const {
      return stored.definition == &lookup.nominal_type() &&
             stored.size == lookup.size() &&
             stored.is_signed == lookup.is_signed();
    }
    bool operator()(const EnumTypeKey& stored,
                    const EnumTypeKey& lookup) const {
      return stored.definition == lookup.definition &&
             stored.size == lookup.size && stored.is_signed == lookup.is_signed;
    }
  };
  using EnumTypeIndex = absl::flat_hash_map<EnumTypeKey, NominalMetadata,
                                            EnumTypeHash, EnumTypeEqual>;

  struct SumTypeHash {
    size_t operator()(const xls::dslx::SumType& type) const {
      return absl::HashOf(&type.nominal_type(), type.parametric_arguments_hash());
    }
  };

  struct SumTypeEqual {
    bool operator()(const xls::dslx::SumType& stored,
                    const xls::dslx::SumType& lookup) const {
      // Abseil passes the stored key first. Use the borrowed lookup's virtual
      // equality so derived observation types remain visible with value keys.
      return &stored.nominal_type() == &lookup.nominal_type() &&
             stored.parametric_arguments_hash() ==
                 lookup.parametric_arguments_hash() &&
             lookup == stored;
    }
  };

  struct StructTypeEntry {
    std::unique_ptr<const xls::dslx::Type> type;
    NominalTypeIdentityPtr identity;
  };

  struct StructTypeHash {
    using is_transparent = void;
    size_t operator()(const xls::dslx::Type&) const { return 0; }
    size_t operator()(const StructTypeEntry& entry) const {
      return (*this)(*entry.type);
    }
  };

  struct StructTypeEqual {
    using is_transparent = void;
    bool operator()(const StructTypeEntry& stored,
                    const xls::dslx::Type& lookup) const {
      return lookup == *stored.type;
    }
    bool operator()(const StructTypeEntry& stored,
                    const StructTypeEntry& lookup) const {
      return (*this)(stored, *lookup.type);
    }
  };
  using StructTypeIndex =
      absl::flat_hash_set<StructTypeEntry, StructTypeHash, StructTypeEqual>;

  // Declared after data so the AST-dependent keys are destroyed first. Borrowed
  // lookup avoids cloning dimensions on hits; keys never clone enum members.
  std::unique_ptr<EnumTypeIndex> enum_nominal_types_;
  // Each node stores a shallow SumType copy with its metadata, avoiding a
  // separate type-wrapper allocation. Full equality resolves hash collisions.
  absl::node_hash_map<xls::dslx::SumType, NominalMetadata, SumTypeHash,
                      SumTypeEqual>
      sum_nominal_types_;
  // All structs share the full-equality index. Entries clone a type once and
  // retain only its identity; formatting descriptors belong to returned values.
  std::unique_ptr<StructTypeIndex> struct_nominal_types_;
  struct SumDescriptorGraph {
    std::unique_ptr<xls::dslx::Type> type;
    ValueMetadataPtr metadata;
  };
  // The concrete Type clone keeps every borrowed payload Type reachable even
  // if the first C value is freed before another getter promotes the graph.
  // Both fields are destroyed before data and its AST at owner teardown.
  std::optional<SumDescriptorGraph> first_sum_descriptor_graph_;
  bool shares_sum_descriptors_ = false;
};

void HarvestSumDescriptorGraph(
    const xls::dslx::Type& type,
    const xls::dslx::ValueFormatDescriptor& descriptor, ImportDataHandle& owner,
    absl::flat_hash_set<const void*>& visited) {
  if (const auto* sum = dynamic_cast<const xls::dslx::SumType*>(&type)) {
    CHECK(descriptor.IsSum());
    CHECK_EQ(descriptor.sum_variant_count(), sum->variant_count());
    if (visited.insert(descriptor.sum_format_identity()).second) {
      auto& entry = owner.InternNominalType(*sum);
      if (entry.value_metadata == nullptr) {
        // A descriptor copy shares its immutable SumFormat. The existing
        // owner index still checks full Type equality after nominal/hash hits.
        entry.value_metadata = std::make_shared<const ValueMetadata>(
            ValueMetadata{ValueTypeIdentity{entry.identity}, descriptor, true});
      }
      for (int64_t i = 0; i < sum->variant_count(); ++i) {
        const auto& variant = sum->variants()[i];
        auto payload_formats = descriptor.sum_variant(i).payload_formats();
        CHECK_EQ(payload_formats.size(), variant.size());
        for (int64_t j = 0; j < variant.size(); ++j) {
          HarvestSumDescriptorGraph(variant.GetMemberType(j),
                                    payload_formats[j], owner, visited);
        }
      }
    }
  } else if (const auto* tuple =
                 dynamic_cast<const xls::dslx::TupleType*>(&type)) {
    CHECK(descriptor.IsTuple());
    auto elements = descriptor.tuple_elements();
    CHECK_EQ(elements.size(), tuple->size());
    for (int64_t i = 0; i < tuple->size(); ++i) {
      HarvestSumDescriptorGraph(tuple->GetMemberType(i), elements[i], owner,
                                visited);
    }
  } else if (const auto* structure =
                 dynamic_cast<const xls::dslx::StructTypeBase*>(&type)) {
    CHECK(descriptor.IsStruct());
    auto elements = descriptor.struct_elements();
    CHECK_EQ(elements.size(), structure->size());
    for (int64_t i = 0; i < structure->size(); ++i) {
      HarvestSumDescriptorGraph(structure->GetMemberType(i), elements[i], owner,
                                visited);
    }
  } else if (const auto* array =
                 dynamic_cast<const xls::dslx::ArrayType*>(&type)) {
    if (!xls::dslx::IsBitsLike(type)) {
      CHECK(descriptor.IsArray());
      HarvestSumDescriptorGraph(array->element_type(),
                                descriptor.array_element_format(), owner,
                                visited);
    }
  }
}

void ImportDataHandle::PromoteSumDescriptorGraph() {
  if (!shares_sum_descriptors_ && first_sum_descriptor_graph_.has_value()) {
    absl::flat_hash_set<const void*> visited;
    HarvestSumDescriptorGraph(*first_sum_descriptor_graph_->type,
                              first_sum_descriptor_graph_->metadata->descriptor,
                              *this, visited);
    first_sum_descriptor_graph_.reset();
    shares_sum_descriptors_ = true;
  }
}

void ImportDataHandle::RetainFirstSumDescriptorGraph(
    const xls::dslx::Type& type, ValueMetadataPtr metadata) {
  if (!shares_sum_descriptors_ && !first_sum_descriptor_graph_.has_value()) {
    first_sum_descriptor_graph_ =
        SumDescriptorGraph{type.CloneToUnique(), std::move(metadata)};
  }
}

struct ImportDataRegistry {
  absl::Mutex mutex;
  std::vector<ImportDataHandle*> owners;
};

ImportDataRegistry& GetImportDataRegistry() {
  // The registry itself outlives all C handles, including handles freed by a
  // client's static destructors. It does not own the registered ImportData.
  static auto* registry = new ImportDataRegistry;
  return *registry;
}

ImportDataHandle& UnwrapImportData(struct xls_dslx_import_data* data) {
  CHECK_NE(data, nullptr);
  return *reinterpret_cast<ImportDataHandle*>(data);
}

bool NeedsValueMetadata(const xls::dslx::Type& type) {
  if (type.IsEnum() || type.IsSum() || type.IsStruct()) {
    return true;
  } else if (auto* tuple = dynamic_cast<const xls::dslx::TupleType*>(&type)) {
    return std::any_of(
        tuple->members().begin(), tuple->members().end(),
        [](const auto& member) { return NeedsValueMetadata(*member); });
  } else if (auto* array = dynamic_cast<const xls::dslx::ArrayType*>(&type)) {
    // Empty arrays have no runtime element from which to recover the type.
    // Bits-like ArrayTypes such as uN[0] retain their width and signedness.
    const auto size = array->size().GetAsInt64();
    return (size.ok() && *size == 0 && !xls::dslx::IsBitsLike(type)) ||
           NeedsValueMetadata(array->element_type());
  } else {
    return false;
  }
}

absl::StatusOr<ValueTypeIdentity> MakeTypeIdentity(const xls::dslx::Type& type,
                                                   ImportDataHandle& owner) {
  if (auto bits = xls::dslx::GetBitsLike(type)) {
    XLS_ASSIGN_OR_RETURN(bool is_signed, bits->is_signed.GetAsBool());
    XLS_ASSIGN_OR_RETURN(int64_t size, bits->size.GetAsInt64());
    return ValueTypeIdentity{BitsTypeIdentity{is_signed, size}};
  } else if (auto* tuple = dynamic_cast<const xls::dslx::TupleType*>(&type)) {
    std::vector<ValueTypeIdentity> members;
    members.reserve(tuple->size());
    for (const auto& member : tuple->members()) {
      XLS_ASSIGN_OR_RETURN(auto identity, MakeTypeIdentity(*member, owner));
      members.push_back(std::move(identity));
    }
    return ValueTypeIdentity{std::move(members)};
  } else if (auto* array = dynamic_cast<const xls::dslx::ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t size, array->size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(auto element,
                         MakeTypeIdentity(array->element_type(), owner));
    return ValueTypeIdentity{ArrayTypeIdentity{
        size, std::make_shared<const ValueTypeIdentity>(std::move(element))}};
  } else if (const auto* enumeration =
                 dynamic_cast<const xls::dslx::EnumType*>(&type)) {
    return ValueTypeIdentity{owner.InternNominalType(*enumeration).identity};
  } else if (auto* sum = dynamic_cast<const xls::dslx::SumType*>(&type)) {
    return ValueTypeIdentity{owner.InternNominalType(*sum).identity};
  } else if (const auto* structure =
                 dynamic_cast<const xls::dslx::StructType*>(&type)) {
    return ValueTypeIdentity{owner.InternStructType(*structure)};
  } else if (type.IsToken()) {
    return ValueTypeIdentity{std::monostate{}};
  } else {
    return absl::InvalidArgumentError("Cannot retain a C value identity for " +
                                      type.ToString());
  }
}

template <typename NominalType, typename DescriptorFactory>
absl::StatusOr<ValueMetadataPtr> MakeNominalValueMetadata(
    const NominalType& type, ImportDataHandle& owner,
    const DescriptorFactory& make_descriptor, bool promote_on_miss = false) {
  // Sum entries are node-stable across recursive construction. Enum entries
  // need not be: their leaf descriptor builder cannot intern another enum.
  auto& entry = owner.InternNominalType(type);
  ValueMetadataPtr metadata = entry.value_metadata;
  if (metadata == nullptr && promote_on_miss &&
      owner.has_pending_sum_descriptor_graph()) {
    owner.PromoteSumDescriptorGraph();
    metadata = entry.value_metadata;
  }
  if (metadata == nullptr) {
    // The canonical builder preserves last-declared names for enum aliases
    // and describes every sum constructor. Published metadata remains
    // available between nonoverlapping C handles.
    XLS_ASSIGN_OR_RETURN(auto descriptor, make_descriptor());
    metadata = std::make_shared<const ValueMetadata>(
        ValueMetadata{ValueTypeIdentity{entry.identity}, std::move(descriptor),
                      type.IsSum()});
    entry.value_metadata = metadata;
  }
  return metadata;
}

absl::StatusOr<ValueMetadataPtr> MakeValueMetadata(const xls::dslx::Type& type,
                                                   ImportDataHandle& owner) {
  auto enum_provider = [&](const xls::dslx::EnumType& enum_type)
      -> absl::StatusOr<xls::dslx::ValueFormatDescriptor> {
    XLS_ASSIGN_OR_RETURN(auto metadata, MakeValueMetadata(enum_type, owner));
    return metadata->descriptor;
  };
  auto make_descriptor = [&](bool share_nested_sums)
      -> absl::StatusOr<xls::dslx::ValueFormatDescriptor> {
    if (share_nested_sums) {
      return xls::dslx::MakeDefaultValueFormatDescriptorWithNestedSumProvider(
          type, enum_provider,
          [&](const xls::dslx::SumType& nested_sum)
              -> absl::StatusOr<xls::dslx::ValueFormatDescriptor> {
            XLS_ASSIGN_OR_RETURN(auto metadata,
                                 MakeValueMetadata(nested_sum, owner));
            return metadata->descriptor;
          });
    } else {
      return xls::dslx::MakeValueFormatDescriptor(
          type, xls::FormatPreference::kDefault, enum_provider);
    }
  };
  if (const auto* enumeration =
          dynamic_cast<const xls::dslx::EnumType*>(&type)) {
    // A leaf uses the canonical table builder directly, without calling the
    // owner provider recursively for the same unfinished enum entry.
    return MakeNominalValueMetadata(*enumeration, owner, [&] {
      return xls::dslx::MakeValueFormatDescriptor(
          type, xls::FormatPreference::kDefault);
    });
  } else if (auto* sum = dynamic_cast<const xls::dslx::SumType*>(&type)) {
    bool built_descriptor = false;
    XLS_ASSIGN_OR_RETURN(
        auto metadata,
        MakeNominalValueMetadata(
            *sum, owner,
            [&] {
              built_descriptor = true;
              return make_descriptor(owner.shares_sum_descriptors());
            },
            /*promote_on_miss=*/true));
    if (built_descriptor) {
      owner.RetainFirstSumDescriptorGraph(type, metadata);
    }
    return metadata;
  } else {
    const bool contains_sum = xls::dslx::TypeContainsSemanticSum(type);
    if (contains_sum) {
      owner.PromoteSumDescriptorGraph();
    }
    XLS_ASSIGN_OR_RETURN(auto identity, MakeTypeIdentity(type, owner));
    XLS_ASSIGN_OR_RETURN(
        auto descriptor,
        make_descriptor(contains_sum && owner.shares_sum_descriptors()));
    ValueMetadataPtr metadata =
        std::make_shared<const ValueMetadata>(ValueMetadata{
            std::move(identity), std::move(descriptor), contains_sum});
    if (contains_sum) {
      owner.RetainFirstSumDescriptorGraph(type, metadata);
    }
    return metadata;
  }
}

ImportDataHandle* FindOwningImportData(const xls::dslx::TypeInfo& type_info) {
  auto& registry = GetImportDataRegistry();
  absl::MutexLock registry_lock(&registry.mutex);
  auto it = std::find_if(registry.owners.begin(), registry.owners.end(),
                         [&](const auto* candidate) {
                           return &candidate->data.file_table() ==
                                  &type_info.file_table();
                         });
  if (it != registry.owners.end()) {
    return *it;
  } else {
    return nullptr;
  }
}

absl::StatusOr<ValueMetadataPtr> MakeValueMetadata(
    const xls::dslx::Type& type, const xls::dslx::TypeInfo& type_info) {
  ImportDataHandle* owner = FindOwningImportData(type_info);
  if (owner == nullptr) {
    return absl::InvalidArgumentError("TypeInfo has no owning C ImportData");
  } else {
    // The caller must keep this borrowed TypeInfo's owner alive. No unmatched
    // owner escapes the registry lock, and we do not hold it while waiting.
    if (owner->metadata_lock_observer_for_testing) {
      owner->metadata_lock_observer_for_testing();
    }
    absl::MutexLock owner_lock(&owner->mutex);
    return MakeValueMetadata(type, *owner);
  }
}

absl::StatusOr<ValueMetadataPtr> MakeEnumMetadata(const xls::dslx::EnumDef& def,
                                                  bool is_signed,
                                                  const xls::Bits& bits) {
  ImportDataHandle* owner = nullptr;
  {
    auto& registry = GetImportDataRegistry();
    absl::MutexLock registry_lock(&registry.mutex);
    auto it = std::find_if(registry.owners.begin(), registry.owners.end(),
                           [&](const auto* candidate) {
                             return candidate->data.OwnsModule(def.owner());
                           });
    if (it != registry.owners.end()) {
      owner = *it;
    }
  }
  if (owner == nullptr) {
    return absl::InvalidArgumentError(
        "Enum definition has no owning C ImportData");
  } else {
    // The caller keeps the borrowed definition's owner alive; unrelated
    // contexts need the registry while this context waits for its operation.
    if (owner->metadata_lock_observer_for_testing) {
      owner->metadata_lock_observer_for_testing();
    }
    absl::MutexLock owner_lock(&owner->mutex);
    XLS_ASSIGN_OR_RETURN(auto* type_info, owner->data.GetRootTypeInfo());
    XLS_ASSIGN_OR_RETURN(auto* meta_type, type_info->GetItemOrError(&def));
    XLS_ASSIGN_OR_RETURN(const auto* type,
                         xls::dslx::UnwrapMetaType(*meta_type));
    if (auto* enum_type = dynamic_cast<const xls::dslx::EnumType*>(type)) {
      XLS_ASSIGN_OR_RETURN(int64_t bit_count, enum_type->size().GetAsInt64());
      ValueMetadataPtr metadata;
      bool is_member = false;
      if (enum_type->is_signed() == is_signed &&
          bit_count == bits.bit_count()) {
        if (const auto* entry = owner->FindEnumType(*enum_type)) {
          metadata = entry->value_metadata;
        }
        if (metadata != nullptr) {
          is_member = metadata->descriptor.value_to_name().contains(bits);
        } else {
          // An invalid-only raw constructor must not intern a nominal type or
          // build a formatting table merely to discover it is not a member.
          is_member =
              std::any_of(enum_type->members().begin(),
                          enum_type->members().end(), [&](const auto& member) {
                            return member.GetBitsOrDie() == bits;
                          });
        }
      }
      if (is_member && metadata != nullptr) {
        return metadata;
      } else if (is_member) {
        return MakeValueMetadata(*enum_type, *owner);
      } else {
        // Preserve the raw constructor's ordinary-value behavior, but do not
        // pass undeclared patterns or mismatched widths/signs to a semantic
        // enum formatter inside a later sum-bearing aggregate.
        return std::make_shared<const ValueMetadata>(
            ValueMetadata{std::nullopt,
                          xls::dslx::ValueFormatDescriptor::MakeLeafValue(
                              xls::FormatPreference::kDefault),
                          false});
      }
    } else {
      return absl::InvalidArgumentError("Expected a concrete enum type");
    }
  }
}

struct FormattedParametricBinding {
  std::string identifier;
  xls::dslx::InterpValue value;
  ValueMetadataPtr metadata;
};

class InterpValueHandle {
 public:
  explicit InterpValueHandle(xls::dslx::InterpValue value,
                             ValueMetadataPtr metadata = nullptr)
      : value_(std::in_place_type<xls::dslx::InterpValue>, std::move(value)),
        metadata_(std::move(metadata)) {}

  static std::unique_ptr<InterpValueHandle> Borrowed(
      const xls::dslx::InterpValue& value,
      ValueMetadataPtr metadata = nullptr) {
    return std::unique_ptr<InterpValueHandle>(
        new InterpValueHandle(&value, std::move(metadata)));
  }

  const xls::dslx::InterpValue& value() const {
    if (const auto* owned = std::get_if<xls::dslx::InterpValue>(&value_)) {
      return *owned;
    } else {
      return *std::get<const xls::dslx::InterpValue*>(value_);
    }
  }

  bool is_owned() const {
    return std::holds_alternative<xls::dslx::InterpValue>(value_);
  }

  const ValueMetadataPtr& metadata() const { return metadata_; }

 private:
  explicit InterpValueHandle(const xls::dslx::InterpValue* value,
                             ValueMetadataPtr metadata)
      : value_(value), metadata_(std::move(metadata)) {}

  std::variant<xls::dslx::InterpValue, const xls::dslx::InterpValue*> value_;
  ValueMetadataPtr metadata_;
};

class ParametricEnvHandle {
 public:
  explicit ParametricEnvHandle(xls::dslx::ParametricEnv env,
                               BindingMetadata binding_metadata = {})
      : env_(std::in_place_type<xls::dslx::ParametricEnv>, std::move(env)) {
    InitializeBindingViews(binding_metadata);
  }

  ParametricEnvHandle(const xls::dslx::ParametricEnv* env,
                      const BindingMetadata& binding_metadata)
      : env_(env) {
    CHECK_NE(env, nullptr);
    InitializeBindingViews(binding_metadata);
  }

  const xls::dslx::ParametricEnv& env() const {
    if (const auto* owned = std::get_if<xls::dslx::ParametricEnv>(&env_)) {
      return *owned;
    } else {
      return *std::get<const xls::dslx::ParametricEnv*>(env_);
    }
  }

  bool is_owned() const {
    return std::holds_alternative<xls::dslx::ParametricEnv>(env_);
  }

  const InterpValueHandle& binding_value(int64_t index) const {
    return *binding_values_.at(index);
  }

  BindingMetadata binding_metadata() const {
    BindingMetadata metadata;
    metadata.reserve(binding_values_.size());
    for (const auto& binding_value : binding_values_) {
      metadata.push_back(binding_value->metadata());
    }
    return metadata;
  }

  std::unique_ptr<ParametricEnvHandle> Clone() const {
    return std::make_unique<ParametricEnvHandle>(env(), binding_metadata());
  }

 private:
  void InitializeBindingViews(const BindingMetadata& binding_metadata) {
    CHECK(binding_metadata.empty() ||
          binding_metadata.size() == env().bindings().size());
    binding_values_.reserve(env().bindings().size());
    for (size_t i = 0; i < env().bindings().size(); ++i) {
      const xls::dslx::ParametricEnvItem& binding = env().bindings().at(i);
      binding_values_.push_back(InterpValueHandle::Borrowed(
          binding.value,
          binding_metadata.empty() ? nullptr : binding_metadata.at(i)));
    }
  }

  std::variant<xls::dslx::ParametricEnv, const xls::dslx::ParametricEnv*> env_;
  std::vector<std::unique_ptr<InterpValueHandle>> binding_values_;
};

class InvocationCalleeDataHandle {
 public:
  InvocationCalleeDataHandle(xls::dslx::InvocationCalleeData value,
                             const BindingMetadata& callee_metadata,
                             const BindingMetadata& caller_metadata)
      : value_(std::move(value)),
        callee_bindings_(&value_.callee_bindings, callee_metadata),
        caller_bindings_(&value_.caller_bindings, caller_metadata) {}

  const xls::dslx::InvocationCalleeData& value() const { return value_; }
  const ParametricEnvHandle& callee_bindings() const {
    return callee_bindings_;
  }
  const ParametricEnvHandle& caller_bindings() const {
    return caller_bindings_;
  }

  std::unique_ptr<InvocationCalleeDataHandle> Clone() const {
    return std::make_unique<InvocationCalleeDataHandle>(
        value_, callee_bindings_.binding_metadata(),
        caller_bindings_.binding_metadata());
  }

 private:
  xls::dslx::InvocationCalleeData value_;
  ParametricEnvHandle callee_bindings_;
  ParametricEnvHandle caller_bindings_;
};

const InterpValueHandle& UnwrapInterpValueHandle(
    const struct xls_dslx_interp_value* value) {
  CHECK_NE(value, nullptr);
  return *reinterpret_cast<const InterpValueHandle*>(value);
}

std::optional<ValueTypeIdentity> RuntimeTypeIdentity(
    const xls::dslx::InterpValue& value) {
  if (value.IsBits()) {
    return ValueTypeIdentity{
        BitsTypeIdentity{value.IsSigned(), value.GetBitsOrDie().bit_count()}};
  } else if (value.IsTuple() || value.IsArray()) {
    std::vector<ValueTypeIdentity> members;
    for (const auto& member : value.GetValuesOrDie()) {
      auto identity = RuntimeTypeIdentity(member);
      if (!identity.has_value()) {
        return std::nullopt;
      } else {
        members.push_back(std::move(*identity));
      }
    }
    if (value.IsTuple()) {
      return ValueTypeIdentity{std::move(members)};
    } else if (members.empty() || !std::all_of(members.begin(), members.end(),
                                               [&](const auto& member) {
                                                 return member ==
                                                        members.front();
                                               })) {
      return std::nullopt;
    } else {
      return ValueTypeIdentity{
          ArrayTypeIdentity{static_cast<int64_t>(members.size()),
                            std::make_shared<const ValueTypeIdentity>(
                                std::move(members.front()))}};
    }
  } else if (value.IsToken()) {
    return ValueTypeIdentity{std::monostate{}};
  } else {
    // Nominal identities cannot be recovered from their erased storage.
    return std::nullopt;
  }
}

enum class AggregateKind { kTuple, kArray };

absl::StatusOr<ValueMetadataPtr> MakeAggregateMetadata(
    absl::Span<xls_dslx_interp_value* const> elements, AggregateKind kind) {
  const bool has_metadata =
      std::any_of(elements.begin(), elements.end(), [](const auto* element) {
        return UnwrapInterpValueHandle(element).metadata() != nullptr;
      });
  if (!has_metadata) {
    return nullptr;
  } else {
    bool contains_sum = false;
    bool has_type = true;
    std::vector<ValueTypeIdentity> member_types;
    std::vector<xls::dslx::ValueFormatDescriptor> descriptors;
    member_types.reserve(elements.size());
    if (kind == AggregateKind::kTuple) {
      descriptors.reserve(elements.size());
    }
    for (const auto* element : elements) {
      const auto& handle = UnwrapInterpValueHandle(element);
      const auto& metadata = handle.metadata();
      auto identity = metadata != nullptr ? metadata->type
                                          : RuntimeTypeIdentity(handle.value());
      if (identity.has_value()) {
        member_types.push_back(std::move(*identity));
      } else {
        has_type = false;
      }
      if (metadata != nullptr) {
        contains_sum |= metadata->contains_sum;
      }
      if (kind == AggregateKind::kTuple) {
        descriptors.push_back(
            metadata != nullptr
                ? metadata->descriptor
                : xls::dslx::ValueFormatDescriptor::MakeLeafValue(
                      xls::FormatPreference::kDefault));
      }
    }
    if (kind == AggregateKind::kArray && has_type) {
      has_type = !member_types.empty() &&
                 std::all_of(member_types.begin(), member_types.end(),
                             [&](const auto& member) {
                               return member == member_types.front();
                             });
    }
    if (contains_sum && !has_type) {
      return absl::InvalidArgumentError(
          kind == AggregateKind::kArray
              ? "Sum-bearing array elements have incompatible DSLX types"
              : "Sum-bearing tuple contains an element with no DSLX type");
    } else {
      std::optional<ValueTypeIdentity> identity;
      auto descriptor = xls::dslx::ValueFormatDescriptor::MakeLeafValue(
          xls::FormatPreference::kDefault);
      if (kind == AggregateKind::kTuple) {
        descriptor = xls::dslx::ValueFormatDescriptor::MakeTuple(descriptors);
        if (has_type) {
          identity = ValueTypeIdentity{std::move(member_types)};
        }
      } else if (has_type) {
        const auto& first =
            UnwrapInterpValueHandle(elements.front()).metadata();
        descriptor = xls::dslx::ValueFormatDescriptor::MakeArray(
            first != nullptr ? first->descriptor : descriptor, elements.size());
        identity = ValueTypeIdentity{
            ArrayTypeIdentity{static_cast<int64_t>(elements.size()),
                              std::make_shared<const ValueTypeIdentity>(
                                  std::move(member_types.front()))}};
      }
      return std::make_shared<const ValueMetadata>(ValueMetadata{
          std::move(identity), std::move(descriptor), contains_sum});
    }
  }
}

std::string FormatInterpValueHandle(const InterpValueHandle& handle) {
  if (handle.metadata() != nullptr && handle.metadata()->contains_sum) {
    absl::StatusOr<std::string> formatted = handle.value().ToFormattedString(
        handle.metadata()->descriptor, /*include_type_prefix=*/true);
    CHECK_OK(formatted.status());
    return std::move(*formatted);
  } else {
    return handle.value().ToString();
  }
}

const ParametricEnvHandle& UnwrapParametricEnvHandle(
    const struct xls_dslx_parametric_env* env) {
  CHECK_NE(env, nullptr);
  return *reinterpret_cast<const ParametricEnvHandle*>(env);
}

const InvocationCalleeDataHandle& UnwrapInvocationCalleeDataHandle(
    const struct xls_dslx_invocation_callee_data* data) {
  CHECK_NE(data, nullptr);
  return *reinterpret_cast<const InvocationCalleeDataHandle*>(data);
}

struct CallGraphHolder {
  xls::dslx::TypeInfo* type_info;
  std::vector<const xls::dslx::Function*> functions;
  absl::flat_hash_map<const xls::dslx::Function*,
                      std::vector<const xls::dslx::Function*>>
      graph;
};

const struct xls_dslx_type* GetMetaTypeHelper(
    struct xls_dslx_type_info* type_info, xls::dslx::AstNode* cpp_node) {
  CHECK_NE(cpp_node, nullptr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  std::optional<xls::dslx::Type*> maybe_type = cpp_type_info->GetItem(cpp_node);
  if (!maybe_type.has_value()) {
    return nullptr;
  }
  CHECK_NE(maybe_type.value(), nullptr);
  // Should always have a metatype as its associated type.
  absl::StatusOr<const xls::dslx::Type*> unwrapped =
      xls::dslx::UnwrapMetaType(*maybe_type.value());
  CHECK_OK(unwrapped);
  return reinterpret_cast<const struct xls_dslx_type*>(*unwrapped);
}

bool BindingNeedsTypeLookup(const xls::dslx::ParametricEnvItem& binding) {
  // These values cannot contain nominal fields. In particular, builtins have
  // bits/type bindings without a published derived TypeInfo to look them up in.
  return !binding.value.IsBits() && !binding.value.IsTypeReference() &&
         !binding.value.IsToken();
}

BindingMetadata MakeBindingMetadata(const xls::dslx::ParametricEnv& env,
                                    const xls::dslx::AstNode* parametric_owner,
                                    const xls::dslx::TypeInfo* type_info,
                                    ImportDataHandle& owner) {
  BindingMetadata metadata;
  metadata.reserve(env.size());
  // Bindings are sorted by name, whereas function parametrics use declaration
  // order. Resolve their NameDefs in the captured concrete context while the
  // C owner is locked. Raw values cannot recover nominal types.
  for (const auto& binding : env.bindings()) {
    ValueMetadataPtr value_metadata;
    if (BindingNeedsTypeLookup(binding)) {
      CHECK_NE(parametric_owner, nullptr);
      CHECK_NE(type_info, nullptr);
      auto find_formal =
          [&](const auto& formals) -> const xls::dslx::ParametricBinding* {
        auto it = std::find_if(
            formals.begin(), formals.end(), [&](const auto* candidate) {
              return candidate->identifier() == binding.identifier;
            });
        return it == formals.end() ? nullptr : *it;
      };
      const xls::dslx::ParametricBinding* formal = nullptr;
      if (const auto* function =
              dynamic_cast<const xls::dslx::Function*>(parametric_owner)) {
        formal = find_formal(function->parametric_bindings());
        if (formal == nullptr) {
          if (auto target_struct = function->GetTargetStruct()) {
            formal = find_formal((*target_struct)->parametric_bindings());
          }
        }
      } else if (const auto* struct_def =
                     dynamic_cast<const xls::dslx::StructDefBase*>(
                         parametric_owner)) {
        formal = find_formal(struct_def->parametric_bindings());
      } else if (const auto* sum_def =
                     dynamic_cast<const xls::dslx::SumDef*>(parametric_owner)) {
        formal = find_formal(sum_def->parametric_bindings());
      }
      CHECK_NE(formal, nullptr);
      auto type = type_info->GetItemOrError(formal->name_def());
      CHECK_OK(type.status());
      if (NeedsValueMetadata(**type)) {
        auto result = MakeValueMetadata(**type, owner);
        CHECK_OK(result.status());
        value_metadata = std::move(*result);
      }
    }
    metadata.push_back(std::move(value_metadata));
  }
  return metadata;
}

struct InvocationCalleeDataArray {
  InvocationCalleeDataArray() = default;

  explicit InvocationCalleeDataArray(
      std::vector<xls::dslx::InvocationCalleeData> entries_in,
      const xls::dslx::TypeInfo& type_info, ImportDataHandle& owner) {
    entries.reserve(entries_in.size());
    for (xls::dslx::InvocationCalleeData& entry : entries_in) {
      const xls::dslx::TypeInfo& concrete_type_info =
          entry.derived_type_info == nullptr ? type_info
                                             : *entry.derived_type_info;
      BindingMetadata callee_metadata = MakeBindingMetadata(
          entry.callee_bindings, entry.callee, &concrete_type_info, owner);
      BindingMetadata caller_metadata = MakeBindingMetadata(
          entry.caller_bindings, entry.caller_parametric_owner,
          entry.caller_type_info, owner);
      entries.push_back(std::make_unique<InvocationCalleeDataHandle>(
          std::move(entry), callee_metadata, caller_metadata));
    }
  }

  std::vector<std::unique_ptr<InvocationCalleeDataHandle>> entries;
};

template <typename T>
xls::dslx::ModuleMember* FindModuleMemberForNode(T* node) {
  if (node == nullptr) {
    return nullptr;
  }
  xls::dslx::Module* module = node->owner();
  if (module == nullptr) {
    return nullptr;
  }
  for (xls::dslx::ModuleMember& member : module->top()) {
    if (std::holds_alternative<T*>(member) && std::get<T*>(member) == node) {
      return &member;
    }
  }
  return nullptr;
}

}  // namespace

namespace xls {

const dslx::ParametricEnv* UnwrapDslxParametricEnv(
    const struct xls_dslx_parametric_env* env) {
  if (env == nullptr) {
    return nullptr;
  } else {
    return &UnwrapParametricEnvHandle(env).env();
  }
}

void SetDslxImporterStackObserverForTesting(
    struct xls_dslx_import_data* import_data,
    std::function<void(const dslx::Span&, const std::filesystem::path&)>
        observer) {
  auto& owner = UnwrapImportData(import_data);
  absl::MutexLock owner_lock(&owner.mutex);
  owner.data.SetImporterStackObserver(std::move(observer));
}

void SetDslxMetadataLockObserverForTesting(
    struct xls_dslx_import_data* import_data, std::function<void()> observer) {
  auto& owner = UnwrapImportData(import_data);
  absl::MutexLock owner_lock(&owner.mutex);
  owner.metadata_lock_observer_for_testing = std::move(observer);
}

std::weak_ptr<const void> GetDslxValueMetadataForTesting(
    const struct xls_dslx_interp_value* value) {
  return UnwrapInterpValueHandle(value).metadata();
}

const dslx::ValueFormatDescriptor* GetDslxValueFormatDescriptorForTesting(
    const struct xls_dslx_interp_value* value) {
  const auto& metadata = UnwrapInterpValueHandle(value).metadata();
  if (metadata != nullptr) {
    return &metadata->descriptor;
  } else {
    return nullptr;
  }
}

std::weak_ptr<const void> GetDslxCachedEnumMetadataForTesting(
    struct xls_dslx_import_data* import_data,
    const struct xls_dslx_type* enum_type) {
  auto& owner = UnwrapImportData(import_data);
  absl::MutexLock owner_lock(&owner.mutex);
  CHECK_NE(enum_type, nullptr);
  const auto* type = reinterpret_cast<const dslx::Type*>(enum_type);
  CHECK(type->IsEnum());
  if (const auto* entry = owner.FindEnumType(type->AsEnum())) {
    return entry->value_metadata;
  } else {
    return {};
  }
}

}  // namespace xls

extern "C" {

bool xls_dslx_parametric_env_create(
    const struct xls_dslx_parametric_env_item* items, size_t items_count,
    char** error_out, struct xls_dslx_parametric_env** env_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(env_out, nullptr);
  *error_out = nullptr;
  if (items_count == 0) {
    *env_out = reinterpret_cast<xls_dslx_parametric_env*>(
        new ParametricEnvHandle(xls::dslx::ParametricEnv()));
    return true;
  }

  std::vector<FormattedParametricBinding> formatted_bindings;
  formatted_bindings.reserve(items_count);
  for (size_t i = 0; i < items_count; ++i) {
    const xls_dslx_parametric_env_item& it = items[i];
    CHECK_NE(it.identifier, nullptr);
    CHECK_NE(it.value, nullptr);
    const InterpValueHandle& value = UnwrapInterpValueHandle(it.value);
    formatted_bindings.push_back(FormattedParametricBinding{
        .identifier = it.identifier,
        .value = value.value(),
        .metadata = value.metadata(),
    });
  }

  std::stable_sort(
      formatted_bindings.begin(), formatted_bindings.end(),
      [](const FormattedParametricBinding& lhs,
         const FormattedParametricBinding& rhs) {
        return lhs.identifier < rhs.identifier ||
               (lhs.identifier == rhs.identifier && lhs.value < rhs.value);
      });
  std::vector<std::pair<std::string, xls::dslx::InterpValue>> values;
  BindingMetadata metadata;
  values.reserve(formatted_bindings.size());
  metadata.reserve(formatted_bindings.size());
  for (FormattedParametricBinding& binding : formatted_bindings) {
    values.emplace_back(std::move(binding.identifier),
                        std::move(binding.value));
    metadata.push_back(std::move(binding.metadata));
  }

  *env_out = reinterpret_cast<xls_dslx_parametric_env*>(new ParametricEnvHandle(
      xls::dslx::ParametricEnv(absl::MakeSpan(values)), std::move(metadata)));
  return true;
}

struct xls_dslx_parametric_env* xls_dslx_parametric_env_clone(
    const struct xls_dslx_parametric_env* env) {
  return reinterpret_cast<xls_dslx_parametric_env*>(
      UnwrapParametricEnvHandle(env).Clone().release());
}

bool xls_dslx_parametric_env_equals(const struct xls_dslx_parametric_env* lhs,
                                    const struct xls_dslx_parametric_env* rhs) {
  return UnwrapParametricEnvHandle(lhs).env() ==
         UnwrapParametricEnvHandle(rhs).env();
}

bool xls_dslx_parametric_env_less_than(
    const struct xls_dslx_parametric_env* lhs,
    const struct xls_dslx_parametric_env* rhs) {
  const auto& lhs_bindings = UnwrapParametricEnvHandle(lhs).env().bindings();
  const auto& rhs_bindings = UnwrapParametricEnvHandle(rhs).env().bindings();
  const int64_t common = std::min(lhs_bindings.size(), rhs_bindings.size());
  for (int64_t i = 0; i < common; ++i) {
    const auto& lhs_item = lhs_bindings[i];
    const auto& rhs_item = rhs_bindings[i];
    if (lhs_item.identifier < rhs_item.identifier) {
      return true;
    }
    if (rhs_item.identifier < lhs_item.identifier) {
      return false;
    }
    if (lhs_item.value < rhs_item.value) {
      return true;
    }
    if (rhs_item.value < lhs_item.value) {
      return false;
    }
  }
  return lhs_bindings.size() < rhs_bindings.size();
}

uint64_t xls_dslx_parametric_env_hash(
    const struct xls_dslx_parametric_env* env) {
  return static_cast<uint64_t>(
      absl::HashOf(UnwrapParametricEnvHandle(env).env()));
}

char* xls_dslx_parametric_env_to_string(
    const struct xls_dslx_parametric_env* env) {
  const ParametricEnvHandle& handle = UnwrapParametricEnvHandle(env);
  std::string formatted = "{";
  for (size_t i = 0; i < handle.env().bindings().size(); ++i) {
    if (i != 0) {
      formatted.append(", ");
    }
    absl::StrAppendFormat(&formatted, "%s: %s",
                          handle.env().bindings().at(i).identifier,
                          FormatInterpValueHandle(handle.binding_value(i)));
  }
  formatted.push_back('}');
  return xls::ToOwnedCString(formatted);
}

void xls_dslx_parametric_env_free(struct xls_dslx_parametric_env* env) {
  if (env != nullptr) {
    auto* handle = reinterpret_cast<ParametricEnvHandle*>(env);
    CHECK(handle->is_owned())
        << "Borrowed parametric environments must not be freed.";
    delete handle;
  }
}

int64_t xls_dslx_parametric_env_get_binding_count(
    const struct xls_dslx_parametric_env* env) {
  return UnwrapParametricEnvHandle(env).env().size();
}

const char* xls_dslx_parametric_env_get_binding_identifier(
    const struct xls_dslx_parametric_env* env, int64_t index) {
  const xls::dslx::ParametricEnvItem& item =
      UnwrapParametricEnvHandle(env).env().bindings().at(index);
  return item.identifier.c_str();
}

struct xls_dslx_interp_value* xls_dslx_parametric_env_get_binding_value(
    const struct xls_dslx_parametric_env* env, int64_t index) {
  return reinterpret_cast<xls_dslx_interp_value*>(
      const_cast<InterpValueHandle*>(
          &UnwrapParametricEnvHandle(env).binding_value(index)));
}

// InterpValue simple constructors
struct xls_dslx_interp_value* xls_dslx_interp_value_make_ubits(
    int64_t bit_count, uint64_t value) {
  auto* iv = new InterpValueHandle(xls::dslx::InterpValue::MakeUBits(
      bit_count, static_cast<int64_t>(value)));
  return reinterpret_cast<xls_dslx_interp_value*>(iv);
}

struct xls_dslx_interp_value* xls_dslx_interp_value_make_sbits(
    int64_t bit_count, int64_t value) {
  auto* iv = new InterpValueHandle(
      xls::dslx::InterpValue::MakeSBits(bit_count, value));
  return reinterpret_cast<xls_dslx_interp_value*>(iv);
}

bool xls_dslx_interp_value_make_enum(
    struct xls_dslx_enum_def* def, bool is_signed, const struct xls_bits* bits,
    char** error_out, struct xls_dslx_interp_value** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  auto* enum_def = reinterpret_cast<xls::dslx::EnumDef*>(def);
  const xls::Bits* cpp_bits = reinterpret_cast<const xls::Bits*>(bits);
  auto metadata = MakeEnumMetadata(*enum_def, is_signed, *cpp_bits);
  if (!metadata.ok()) {
    *error_out = xls::ToOwnedCString(metadata.status().ToString());
    *result_out = nullptr;
    return false;
  } else {
    auto iv = xls::dslx::InterpValue::MakeEnum(*cpp_bits, is_signed, enum_def);
    *result_out = reinterpret_cast<xls_dslx_interp_value*>(
        new InterpValueHandle(std::move(iv), std::move(*metadata)));
    return true;
  }
}

bool xls_dslx_interp_value_from_string(
    const char* text, const char* dslx_stdlib_path, char** error_out,
    struct xls_dslx_interp_value** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  auto status_or = xls::dslx::InterpValueFromString(
      std::string_view{text}, std::filesystem::path{dslx_stdlib_path});
  if (!status_or.ok()) {
    *result_out = nullptr;
    *error_out = xls::ToOwnedCString(status_or.status().ToString());
    return false;
  }
  *result_out = reinterpret_cast<xls_dslx_interp_value*>(
      new InterpValueHandle(std::move(status_or.value())));
  return true;
}

bool xls_dslx_interp_value_make_tuple(
    size_t element_count, struct xls_dslx_interp_value** elements,
    char** error_out, struct xls_dslx_interp_value** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  auto metadata = MakeAggregateMetadata(
      absl::MakeConstSpan(elements, element_count), AggregateKind::kTuple);
  if (!metadata.ok()) {
    *result_out = nullptr;
    *error_out = xls::ToOwnedCString(metadata.status().ToString());
    return false;
  } else {
    std::vector<xls::dslx::InterpValue> vec;
    vec.reserve(element_count);
    for (size_t i = 0; i < element_count; ++i) {
      vec.push_back(UnwrapInterpValueHandle(elements[i]).value());
    }
    auto value = xls::dslx::InterpValue::MakeTuple(std::move(vec));
    *result_out = reinterpret_cast<xls_dslx_interp_value*>(
        new InterpValueHandle(std::move(value), std::move(*metadata)));
    return true;
  }
}

bool xls_dslx_interp_value_make_array(
    size_t element_count, struct xls_dslx_interp_value** elements,
    char** error_out, struct xls_dslx_interp_value** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  auto metadata = MakeAggregateMetadata(
      absl::MakeConstSpan(elements, element_count), AggregateKind::kArray);
  if (!metadata.ok()) {
    *result_out = nullptr;
    *error_out = xls::ToOwnedCString(metadata.status().ToString());
    return false;
  } else {
    std::vector<xls::dslx::InterpValue> vec;
    vec.reserve(element_count);
    for (size_t i = 0; i < element_count; ++i) {
      vec.push_back(UnwrapInterpValueHandle(elements[i]).value());
    }
    auto array = xls::dslx::InterpValue::MakeArray(std::move(vec));
    if (!array.ok()) {
      *result_out = nullptr;
      *error_out = xls::ToOwnedCString(array.status().ToString());
      return false;
    } else {
      *result_out = reinterpret_cast<xls_dslx_interp_value*>(
          new InterpValueHandle(std::move(*array), std::move(*metadata)));
      return true;
    }
  }
}

struct xls_dslx_interp_value* xls_dslx_interp_value_clone(
    const struct xls_dslx_interp_value* value) {
  const InterpValueHandle& source = UnwrapInterpValueHandle(value);
  auto* heap = new InterpValueHandle(source.value(), source.metadata());
  return reinterpret_cast<xls_dslx_interp_value*>(heap);
}

struct xls_dslx_import_data* xls_dslx_import_data_create(
    const char* dslx_stdlib_path, const char* additional_search_paths[],
    size_t additional_search_paths_count) {
  std::filesystem::path cpp_stdlib_path{dslx_stdlib_path};
  std::vector<std::filesystem::path> cpp_additional_search_paths =
      xls::ToCppPaths(additional_search_paths, additional_search_paths_count);
  xls::dslx::ImportData import_data = CreateImportData(
      cpp_stdlib_path, cpp_additional_search_paths, xls::dslx::kAllWarningsSet,
      std::make_unique<xls::dslx::RealFilesystem>());
  auto* owner = new ImportDataHandle(std::move(import_data));
  auto& registry = GetImportDataRegistry();
  absl::MutexLock registry_lock(&registry.mutex);
  registry.owners.push_back(owner);
  return reinterpret_cast<xls_dslx_import_data*>(owner);
}

void xls_dslx_import_data_free(struct xls_dslx_import_data* x) {
  if (x != nullptr) {
    auto* owner = reinterpret_cast<ImportDataHandle*>(x);
    auto& registry = GetImportDataRegistry();
    {
      absl::MutexLock registry_lock(&registry.mutex);
      std::erase(registry.owners, owner);
    }
    delete owner;
  }
}

void xls_dslx_typechecked_module_free(struct xls_dslx_typechecked_module* tm) {
  delete reinterpret_cast<xls::dslx::TypecheckedModule*>(tm);
}

struct xls_dslx_module* xls_dslx_typechecked_module_get_module(
    struct xls_dslx_typechecked_module* tm) {
  auto* cpp_tm = reinterpret_cast<xls::dslx::TypecheckedModule*>(tm);
  xls::dslx::Module* cpp_module = cpp_tm->module;
  return reinterpret_cast<xls_dslx_module*>(cpp_module);
}

struct xls_dslx_type_info* xls_dslx_typechecked_module_get_type_info(
    struct xls_dslx_typechecked_module* tm) {
  auto* cpp_tm = reinterpret_cast<xls::dslx::TypecheckedModule*>(tm);
  xls::dslx::TypeInfo* cpp_type_info = cpp_tm->type_info;
  return reinterpret_cast<xls_dslx_type_info*>(cpp_type_info);
}

struct xls_dslx_type_info* xls_dslx_type_info_get_imported_type_info(
    struct xls_dslx_type_info* type_info, struct xls_dslx_module* module) {
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  std::optional<xls::dslx::TypeInfo*> imported =
      cpp_type_info->GetImportedTypeInfo(cpp_module);
  if (!imported.has_value()) {
    return nullptr;
  }
  return reinterpret_cast<xls_dslx_type_info*>(*imported);
}

bool xls_dslx_parse_and_typecheck(
    const char* text, const char* path, const char* module_name,
    struct xls_dslx_import_data* import_data, char** error_out,
    struct xls_dslx_typechecked_module** result_out) {
  auto& import_owner = UnwrapImportData(import_data);
  absl::MutexLock import_lock(&import_owner.mutex);
  auto* cpp_import_data = &import_owner.data;

  absl::StatusOr<xls::dslx::TypecheckedModule> tm =
      xls::dslx::ParseAndTypecheck(text, path, module_name, cpp_import_data);
  if (tm.ok()) {
    auto* tm_on_heap = new xls::dslx::TypecheckedModule{*std::move(tm)};
    *result_out = reinterpret_cast<xls_dslx_typechecked_module*>(tm_on_heap);
    *error_out = nullptr;
    return true;
  }

  *result_out = nullptr;
  *error_out = xls::ToOwnedCString(tm.status().ToString());
  return false;
}

bool xls_dslx_typechecked_module_clone_removing_functions(
    struct xls_dslx_typechecked_module* tm,
    struct xls_dslx_function* functions[], size_t function_count,
    const char* install_subject, struct xls_dslx_import_data* import_data,
    char** error_out, struct xls_dslx_typechecked_module** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  auto fail = [&](std::string_view message) {
    *error_out = xls::ToOwnedCString(std::string(message));
    *result_out = nullptr;
    return false;
  };
  if (function_count != 0 && functions == nullptr) {
    return fail("functions array is null");
  }
  std::vector<xls_dslx_module_member*> members;
  members.reserve(function_count);
  for (size_t i = 0; i < function_count; ++i) {
    auto* fn = reinterpret_cast<xls::dslx::Function*>(functions[i]);
    xls::dslx::ModuleMember* member = FindModuleMemberForNode(fn);
    if (member == nullptr) {
      return fail("function does not belong to the provided module");
    }
    members.push_back(reinterpret_cast<xls_dslx_module_member*>(member));
  }
  return xls_dslx_typechecked_module_clone_removing_members(
      tm, members.empty() ? nullptr : members.data(), function_count,
      install_subject, import_data, error_out, result_out);
}

bool xls_dslx_typechecked_module_clone_removing_members(
    struct xls_dslx_typechecked_module* tm,
    struct xls_dslx_module_member* members[], size_t member_count,
    const char* install_subject, struct xls_dslx_import_data* import_data,
    char** error_out, struct xls_dslx_typechecked_module** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  *result_out = nullptr;

  if (tm == nullptr || import_data == nullptr) {
    *error_out = xls::ToOwnedCString("null argument provided");
    return false;
  }
  if (member_count != 0 && members == nullptr) {
    *error_out = xls::ToOwnedCString("members array is null");
    return false;
  }

  auto* cpp_tm = reinterpret_cast<xls::dslx::TypecheckedModule*>(tm);
  auto& import_owner = UnwrapImportData(import_data);
  absl::MutexLock import_lock(&import_owner.mutex);
  auto* cpp_import_data = &import_owner.data;

  std::string subject = std::string(install_subject);
  if (subject.empty()) {
    *error_out = xls::ToOwnedCString("install_subject must not be empty");
    return false;
  }

  std::vector<const xls::dslx::AstNode*> nodes_to_remove;
  nodes_to_remove.reserve(member_count);
  for (size_t i = 0; i < member_count; ++i) {
    if (members[i] == nullptr) {
      *error_out = xls::ToOwnedCString("members array contains null entry");
      return false;
    }
    auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(members[i]);
    const xls::dslx::AstNode* node = xls::dslx::ToAstNode(*cpp_member);
    if (node == nullptr || node->owner() != cpp_tm->module) {
      *error_out = xls::ToOwnedCString(
          "module member does not belong to the provided module");
      return false;
    }
    nodes_to_remove.push_back(node);
  }

  absl::StatusOr<std::unique_ptr<xls::dslx::Module>> cloned_module_or =
      xls::dslx::CloneModuleRemovingMembers(*cpp_tm->module, nodes_to_remove);
  if (!cloned_module_or.ok()) {
    *error_out = xls::ToOwnedCString(cloned_module_or.status().ToString());
    return false;
  }

  std::unique_ptr<xls::dslx::Module> cloned_module =
      std::move(cloned_module_or).value();
  if (cloned_module->name() != subject) {
    cloned_module->SetName(subject);
  }
  std::string path = cpp_tm->module->fs_path().has_value()
                         ? cpp_tm->module->fs_path()->string()
                         : std::string(cpp_tm->module->name());

  absl::StatusOr<xls::dslx::TypecheckedModule> retyped =
      xls::dslx::TypecheckModule(std::move(cloned_module), path,
                                 cpp_import_data);
  if (!retyped.ok()) {
    *error_out = xls::ToOwnedCString(retyped.status().ToString());
    return false;
  }
  auto* new_tm = new xls::dslx::TypecheckedModule{std::move(retyped).value()};
  *result_out = reinterpret_cast<xls_dslx_typechecked_module*>(new_tm);
  return true;
}

int64_t xls_dslx_module_get_member_count(struct xls_dslx_module* module) {
  CHECK_NE(module, nullptr);
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  return cpp_module->top().size();
}

xls_dslx_module_member_kind xls_dslx_module_member_get_kind(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  xls::dslx::ModuleMember& cpp_member_ref = *cpp_member;
  xls_dslx_module_member_kind result = absl::visit(
      xls::Visitor{
          [](xls::dslx::Function*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_function;
          },
          [](xls::dslx::Proc*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_proc;
          },
          [](xls::dslx::ProcAlias*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_proc_alias;
          },
          [](xls::dslx::TestFunction*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_test_function;
          },
          [](xls::dslx::TestProc*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_test_proc;
          },
          [](xls::dslx::QuickCheck*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_quick_check;
          },
          [](xls::dslx::TypeAlias*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_type_alias;
          },
          [](xls::dslx::StructDef*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_struct_def;
          },
          [](xls::dslx::ProcDef*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_proc_def;
          },
          [](xls::dslx::ConstantDef*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_constant_def;
          },
          [](xls::dslx::EnumDef*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_enum_def;
          },
          [](xls::dslx::SumDef*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_sum_def;
          },
          [](xls::dslx::Import*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_import;
          },
          [](xls::dslx::Use*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_use;
          },
          [](xls::dslx::ConstAssert*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_const_assert;
          },
          [](xls::dslx::Impl*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_impl;
          },
          [](xls::dslx::Trait*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_trait;
          },
          [](xls::dslx::VerbatimNode*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_verbatim_node;
          },
          [](xls::dslx::FuzzTestFunction*&) -> xls_dslx_module_member_kind {
            return xls_dslx_module_member_kind_fuzz_test_function;
          },
      },
      cpp_member_ref);
  return result;
}

struct xls_dslx_module_member* xls_dslx_module_get_member(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::ModuleMember& cpp_member = cpp_module->top().at(i);
  return reinterpret_cast<xls_dslx_module_member*>(&cpp_member);
}

struct xls_dslx_constant_def* xls_dslx_module_member_get_constant_def(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_constant_def = std::get<xls::dslx::ConstantDef*>(*cpp_member);
  return reinterpret_cast<xls_dslx_constant_def*>(cpp_constant_def);
}

struct xls_dslx_struct_def* xls_dslx_module_member_get_struct_def(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_struct_def = std::get<xls::dslx::StructDef*>(*cpp_member);
  return reinterpret_cast<xls_dslx_struct_def*>(cpp_struct_def);
}

struct xls_dslx_enum_def* xls_dslx_module_member_get_enum_def(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_enum_def = std::get<xls::dslx::EnumDef*>(*cpp_member);
  return reinterpret_cast<xls_dslx_enum_def*>(cpp_enum_def);
}

struct xls_dslx_sum_def* xls_dslx_module_member_get_sum_def(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_sum_def = std::get<xls::dslx::SumDef*>(*cpp_member);
  return reinterpret_cast<xls_dslx_sum_def*>(cpp_sum_def);
}

struct xls_dslx_type_alias* xls_dslx_module_member_get_type_alias(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_type_alias = std::get<xls::dslx::TypeAlias*>(*cpp_member);
  return reinterpret_cast<xls_dslx_type_alias*>(cpp_type_alias);
}

struct xls_dslx_import* xls_dslx_module_member_get_import(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  if (std::holds_alternative<xls::dslx::Import*>(*cpp_member)) {
    auto* cpp_import = std::get<xls::dslx::Import*>(*cpp_member);
    return reinterpret_cast<xls_dslx_import*>(cpp_import);
  }
  return nullptr;
}

struct xls_dslx_function* xls_dslx_module_member_get_function(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  if (std::holds_alternative<xls::dslx::Function*>(*cpp_member)) {
    auto* cpp_function = std::get<xls::dslx::Function*>(*cpp_member);
    return reinterpret_cast<xls_dslx_function*>(cpp_function);
  }
  return nullptr;
}

struct xls_dslx_module_member* xls_dslx_module_member_from_constant_def(
    struct xls_dslx_constant_def* constant_def) {
  auto* cpp_constant_def =
      reinterpret_cast<xls::dslx::ConstantDef*>(constant_def);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_constant_def);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_struct_def(
    struct xls_dslx_struct_def* struct_def) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(struct_def);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_struct_def);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_enum_def(
    struct xls_dslx_enum_def* enum_def) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(enum_def);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_enum_def);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_sum_def(
    struct xls_dslx_sum_def* sum_def) {
  auto* cpp_sum_def = reinterpret_cast<xls::dslx::SumDef*>(sum_def);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_sum_def);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_type_alias(
    struct xls_dslx_type_alias* type_alias) {
  auto* cpp_type_alias = reinterpret_cast<xls::dslx::TypeAlias*>(type_alias);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_type_alias);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_function(
    struct xls_dslx_function* function) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(function);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_function);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

struct xls_dslx_module_member* xls_dslx_module_member_from_quickcheck(
    struct xls_dslx_quickcheck* quickcheck) {
  auto* cpp_qc = reinterpret_cast<xls::dslx::QuickCheck*>(quickcheck);
  xls::dslx::ModuleMember* member = FindModuleMemberForNode(cpp_qc);
  return reinterpret_cast<xls_dslx_module_member*>(member);
}

bool xls_dslx_function_is_parametric(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return cpp_function->IsParametric();
}

bool xls_dslx_function_is_public(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return cpp_function->is_public();
}

char* xls_dslx_function_get_identifier(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  const std::string& result = cpp_function->identifier();
  return xls::ToOwnedCString(result);
}

int64_t xls_dslx_function_get_param_count(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return static_cast<int64_t>(cpp_function->params().size());
}

int64_t xls_dslx_function_get_parametric_binding_count(
    struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return static_cast<int64_t>(cpp_function->parametric_bindings().size());
}

struct xls_dslx_param* xls_dslx_function_get_param(struct xls_dslx_function* fn,
                                                   int64_t index) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  xls::dslx::Param* cpp_param = cpp_function->params().at(index);
  return reinterpret_cast<xls_dslx_param*>(cpp_param);
}

struct xls_dslx_parametric_binding* xls_dslx_function_get_parametric_binding(
    struct xls_dslx_function* fn, int64_t index) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  xls::dslx::ParametricBinding* cpp_binding =
      cpp_function->parametric_bindings().at(index);
  return reinterpret_cast<xls_dslx_parametric_binding*>(cpp_binding);
}

char* xls_dslx_param_get_name(struct xls_dslx_param* p) {
  auto* cpp_param = reinterpret_cast<xls::dslx::Param*>(p);
  return xls::ToOwnedCString(cpp_param->name_def()->identifier());
}

struct xls_dslx_type_annotation* xls_dslx_param_get_type_annotation(
    struct xls_dslx_param* p) {
  auto* cpp_param = reinterpret_cast<xls::dslx::Param*>(p);
  xls::dslx::TypeAnnotation* cpp_ta = cpp_param->type_annotation();
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_ta);
}

struct xls_dslx_expr* xls_dslx_function_get_body(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  xls::dslx::StatementBlock* cpp_body = cpp_function->body();
  return reinterpret_cast<xls_dslx_expr*>(cpp_body);
}

struct xls_dslx_type_annotation* xls_dslx_function_get_return_type(
    struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  xls::dslx::TypeAnnotation* cpp_return_type = cpp_function->return_type();
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_return_type);
}

int64_t xls_dslx_function_get_attribute_count(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return static_cast<int64_t>(cpp_function->attributes().size());
}

struct xls_dslx_attribute* xls_dslx_function_get_attribute(
    struct xls_dslx_function* fn, int64_t index) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  xls::dslx::Attribute* cpp_attribute = cpp_function->attributes().at(index);
  return reinterpret_cast<xls_dslx_attribute*>(cpp_attribute);
}

xls_dslx_attribute_kind xls_dslx_attribute_get_kind(
    struct xls_dslx_attribute* attribute) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  switch (cpp_attribute->attribute_kind()) {
    case xls::AttributeKind::kCfg:
      return xls_dslx_attribute_kind_cfg;
    case xls::AttributeKind::kDslxFormatDisable:
      return xls_dslx_attribute_kind_dslx_format_disable;
    case xls::AttributeKind::kExternVerilog:
      return xls_dslx_attribute_kind_extern_verilog;
    case xls::AttributeKind::kSvType:
      return xls_dslx_attribute_kind_sv_type;
    case xls::AttributeKind::kTest:
      return xls_dslx_attribute_kind_test;
    case xls::AttributeKind::kTestProc:
      return xls_dslx_attribute_kind_test_proc;
    case xls::AttributeKind::kQuickcheck:
      return xls_dslx_attribute_kind_quickcheck;
    case xls::AttributeKind::kTrivialNext:
      return xls_dslx_attribute_kind_trivial_next;
    default:
      CHECK(false) << "Unhandled attribute kind";
  }
  return xls_dslx_attribute_kind_cfg;
}

int64_t xls_dslx_attribute_get_argument_count(
    struct xls_dslx_attribute* attribute) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  return static_cast<int64_t>(cpp_attribute->args().size());
}

xls_dslx_attribute_argument_kind xls_dslx_attribute_get_argument_kind(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  if (std::holds_alternative<std::string>(argument)) {
    return xls_dslx_attribute_argument_kind_string;
  }
  if (std::holds_alternative<xls::AttributeData::StringKeyValueArgument>(
          argument)) {
    return xls_dslx_attribute_argument_kind_string_key_value;
  }
  if (std::holds_alternative<xls::AttributeData::IntKeyValueArgument>(
          argument)) {
    return xls_dslx_attribute_argument_kind_int_key_value;
  }
  if (std::holds_alternative<xls::AttributeData::StringLiteralArgument>(
          argument)) {
    return xls_dslx_attribute_argument_kind_string_literal;
  }
  CHECK(false) << "Unexpected attribute argument kind";
  return xls_dslx_attribute_argument_kind_string;
}

char* xls_dslx_attribute_get_string_argument(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  const std::string* value = std::get_if<std::string>(&argument);
  CHECK_NE(value, nullptr) << "Attribute argument is not a string";
  return xls::ToOwnedCString(*value);
}

char* xls_dslx_attribute_get_string_literal_argument(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  const xls::AttributeData::StringLiteralArgument* value =
      std::get_if<xls::AttributeData::StringLiteralArgument>(&argument);
  CHECK_NE(value, nullptr) << "Attribute argument is not a string literal";
  return xls::ToOwnedCString(value->text);
}

char* xls_dslx_attribute_get_key_value_argument_key(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  if (const auto* kv =
          std::get_if<xls::AttributeData::StringKeyValueArgument>(&argument)) {
    return xls::ToOwnedCString(kv->first);
  }
  if (const auto* kv =
          std::get_if<xls::AttributeData::IntKeyValueArgument>(&argument)) {
    return xls::ToOwnedCString(kv->first);
  }
  CHECK(false) << "Attribute argument is not key/value";
  return nullptr;
}

char* xls_dslx_attribute_get_key_value_string_argument_value(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  const auto* kv =
      std::get_if<xls::AttributeData::StringKeyValueArgument>(&argument);
  CHECK_NE(kv, nullptr)
      << "Attribute argument is not a string key/value argument";
  return xls::ToOwnedCString(kv->second);
}

int64_t xls_dslx_attribute_get_key_value_int_argument_value(
    struct xls_dslx_attribute* attribute, int64_t index) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  const xls::AttributeData::Argument& argument =
      cpp_attribute->args().at(index);
  const auto* kv =
      std::get_if<xls::AttributeData::IntKeyValueArgument>(&argument);
  CHECK_NE(kv, nullptr)
      << "Attribute argument is not an int key/value argument";
  return kv->second;
}

char* xls_dslx_attribute_to_string(struct xls_dslx_attribute* attribute) {
  auto* cpp_attribute = reinterpret_cast<xls::dslx::Attribute*>(attribute);
  return xls::ToOwnedCString(cpp_attribute->ToString());
}

char* xls_dslx_parametric_binding_get_identifier(
    struct xls_dslx_parametric_binding* binding) {
  auto* cpp_binding = reinterpret_cast<xls::dslx::ParametricBinding*>(binding);
  return xls::ToOwnedCString(cpp_binding->identifier());
}

struct xls_dslx_type_annotation*
xls_dslx_parametric_binding_get_type_annotation(
    struct xls_dslx_parametric_binding* binding) {
  auto* cpp_binding = reinterpret_cast<xls::dslx::ParametricBinding*>(binding);
  xls::dslx::TypeAnnotation* cpp_type = cpp_binding->type_annotation();
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_type);
}

struct xls_dslx_expr* xls_dslx_parametric_binding_get_expr(
    struct xls_dslx_parametric_binding* binding) {
  auto* cpp_binding = reinterpret_cast<xls::dslx::ParametricBinding*>(binding);
  std::optional<xls::dslx::ExprOrType> cpp_default =
      cpp_binding->default_expr_or_type();
  return cpp_default.has_value() &&
                 std::holds_alternative<xls::dslx::Expr*>(*cpp_default)
             ? reinterpret_cast<xls_dslx_expr*>(
                   std::get<xls::dslx::Expr*>(*cpp_default))
             : nullptr;
}

struct xls_dslx_type_annotation*
xls_dslx_parametric_binding_get_default_generic_type(
    struct xls_dslx_parametric_binding* binding) {
  auto* cpp_binding = reinterpret_cast<xls::dslx::ParametricBinding*>(binding);
  std::optional<xls::dslx::ExprOrType> cpp_default =
      cpp_binding->default_expr_or_type();
  return cpp_default.has_value() &&
                 std::holds_alternative<xls::dslx::TypeAnnotation*>(
                     *cpp_default)
             ? reinterpret_cast<xls_dslx_type_annotation*>(
                   std::get<xls::dslx::TypeAnnotation*>(*cpp_default))
             : nullptr;
}

char* xls_dslx_function_to_string(struct xls_dslx_function* fn) {
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(fn);
  return xls::ToOwnedCString(cpp_function->ToString());
}

char* xls_dslx_expr_to_string(struct xls_dslx_expr* expr) {
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  return xls::ToOwnedCString(cpp_expr->ToString());
}

bool xls_dslx_type_info_build_function_call_graph(
    struct xls_dslx_type_info* type_info, char** error_out,
    struct xls_dslx_call_graph** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *result_out = nullptr;
  *error_out = xls::ToOwnedCString(
      "xls_dslx_type_info_build_function_call_graph is deprecated because "
      "TypeInfo is now module-independent. Use "
      "xls_dslx_type_info_build_function_call_graph_for_module instead.");
  return false;
}

bool xls_dslx_type_info_build_function_call_graph_for_module(
    struct xls_dslx_type_info* type_info, struct xls_dslx_module* module,
    char** error_out, struct xls_dslx_call_graph** result_out) {
  CHECK_NE(type_info, nullptr);
  CHECK_NE(module, nullptr);
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  *result_out = nullptr;

  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  auto graph = cpp_type_info->GetFunctionCallGraph(cpp_module);
  auto holder = std::make_unique<CallGraphHolder>();
  holder->type_info = cpp_type_info;
  holder->graph = std::move(graph);

  for (xls::dslx::ModuleMember& member : cpp_module->top()) {
    if (std::holds_alternative<xls::dslx::Function*>(member)) {
      const xls::dslx::Function* fn = std::get<xls::dslx::Function*>(member);
      holder->functions.push_back(fn);
      if (!holder->graph.contains(fn)) {
        holder->graph.emplace(fn, std::vector<const xls::dslx::Function*>{});
      }
    }
  }

  *result_out = reinterpret_cast<xls_dslx_call_graph*>(holder.release());
  return true;
}

void xls_dslx_call_graph_free(struct xls_dslx_call_graph* call_graph) {
  delete reinterpret_cast<CallGraphHolder*>(call_graph);
}

int64_t xls_dslx_call_graph_get_function_count(
    struct xls_dslx_call_graph* call_graph) {
  if (call_graph == nullptr) {
    return 0;
  }
  auto* holder = reinterpret_cast<CallGraphHolder*>(call_graph);
  return static_cast<int64_t>(holder->functions.size());
}

struct xls_dslx_function* xls_dslx_call_graph_get_function(
    struct xls_dslx_call_graph* call_graph, int64_t index) {
  if (call_graph == nullptr) {
    return nullptr;
  }
  auto* holder = reinterpret_cast<CallGraphHolder*>(call_graph);
  if (index < 0 || index >= static_cast<int64_t>(holder->functions.size())) {
    return nullptr;
  }
  const xls::dslx::Function* fn = holder->functions.at(index);
  return reinterpret_cast<xls_dslx_function*>(
      const_cast<xls::dslx::Function*>(fn));
}

int64_t xls_dslx_call_graph_get_callee_count(
    struct xls_dslx_call_graph* call_graph, struct xls_dslx_function* caller) {
  if (call_graph == nullptr || caller == nullptr) {
    return 0;
  }
  auto* holder = reinterpret_cast<CallGraphHolder*>(call_graph);
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(caller);
  auto it = holder->graph.find(cpp_function);
  if (it == holder->graph.end()) {
    return 0;
  }
  return static_cast<int64_t>(it->second.size());
}

struct xls_dslx_function* xls_dslx_call_graph_get_callee_function(
    struct xls_dslx_call_graph* call_graph, struct xls_dslx_function* caller,
    int64_t callee_index) {
  if (call_graph == nullptr || caller == nullptr) {
    return nullptr;
  }
  auto* holder = reinterpret_cast<CallGraphHolder*>(call_graph);
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(caller);
  auto it = holder->graph.find(cpp_function);
  if (it == holder->graph.end()) {
    return nullptr;
  }
  const std::vector<const xls::dslx::Function*>& callees = it->second;
  if (callee_index < 0 ||
      callee_index >= static_cast<int64_t>(callees.size())) {
    return nullptr;
  }
  const xls::dslx::Function* fn = callees.at(callee_index);
  return reinterpret_cast<xls_dslx_function*>(
      const_cast<xls::dslx::Function*>(fn));
}

bool xls_dslx_typechecked_module_insert_function_specializations(
    struct xls_dslx_typechecked_module* typechecked_module,
    const struct xls_dslx_function_specialization_request* requests,
    size_t request_count, struct xls_dslx_import_data* import_data,
    const char* install_subject, char** error_out,
    struct xls_dslx_typechecked_module** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  *result_out = nullptr;
  if (typechecked_module == nullptr || requests == nullptr ||
      request_count == 0 || install_subject == nullptr) {
    *error_out = xls::ToOwnedCString(
        absl::InvalidArgumentError("Invalid arguments").ToString());
    return false;
  }

  auto* cpp_tm =
      reinterpret_cast<xls::dslx::TypecheckedModule*>(typechecked_module);
  auto* cpp_module = cpp_tm->module;
  CHECK_NE(cpp_module, nullptr);

  absl::StatusOr<std::unique_ptr<xls::dslx::Module>> cloned_module_or =
      xls::dslx::CloneModule(*cpp_module);
  if (!cloned_module_or.ok()) {
    *error_out = xls::ToOwnedCString(cloned_module_or.status().ToString());
    return false;
  }
  std::unique_ptr<xls::dslx::Module> cloned_module =
      std::move(cloned_module_or.value());

  for (size_t i = 0; i < request_count; ++i) {
    const xls_dslx_function_specialization_request& req = requests[i];
    if (req.function_name == nullptr || req.specialized_name == nullptr) {
      *error_out = xls::ToOwnedCString(
          absl::InvalidArgumentError("Null function or specialized name")
              .ToString());
      return false;
    }

    std::optional<xls::dslx::Function*> cloned_function_opt =
        cloned_module->GetFunction(req.function_name);
    if (!cloned_function_opt.has_value() ||
        cloned_function_opt.value() == nullptr) {
      *error_out = xls::ToOwnedCString(
          absl::NotFoundError(
              absl::StrFormat("Function '%s' not found", req.function_name))
              .ToString());
      return false;
    }

    xls::dslx::ParametricEnv empty_env;
    const xls::dslx::ParametricEnv* request_env =
        xls::UnwrapDslxParametricEnv(req.env);
    const xls::dslx::ParametricEnv& env_ref =
        request_env != nullptr ? *request_env : empty_env;

    absl::StatusOr<xls::dslx::Function*> specialized_function_or =
        xls::dslx::InsertFunctionSpecialization(cloned_function_opt.value(),
                                                env_ref, req.specialized_name);
    if (!specialized_function_or.ok()) {
      *error_out =
          xls::ToOwnedCString(specialized_function_or.status().ToString());
      return false;
    }
  }

  if (import_data == nullptr) {
    *error_out = xls::ToOwnedCString(
        absl::InvalidArgumentError("ImportData must be provided").ToString());
    return false;
  } else {
    auto& import_owner = UnwrapImportData(import_data);
    absl::MutexLock import_lock(&import_owner.mutex);
    auto* cpp_import_data = &import_owner.data;
    std::string path = cpp_tm->module->fs_path().has_value()
                           ? cpp_tm->module->fs_path()->string()
                           : std::string(cpp_tm->module->name());
    cloned_module->SetName(install_subject);

    absl::StatusOr<xls::dslx::TypecheckedModule> retyped_or =
        xls::dslx::TypecheckModule(std::move(cloned_module), path,
                                   cpp_import_data);
    if (!retyped_or.ok()) {
      *error_out = xls::ToOwnedCString(retyped_or.status().ToString());
      return false;
    } else {
      auto* tm_on_heap =
          new xls::dslx::TypecheckedModule{std::move(retyped_or.value())};
      *result_out = reinterpret_cast<xls_dslx_typechecked_module*>(tm_on_heap);
      return true;
    }
  }
}

struct xls_dslx_quickcheck* xls_dslx_module_member_get_quickcheck(
    struct xls_dslx_module_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::ModuleMember*>(member);
  auto* cpp_qc = std::get<xls::dslx::QuickCheck*>(*cpp_member);
  return reinterpret_cast<xls_dslx_quickcheck*>(cpp_qc);
}
struct xls_dslx_function* xls_dslx_quickcheck_get_function(
    struct xls_dslx_quickcheck* quickcheck) {
  auto* cpp_qc = reinterpret_cast<xls::dslx::QuickCheck*>(quickcheck);
  xls::dslx::Function* cpp_fn = cpp_qc->fn();
  return reinterpret_cast<xls_dslx_function*>(cpp_fn);
}

bool xls_dslx_quickcheck_is_exhaustive(struct xls_dslx_quickcheck* quickcheck) {
  auto* cpp_qc = reinterpret_cast<xls::dslx::QuickCheck*>(quickcheck);
  return cpp_qc->test_cases().tag() ==
         xls::dslx::QuickCheckTestCasesTag::kExhaustive;
}

bool xls_dslx_quickcheck_get_count(struct xls_dslx_quickcheck* quickcheck,
                                   int64_t* result_out) {
  auto* cpp_qc = reinterpret_cast<xls::dslx::QuickCheck*>(quickcheck);
  const xls::dslx::QuickCheckTestCases& tc = cpp_qc->test_cases();
  if (tc.tag() != xls::dslx::QuickCheckTestCasesTag::kCounted) {
    return false;
  }
  std::optional<int64_t> count = tc.count();
  if (count.has_value()) {
    *result_out = *count;
  } else {
    *result_out = xls::dslx::QuickCheckTestCases::kDefaultTestCount;
  }
  return true;
}

char* xls_dslx_quickcheck_to_string(struct xls_dslx_quickcheck* quickcheck) {
  auto* cpp_qc = reinterpret_cast<xls::dslx::QuickCheck*>(quickcheck);
  return xls::ToOwnedCString(cpp_qc->ToString());
}

bool xls_dslx_type_info_get_requires_implicit_token(
    struct xls_dslx_type_info* type_info, struct xls_dslx_function* function,
    char** error_out, bool* result_out) {
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  CHECK_NE(cpp_type_info, nullptr);
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(function);
  CHECK_NE(cpp_function, nullptr);
  std::optional<bool> requires_implicit_token =
      cpp_type_info->GetRequiresImplicitToken(*cpp_function);
  if (!requires_implicit_token.has_value()) {
    *result_out = false;
    *error_out = xls::ToOwnedCString(
        absl::NotFoundError(
            "No implicit-token calling convention information for function")
            .ToString());
    return false;
  }

  *result_out = *requires_implicit_token;
  *error_out = nullptr;
  return true;
}

int64_t xls_dslx_module_get_type_definition_count(
    struct xls_dslx_module* module) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  return cpp_module->GetTypeDefinitions().size();
}

xls_dslx_type_definition_kind xls_dslx_module_get_type_definition_kind(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::TypeDefinition cpp_type_definition =
      cpp_module->GetTypeDefinitions().at(i);
  return absl::visit(xls::Visitor{
                         [](const xls::dslx::StructDef*) {
                           return xls_dslx_type_definition_kind_struct_def;
                         },
                         [](const xls::dslx::ProcDef*) {
                           return xls_dslx_type_definition_kind_proc_def;
                         },
                         [](const xls::dslx::EnumDef*) {
                           return xls_dslx_type_definition_kind_enum_def;
                         },
                         [](const xls::dslx::SumDef*) {
                           return xls_dslx_type_definition_kind_sum_def;
                         },
                         [](const xls::dslx::TypeAlias*) {
                           return xls_dslx_type_definition_kind_type_alias;
                         },
                         [](const xls::dslx::ColonRef*) {
                           return xls_dslx_type_definition_kind_colon_ref;
                         },
                         [](const xls::dslx::UseTreeEntry*) {
                           return xls_dslx_type_definition_kind_use_tree_entry;
                         },
                     },
                     cpp_type_definition);
}

char* xls_dslx_module_get_name(struct xls_dslx_module* module) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  const std::string& result = cpp_module->name();
  return xls::ToOwnedCString(result);
}

char* xls_dslx_module_to_string(struct xls_dslx_module* module) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  return xls::ToOwnedCString(cpp_module->ToString());
}

struct xls_dslx_struct_def* xls_dslx_module_get_type_definition_as_struct_def(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::TypeDefinition cpp_type_definition =
      cpp_module->GetTypeDefinitions().at(i);
  auto* cpp_struct_def = std::get<xls::dslx::StructDef*>(cpp_type_definition);
  return reinterpret_cast<xls_dslx_struct_def*>(cpp_struct_def);
}

struct xls_dslx_enum_def* xls_dslx_module_get_type_definition_as_enum_def(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::TypeDefinition cpp_type_definition =
      cpp_module->GetTypeDefinitions().at(i);
  auto* cpp_enum_def = std::get<xls::dslx::EnumDef*>(cpp_type_definition);
  return reinterpret_cast<xls_dslx_enum_def*>(cpp_enum_def);
}

struct xls_dslx_sum_def* xls_dslx_module_get_type_definition_as_sum_def(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::TypeDefinition cpp_type_definition =
      cpp_module->GetTypeDefinitions().at(i);
  auto* cpp_sum_def = std::get<xls::dslx::SumDef*>(cpp_type_definition);
  return reinterpret_cast<xls_dslx_sum_def*>(cpp_sum_def);
}

struct xls_dslx_type_alias* xls_dslx_module_get_type_definition_as_type_alias(
    struct xls_dslx_module* module, int64_t i) {
  auto* cpp_module = reinterpret_cast<xls::dslx::Module*>(module);
  xls::dslx::TypeDefinition cpp_type_definition =
      cpp_module->GetTypeDefinitions().at(i);
  auto* cpp_type_alias = std::get<xls::dslx::TypeAlias*>(cpp_type_definition);
  return reinterpret_cast<xls_dslx_type_alias*>(cpp_type_alias);
}

char* xls_dslx_struct_def_get_identifier(struct xls_dslx_struct_def* n) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  const std::string& result = cpp_struct_def->identifier();
  return xls::ToOwnedCString(result);
}

bool xls_dslx_struct_def_is_parametric(struct xls_dslx_struct_def* n) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  return cpp_struct_def->IsParametric();
}

int64_t xls_dslx_struct_def_get_parametric_binding_count(
    struct xls_dslx_struct_def* n) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  return static_cast<int64_t>(cpp_struct_def->parametric_bindings().size());
}

struct xls_dslx_parametric_binding* xls_dslx_struct_def_get_parametric_binding(
    struct xls_dslx_struct_def* n, int64_t index) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  xls::dslx::ParametricBinding* cpp_binding =
      cpp_struct_def->parametric_bindings().at(index);
  return reinterpret_cast<xls_dslx_parametric_binding*>(cpp_binding);
}

int64_t xls_dslx_struct_def_get_member_count(struct xls_dslx_struct_def* n) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  return cpp_struct_def->size();
}

char* xls_dslx_struct_def_to_string(struct xls_dslx_struct_def* n) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(n);
  return xls::ToOwnedCString(cpp_struct_def->ToString());
}

// -- colon_ref

struct xls_dslx_import* xls_dslx_colon_ref_resolve_import_subject(
    struct xls_dslx_colon_ref* n) {
  auto* cpp_colon_ref = reinterpret_cast<xls::dslx::ColonRef*>(n);
  std::optional<std::variant<xls::dslx::UseTreeEntry*, xls::dslx::Import*>>
      cpp_import = cpp_colon_ref->ResolveImportSubject();
  if (!cpp_import.has_value()) {
    return nullptr;
  }
  return absl::visit(
      xls::Visitor{
          [](xls::dslx::UseTreeEntry* entry) -> xls_dslx_import* {
            return nullptr;
          },
          [](xls::dslx::Import* import) -> xls_dslx_import* {
            return reinterpret_cast<xls_dslx_import*>(import);
          },
      },
      cpp_import.value());
}

char* xls_dslx_colon_ref_get_attr(struct xls_dslx_colon_ref* n) {
  auto* cpp_colon_ref = reinterpret_cast<xls::dslx::ColonRef*>(n);
  const std::string& result = cpp_colon_ref->attr();
  return xls::ToOwnedCString(result);
}

// -- type_alias

char* xls_dslx_type_alias_get_identifier(struct xls_dslx_type_alias* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeAlias*>(n);
  const std::string& result = cpp->identifier();
  return xls::ToOwnedCString(result);
}

struct xls_dslx_type_annotation* xls_dslx_type_alias_get_type_annotation(
    struct xls_dslx_type_alias* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeAlias*>(n);
  xls::dslx::TypeAnnotation& cpp_type_annotation = cpp->type_annotation();
  return reinterpret_cast<xls_dslx_type_annotation*>(&cpp_type_annotation);
}

char* xls_dslx_type_alias_to_string(struct xls_dslx_type_alias* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeAlias*>(n);
  return xls::ToOwnedCString(cpp->ToString());
}

// -- type_annotation

struct xls_dslx_type_ref_type_annotation*
xls_dslx_type_annotation_get_type_ref_type_annotation(
    struct xls_dslx_type_annotation* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeAnnotation*>(n);
  auto* cpp_type_ref = dynamic_cast<xls::dslx::TypeRefTypeAnnotation*>(cpp);
  return reinterpret_cast<xls_dslx_type_ref_type_annotation*>(cpp_type_ref);
}

struct xls_dslx_array_type_annotation*
xls_dslx_type_annotation_get_array_type_annotation(
    struct xls_dslx_type_annotation* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeAnnotation*>(n);
  auto* cpp_array = dynamic_cast<xls::dslx::ArrayTypeAnnotation*>(cpp);
  return reinterpret_cast<xls_dslx_array_type_annotation*>(cpp_array);
}

// -- array_type_annotation

struct xls_dslx_type_annotation*
xls_dslx_array_type_annotation_get_element_type(
    struct xls_dslx_array_type_annotation* n) {
  auto* cpp = reinterpret_cast<xls::dslx::ArrayTypeAnnotation*>(n);
  xls::dslx::TypeAnnotation* cpp_element_type = cpp->element_type();
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_element_type);
}

// -- type_ref_type_annotation

struct xls_dslx_type_ref* xls_dslx_type_ref_type_annotation_get_type_ref(
    struct xls_dslx_type_ref_type_annotation* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeRefTypeAnnotation*>(n);
  auto* cpp_type_ref = cpp->type_ref();
  return reinterpret_cast<xls_dslx_type_ref*>(cpp_type_ref);
}

int64_t xls_dslx_type_ref_type_annotation_get_parametric_count(
    struct xls_dslx_type_ref_type_annotation* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeRefTypeAnnotation*>(n);
  return static_cast<int64_t>(cpp->parametrics().size());
}

struct xls_dslx_expr* xls_dslx_type_ref_type_annotation_get_parametric_expr(
    struct xls_dslx_type_ref_type_annotation* n, int64_t index) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeRefTypeAnnotation*>(n);
  const xls::dslx::ExprOrType& parametric = cpp->parametrics().at(index);
  if (std::holds_alternative<xls::dslx::Expr*>(parametric)) {
    xls::dslx::Expr* cpp_expr = std::get<xls::dslx::Expr*>(parametric);
    return reinterpret_cast<xls_dslx_expr*>(cpp_expr);
  }
  return nullptr;
}

// -- type_ref

struct xls_dslx_type_definition* xls_dslx_type_ref_get_type_definition(
    struct xls_dslx_type_ref* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeRef*>(n);
  // const_cast is ok because the C API can only do immutable query-like things
  // with the node anyway.
  auto& cpp_type_def =
      const_cast<xls::dslx::TypeDefinition&>(cpp->type_definition());
  return reinterpret_cast<xls_dslx_type_definition*>(&cpp_type_def);
}

// -- type_definition

struct xls_dslx_colon_ref* xls_dslx_type_definition_get_colon_ref(
    struct xls_dslx_type_definition* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeDefinition*>(n);
  if (std::holds_alternative<xls::dslx::ColonRef*>(*cpp)) {
    auto* colon_ref = std::get<xls::dslx::ColonRef*>(*cpp);
    return reinterpret_cast<xls_dslx_colon_ref*>(colon_ref);
  }
  return nullptr;
}

struct xls_dslx_type_alias* xls_dslx_type_definition_get_type_alias(
    struct xls_dslx_type_definition* n) {
  auto* cpp = reinterpret_cast<xls::dslx::TypeDefinition*>(n);
  if (std::holds_alternative<xls::dslx::TypeAlias*>(*cpp)) {
    auto* type_alias = std::get<xls::dslx::TypeAlias*>(*cpp);
    return reinterpret_cast<xls_dslx_type_alias*>(type_alias);
  }
  return nullptr;
}

// -- import

int64_t xls_dslx_import_get_subject_count(struct xls_dslx_import* n) {
  auto* cpp = reinterpret_cast<xls::dslx::Import*>(n);
  return static_cast<int64_t>(cpp->subject().size());
}

char* xls_dslx_import_get_subject(struct xls_dslx_import* n, int64_t i) {
  auto* cpp = reinterpret_cast<xls::dslx::Import*>(n);
  const std::string& result = cpp->subject().at(i);
  return xls::ToOwnedCString(result);
}

// -- constant_def

char* xls_dslx_constant_def_get_name(struct xls_dslx_constant_def* n) {
  auto* cpp = reinterpret_cast<xls::dslx::ConstantDef*>(n);
  const std::string& result = cpp->name_def()->identifier();
  return xls::ToOwnedCString(result);
}

struct xls_dslx_expr* xls_dslx_constant_def_get_value(
    struct xls_dslx_constant_def* n) {
  auto* cpp = reinterpret_cast<xls::dslx::ConstantDef*>(n);
  xls::dslx::Expr* cpp_value = cpp->value();
  return reinterpret_cast<xls_dslx_expr*>(cpp_value);
}

char* xls_dslx_constant_def_to_string(struct xls_dslx_constant_def* n) {
  auto* cpp = reinterpret_cast<xls::dslx::ConstantDef*>(n);
  return xls::ToOwnedCString(cpp->ToString());
}

// -- enum_def

char* xls_dslx_enum_def_get_identifier(struct xls_dslx_enum_def* n) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(n);
  const std::string& result = cpp_enum_def->identifier();
  return xls::ToOwnedCString(result);
}

struct xls_dslx_type_annotation* xls_dslx_enum_def_get_underlying(
    struct xls_dslx_enum_def* n) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(n);
  auto* cpp_type_annotation = cpp_enum_def->type_annotation();
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_type_annotation);
}

int64_t xls_dslx_enum_def_get_member_count(struct xls_dslx_enum_def* n) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(n);
  return static_cast<int64_t>(cpp_enum_def->values().size());
}

struct xls_dslx_enum_member* xls_dslx_enum_def_get_member(
    struct xls_dslx_enum_def* n, int64_t i) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(n);
  xls::dslx::EnumMember& cpp_member = cpp_enum_def->mutable_values().at(i);
  return reinterpret_cast<xls_dslx_enum_member*>(&cpp_member);
}

char* xls_dslx_enum_member_get_name(struct xls_dslx_enum_member* m) {
  auto* cpp_member = reinterpret_cast<xls::dslx::EnumMember*>(m);
  return xls::ToOwnedCString(cpp_member->name_def->identifier());
}

struct xls_dslx_expr* xls_dslx_enum_member_get_value(
    struct xls_dslx_enum_member* m) {
  auto* cpp_member = reinterpret_cast<xls::dslx::EnumMember*>(m);
  xls::dslx::Expr* cpp_value = cpp_member->value;
  return reinterpret_cast<xls_dslx_expr*>(cpp_value);
}

char* xls_dslx_enum_def_to_string(struct xls_dslx_enum_def* n) {
  auto* cpp_enum_def = reinterpret_cast<xls::dslx::EnumDef*>(n);
  return xls::ToOwnedCString(cpp_enum_def->ToString());
}

char* xls_dslx_sum_def_get_identifier(struct xls_dslx_sum_def* n) {
  auto* cpp_sum_def = reinterpret_cast<xls::dslx::SumDef*>(n);
  return xls::ToOwnedCString(cpp_sum_def->identifier());
}

bool xls_dslx_sum_def_is_parametric(struct xls_dslx_sum_def* n) {
  auto* cpp_sum_def = reinterpret_cast<xls::dslx::SumDef*>(n);
  return cpp_sum_def->IsParametric();
}

int64_t xls_dslx_sum_def_get_variant_count(struct xls_dslx_sum_def* n) {
  auto* cpp_sum_def = reinterpret_cast<xls::dslx::SumDef*>(n);
  return cpp_sum_def->variants().size();
}

char* xls_dslx_sum_def_to_string(struct xls_dslx_sum_def* n) {
  auto* cpp_sum_def = reinterpret_cast<xls::dslx::SumDef*>(n);
  return xls::ToOwnedCString(cpp_sum_def->ToString());
}

struct xls_dslx_module* xls_dslx_expr_get_owner_module(
    struct xls_dslx_expr* expr) {
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  xls::dslx::Module* cpp_module = cpp_expr->owner();
  return reinterpret_cast<xls_dslx_module*>(cpp_module);
}

// -- type_info

const struct xls_dslx_type* xls_dslx_type_info_get_type_struct_def(
    struct xls_dslx_type_info* type_info,
    struct xls_dslx_struct_def* struct_def) {
  auto* node = reinterpret_cast<xls::dslx::AstNode*>(struct_def);
  return GetMetaTypeHelper(type_info, node);
}

const struct xls_dslx_type* xls_dslx_type_info_get_type_struct_member(
    struct xls_dslx_type_info* type_info,
    struct xls_dslx_struct_member* struct_member) {
  // Note: StructMember is not itself an AST node, it's just a POD struct, so
  // we need to traverse to its type annotation.
  auto* cpp_struct_member =
      reinterpret_cast<xls::dslx::StructMember*>(struct_member);
  xls::dslx::TypeAnnotation* node = cpp_struct_member->type;
  return GetMetaTypeHelper(type_info, node);
}

const struct xls_dslx_type* xls_dslx_type_info_get_type_enum_def(
    struct xls_dslx_type_info* type_info, struct xls_dslx_enum_def* enum_def) {
  auto* node = reinterpret_cast<xls::dslx::AstNode*>(enum_def);
  return GetMetaTypeHelper(type_info, node);
}

const struct xls_dslx_type* xls_dslx_type_info_get_type_sum_def(
    struct xls_dslx_type_info* type_info, struct xls_dslx_sum_def* sum_def) {
  auto* node = reinterpret_cast<xls::dslx::AstNode*>(sum_def);
  return GetMetaTypeHelper(type_info, node);
}

const struct xls_dslx_type* xls_dslx_type_info_get_type_constant_def(
    struct xls_dslx_type_info* type_info,
    struct xls_dslx_constant_def* constant_def) {
  auto* cpp_node = reinterpret_cast<xls::dslx::AstNode*>(constant_def);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  std::optional<xls::dslx::Type*> maybe_type = cpp_type_info->GetItem(cpp_node);
  if (!maybe_type.has_value()) {
    return nullptr;
  }
  CHECK_NE(maybe_type.value(), nullptr);
  xls::dslx::Type* cpp_type = maybe_type.value();
  return reinterpret_cast<const struct xls_dslx_type*>(cpp_type);
}

const struct xls_dslx_type* xls_dslx_type_info_get_type_type_annotation(
    struct xls_dslx_type_info* type_info,
    struct xls_dslx_type_annotation* type_annotation) {
  auto* node = reinterpret_cast<xls::dslx::AstNode*>(type_annotation);
  return GetMetaTypeHelper(type_info, node);
}

bool xls_dslx_type_get_total_bit_count(const struct xls_dslx_type* type,
                                       char** error_out, int64_t* result_out) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  absl::StatusOr<xls::dslx::TypeDim> bit_count = cpp_type->GetTotalBitCount();
  if (!bit_count.ok()) {
    *result_out = 0;
    *error_out = xls::ToOwnedCString(bit_count.status().ToString());
    return false;
  }

  absl::StatusOr<int64_t> width_or = bit_count->GetAsInt64();
  if (!width_or.ok()) {
    *result_out = 0;
    *error_out = xls::ToOwnedCString(width_or.status().ToString());
    return false;
  }

  *result_out = width_or.value();
  *error_out = nullptr;
  return true;
}

struct xls_dslx_struct_member* xls_dslx_struct_def_get_member(
    struct xls_dslx_struct_def* struct_def, int64_t i) {
  auto* cpp_struct_def = reinterpret_cast<xls::dslx::StructDef*>(struct_def);
  xls::dslx::StructMember& cpp_member = cpp_struct_def->mutable_members().at(i);
  return reinterpret_cast<xls_dslx_struct_member*>(&cpp_member);
}

struct xls_dslx_type_annotation* xls_dslx_struct_member_get_type(
    struct xls_dslx_struct_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::StructMember*>(member);
  xls::dslx::TypeAnnotation* cpp_type_annotation = cpp_member->type;
  return reinterpret_cast<xls_dslx_type_annotation*>(cpp_type_annotation);
}

char* xls_dslx_struct_member_get_name(struct xls_dslx_struct_member* member) {
  auto* cpp_member = reinterpret_cast<xls::dslx::StructMember*>(member);
  const std::string& name = cpp_member->name;
  return xls::ToOwnedCString(name);
}

bool xls_dslx_type_info_get_const_expr(
    struct xls_dslx_type_info* type_info, struct xls_dslx_expr* expr,
    char** error_out, struct xls_dslx_interp_value** result_out) {
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  absl::StatusOr<xls::dslx::InterpValue> value =
      cpp_type_info->GetConstExpr(cpp_expr);
  if (!value.ok()) {
    *result_out = nullptr;
    *error_out = xls::ToOwnedCString(value.status().ToString());
    return false;
  }

  ValueMetadataPtr metadata;
  std::optional<xls::dslx::Type*> concrete_type;
  // Plain bits retain their own width and signedness. Enums and aggregate
  // carriers still need declared-type metadata, even when their payload is empty.
  if (!value->IsBits()) {
    concrete_type = cpp_type_info->GetItem(cpp_expr);
  }
  if (concrete_type.has_value() && NeedsValueMetadata(**concrete_type)) {
    auto metadata_or = MakeValueMetadata(**concrete_type, *cpp_type_info);
    if (!metadata_or.ok()) {
      *result_out = nullptr;
      *error_out = xls::ToOwnedCString(metadata_or.status().ToString());
      return false;
    } else {
      metadata = std::move(*metadata_or);
    }
  }

  auto* heap = new InterpValueHandle(*std::move(value), std::move(metadata));
  *result_out = reinterpret_cast<xls_dslx_interp_value*>(heap);
  *error_out = nullptr;
  return true;
}

struct xls_dslx_invocation_callee_data_array*
xls_dslx_type_info_get_unique_invocation_callee_data(
    struct xls_dslx_type_info* type_info, struct xls_dslx_function* function) {
  CHECK_NE(type_info, nullptr);
  CHECK_NE(function, nullptr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(function);
  ImportDataHandle* owner = FindOwningImportData(*cpp_type_info);
  CHECK_NE(owner, nullptr);
  if (owner->metadata_lock_observer_for_testing) {
    owner->metadata_lock_observer_for_testing();
  }
  // Registry lookup is complete before waiting on this owner's metadata lock.
  absl::MutexLock owner_lock(&owner->mutex);
  std::vector<xls::dslx::InvocationCalleeData> entries =
      cpp_type_info->GetUniqueInvocationCalleeData(cpp_function);
  auto* array =
      new InvocationCalleeDataArray(std::move(entries), *cpp_type_info, *owner);
  return reinterpret_cast<xls_dslx_invocation_callee_data_array*>(array);
}

struct xls_dslx_invocation_callee_data_array*
xls_dslx_type_info_get_all_invocation_callee_data(
    struct xls_dslx_type_info* type_info, struct xls_dslx_function* function) {
  CHECK_NE(type_info, nullptr);
  CHECK_NE(function, nullptr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_function = reinterpret_cast<xls::dslx::Function*>(function);
  ImportDataHandle* owner = FindOwningImportData(*cpp_type_info);
  CHECK_NE(owner, nullptr);
  if (owner->metadata_lock_observer_for_testing) {
    owner->metadata_lock_observer_for_testing();
  }
  absl::MutexLock owner_lock(&owner->mutex);
  std::vector<xls::dslx::InvocationCalleeData> entries =
      cpp_type_info->GetAllInvocationCalleeData(cpp_function);
  auto* array =
      new InvocationCalleeDataArray(std::move(entries), *cpp_type_info, *owner);
  return reinterpret_cast<xls_dslx_invocation_callee_data_array*>(array);
}

struct xls_dslx_invocation_data* xls_dslx_type_info_get_root_invocation_data(
    struct xls_dslx_type_info* type_info,
    struct xls_dslx_invocation* invocation) {
  CHECK_NE(type_info, nullptr);
  CHECK_NE(invocation, nullptr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_invocation = reinterpret_cast<xls::dslx::Invocation*>(invocation);
  std::optional<const xls::dslx::InvocationData*> result =
      cpp_type_info->GetInvocationData(cpp_invocation);
  if (!result.has_value()) {
    return nullptr;
  }
  return reinterpret_cast<xls_dslx_invocation_data*>(
      const_cast<xls::dslx::InvocationData*>(*result));
}

void xls_dslx_invocation_callee_data_array_free(
    struct xls_dslx_invocation_callee_data_array* array) {
  if (array == nullptr) {
    return;
  }
  auto* cpp_array = reinterpret_cast<InvocationCalleeDataArray*>(array);
  delete cpp_array;
}

int64_t xls_dslx_invocation_callee_data_array_get_count(
    struct xls_dslx_invocation_callee_data_array* array) {
  CHECK_NE(array, nullptr);
  auto* cpp_array = reinterpret_cast<InvocationCalleeDataArray*>(array);
  return cpp_array->entries.size();
}

struct xls_dslx_invocation_callee_data*
xls_dslx_invocation_callee_data_array_get(
    struct xls_dslx_invocation_callee_data_array* array, int64_t index) {
  CHECK_NE(array, nullptr);
  auto* cpp_array = reinterpret_cast<InvocationCalleeDataArray*>(array);
  return reinterpret_cast<xls_dslx_invocation_callee_data*>(
      cpp_array->entries.at(index).get());
}

struct xls_dslx_invocation_callee_data* xls_dslx_invocation_callee_data_clone(
    struct xls_dslx_invocation_callee_data* data) {
  const InvocationCalleeDataHandle& source =
      UnwrapInvocationCalleeDataHandle(data);
  return reinterpret_cast<xls_dslx_invocation_callee_data*>(
      source.Clone().release());
}

void xls_dslx_invocation_callee_data_free(
    struct xls_dslx_invocation_callee_data* data) {
  if (data == nullptr) {
    return;
  }
  delete reinterpret_cast<InvocationCalleeDataHandle*>(data);
}

const struct xls_dslx_parametric_env*
xls_dslx_invocation_callee_data_get_callee_bindings(
    struct xls_dslx_invocation_callee_data* data) {
  return reinterpret_cast<const struct xls_dslx_parametric_env*>(
      &UnwrapInvocationCalleeDataHandle(data).callee_bindings());
}

const struct xls_dslx_parametric_env*
xls_dslx_invocation_callee_data_get_caller_bindings(
    struct xls_dslx_invocation_callee_data* data) {
  return reinterpret_cast<const struct xls_dslx_parametric_env*>(
      &UnwrapInvocationCalleeDataHandle(data).caller_bindings());
}

struct xls_dslx_type_info*
xls_dslx_invocation_callee_data_get_derived_type_info(
    struct xls_dslx_invocation_callee_data* data) {
  return reinterpret_cast<xls_dslx_type_info*>(
      UnwrapInvocationCalleeDataHandle(data).value().derived_type_info);
}

struct xls_dslx_invocation* xls_dslx_invocation_callee_data_get_invocation(
    struct xls_dslx_invocation_callee_data* data) {
  return reinterpret_cast<xls_dslx_invocation*>(
      const_cast<xls::dslx::Invocation*>(
          UnwrapInvocationCalleeDataHandle(data).value().invocation));
}

struct xls_dslx_invocation* xls_dslx_invocation_data_get_invocation(
    struct xls_dslx_invocation_data* data) {
  CHECK_NE(data, nullptr);
  auto* cpp_data = reinterpret_cast<xls::dslx::InvocationData*>(data);
  return reinterpret_cast<xls_dslx_invocation*>(
      const_cast<xls::dslx::Invocation*>(cpp_data->node()));
}

struct xls_dslx_function* xls_dslx_invocation_data_get_callee(
    struct xls_dslx_invocation_data* data) {
  CHECK_NE(data, nullptr);
  auto* cpp_data = reinterpret_cast<xls::dslx::InvocationData*>(data);
  return reinterpret_cast<xls_dslx_function*>(
      const_cast<xls::dslx::Function*>(cpp_data->callee()));
}

struct xls_dslx_function* xls_dslx_invocation_data_get_caller(
    struct xls_dslx_invocation_data* data) {
  CHECK_NE(data, nullptr);
  auto* cpp_data = reinterpret_cast<xls::dslx::InvocationData*>(data);
  return reinterpret_cast<xls_dslx_function*>(
      const_cast<xls::dslx::Function*>(cpp_data->caller()));
}

// -- interp_value

char* xls_dslx_interp_value_to_string(struct xls_dslx_interp_value* v) {
  return xls::ToOwnedCString(
      FormatInterpValueHandle(UnwrapInterpValueHandle(v)));
}

void xls_dslx_interp_value_free(struct xls_dslx_interp_value* v) {
  if (v != nullptr) {
    auto* handle = reinterpret_cast<InterpValueHandle*>(v);
    CHECK(handle->is_owned())
        << "Borrowed interpreter values must not be freed.";
    delete handle;
  }
}

bool xls_dslx_interp_value_convert_to_ir(const struct xls_dslx_interp_value* v,
                                         char** error_out,
                                         struct xls_value** result_out) {
  absl::StatusOr<xls::Value> ir_value =
      UnwrapInterpValueHandle(v).value().ConvertToIr();
  if (!ir_value.ok()) {
    *error_out = xls::ToOwnedCString(ir_value.status().ToString());
    *result_out = nullptr;
    return false;
  }

  auto* heap = new xls::Value{*std::move(ir_value)};
  *result_out = reinterpret_cast<xls_value*>(heap);
  *error_out = nullptr;
  return true;
}

// -- type

bool xls_dslx_type_is_signed_bits(const struct xls_dslx_type* type,
                                  char** error_out, bool* result_out) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  absl::StatusOr<bool> is_signed = xls::dslx::IsSigned(*cpp_type);
  if (!is_signed.ok()) {
    *error_out = xls::ToOwnedCString(is_signed.status().ToString());
    *result_out = false;
    return false;
  }

  *error_out = nullptr;
  *result_out = *is_signed;
  return true;
}

bool xls_dslx_type_is_enum(const struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  return cpp_type->IsEnum();
}

bool xls_dslx_type_is_struct(const struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  return cpp_type->IsStruct();
}

bool xls_dslx_type_is_array(const struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  return cpp_type->IsArray();
}

int64_t xls_dslx_type_struct_get_member_count(
    const struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  CHECK(cpp_type->IsStruct());
  return cpp_type->AsStruct().size();
}

const struct xls_dslx_type* xls_dslx_type_struct_get_member_type(
    const struct xls_dslx_type* type, int64_t index) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  CHECK(cpp_type->IsStruct());
  const xls::dslx::Type& cpp_member_type =
      cpp_type->AsStruct().GetMemberType(index);
  return reinterpret_cast<const xls_dslx_type*>(&cpp_member_type);
}

struct xls_dslx_type* xls_dslx_type_array_get_element_type(
    struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  CHECK(cpp_type->IsArray());
  const xls::dslx::Type& cpp_element_type = cpp_type->AsArray().element_type();
  const auto* element_type =
      reinterpret_cast<const xls_dslx_type*>(&cpp_element_type);
  // const_cast is ok because the C API can only do immutable query-like things
  // with the type anyway.
  return const_cast<xls_dslx_type*>(element_type);
}

struct xls_dslx_type_dim* xls_dslx_type_array_get_size(
    struct xls_dslx_type* type) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  CHECK(cpp_type->IsArray());
  const xls::dslx::TypeDim& cpp_size = cpp_type->AsArray().size();
  auto* cpp_type_dim = new xls::dslx::TypeDim(cpp_size);
  return reinterpret_cast<xls_dslx_type_dim*>(cpp_type_dim);
}

struct xls_dslx_enum_def* xls_dslx_type_get_enum_def(
    struct xls_dslx_type* type) {
  auto* cpp_type = reinterpret_cast<xls::dslx::Type*>(type);
  CHECK(cpp_type->IsEnum());
  const xls::dslx::EnumType& enum_type = cpp_type->AsEnum();
  const xls::dslx::EnumDef& cpp_enum_def = enum_type.nominal_type();
  const auto* enum_def =
      reinterpret_cast<const xls_dslx_enum_def*>(&cpp_enum_def);
  // const_cast is ok because the C API can only do immutable query-like things
  // with the node anyway.
  return const_cast<xls_dslx_enum_def*>(enum_def);
}

struct xls_dslx_struct_def* xls_dslx_type_get_struct_def(
    struct xls_dslx_type* type) {
  auto* cpp_type = reinterpret_cast<xls::dslx::Type*>(type);
  CHECK(cpp_type->IsStruct());
  const xls::dslx::StructType& struct_type = cpp_type->AsStruct();
  const xls::dslx::StructDef& cpp_struct_def = struct_type.nominal_type();
  const auto* struct_def =
      reinterpret_cast<const xls_dslx_struct_def*>(&cpp_struct_def);
  // const_cast is ok because the C API can only do immutable query-like things
  // with the node anyway.
  return const_cast<xls_dslx_struct_def*>(struct_def);
}

bool xls_dslx_type_to_string(const struct xls_dslx_type* type, char** error_out,
                             char** result_out) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  *error_out = nullptr;
  *result_out = xls::ToOwnedCString(cpp_type->ToString());
  return true;
}

bool xls_dslx_type_is_bits_like(struct xls_dslx_type* type,
                                struct xls_dslx_type_dim** is_signed,
                                struct xls_dslx_type_dim** size) {
  const auto* cpp_type = reinterpret_cast<const xls::dslx::Type*>(type);
  std::optional<xls::dslx::BitsLikeProperties> properties =
      GetBitsLike(*cpp_type);
  if (!properties.has_value()) {
    *is_signed = nullptr;
    *size = nullptr;
    return false;
  }

  *is_signed = reinterpret_cast<xls_dslx_type_dim*>(
      new xls::dslx::TypeDim(std::move(properties->is_signed)));
  *size = reinterpret_cast<xls_dslx_type_dim*>(
      new xls::dslx::TypeDim(std::move(properties->size)));
  return true;
}

// -- type_dim

bool xls_dslx_type_dim_is_parametric(struct xls_dslx_type_dim* td) {
  return false;
}

bool xls_dslx_type_dim_get_as_bool(struct xls_dslx_type_dim* td,
                                   char** error_out, bool* result_out) {
  auto* cpp_type_dim = reinterpret_cast<xls::dslx::TypeDim*>(td);
  absl::StatusOr<bool> value = cpp_type_dim->GetAsBool();
  if (!value.ok()) {
    *result_out = false;
    *error_out = xls::ToOwnedCString(value.status().ToString());
    return false;
  }

  *result_out = *value;
  *error_out = nullptr;
  return true;
}

bool xls_dslx_type_dim_get_as_int64(struct xls_dslx_type_dim* td,
                                    char** error_out, int64_t* result_out) {
  auto* cpp_type_dim = reinterpret_cast<xls::dslx::TypeDim*>(td);
  absl::StatusOr<int64_t> value = cpp_type_dim->GetAsInt64();
  if (!value.ok()) {
    *result_out = 0;
    *error_out = xls::ToOwnedCString(value.status().ToString());
    return false;
  }

  *result_out = *value;
  *error_out = nullptr;
  return true;
}

void xls_dslx_type_dim_free(struct xls_dslx_type_dim* td) {
  auto* cpp_type_dim = reinterpret_cast<xls::dslx::TypeDim*>(td);
  delete cpp_type_dim;
}

bool xls_dslx_replace_invocations_in_module(
    struct xls_dslx_typechecked_module* tm,
    struct xls_dslx_function* const callers[], size_t callers_count,
    const struct xls_dslx_invocation_rewrite_rule* rules, size_t rules_count,
    struct xls_dslx_import_data* import_data, const char* install_subject,
    char** error_out, struct xls_dslx_typechecked_module** result_out) {
  CHECK_NE(error_out, nullptr);
  CHECK_NE(result_out, nullptr);
  *error_out = nullptr;
  *result_out = nullptr;
  CHECK_NE(tm, nullptr);
  CHECK_NE(import_data, nullptr);
  CHECK_NE(install_subject, nullptr);
  CHECK(callers != nullptr || callers_count == 0);
  CHECK(rules != nullptr || rules_count == 0);

  auto* cpp_tm = reinterpret_cast<xls::dslx::TypecheckedModule*>(tm);
  auto& import_owner = UnwrapImportData(import_data);
  absl::MutexLock import_lock(&import_owner.mutex);
  auto* cpp_import_data = &import_owner.data;

  std::vector<const xls::dslx::Function*> callers_cpp;
  callers_cpp.reserve(callers_count);
  for (size_t i = 0; i < callers_count; ++i) {
    CHECK_NE(callers[i], nullptr);
    callers_cpp.push_back(
        reinterpret_cast<const xls::dslx::Function*>(callers[i]));
  }

  std::vector<xls::dslx::InvocationRewriteRule> rules_cpp;
  rules_cpp.reserve(rules_count);
  for (size_t i = 0; i < rules_count; ++i) {
    const xls_dslx_invocation_rewrite_rule& r = rules[i];
    CHECK_NE(r.from_callee, nullptr);
    CHECK_NE(r.to_callee, nullptr);
    xls::dslx::InvocationRewriteRule rr;
    rr.from_callee =
        reinterpret_cast<const xls::dslx::Function*>(r.from_callee);
    rr.to_callee = reinterpret_cast<const xls::dslx::Function*>(r.to_callee);
    if (r.match_callee_env != nullptr) {
      rr.match_callee_env = *xls::UnwrapDslxParametricEnv(r.match_callee_env);
    }
    if (r.to_callee_env != nullptr) {
      rr.to_callee_env = *xls::UnwrapDslxParametricEnv(r.to_callee_env);
    }
    rules_cpp.push_back(std::move(rr));
  }

  absl::StatusOr<xls::dslx::TypecheckedModule> new_tm =
      xls::dslx::ReplaceInvocationsInModule(
          *cpp_tm, absl::MakeSpan(callers_cpp), absl::MakeSpan(rules_cpp),
          *cpp_import_data, std::string_view{install_subject});
  if (!new_tm.ok()) {
    *error_out = xls::ToOwnedCString(new_tm.status().ToString());
    return false;
  }
  auto* heap_tm = new xls::dslx::TypecheckedModule{*std::move(new_tm)};
  *result_out = reinterpret_cast<xls_dslx_typechecked_module*>(heap_tm);
  return true;
}

}  // extern "C"
