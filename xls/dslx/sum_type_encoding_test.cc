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

#include "xls/dslx/sum_type_encoding.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {
namespace {

using ::testing::ElementsAre;

absl::StatusOr<int64_t> GetBitCount(const Type& type) {
  std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
  if (!bits_like.has_value()) {
    return absl::InvalidArgumentError("Expected bits-like type.");
  }
  return bits_like->size.GetAsInt64();
}

SumType MakeTuplePayloadSumType(Module& module) {
  const Span kFakeSpan = Span::Fake();

  auto* sum_name = module.Make<NameDef>(kFakeSpan, "Example", nullptr);
  auto* none_name = module.Make<NameDef>(kFakeSpan, "None", nullptr);
  auto* left_name = module.Make<NameDef>(kFakeSpan, "Left", nullptr);
  auto* pair_name = module.Make<NameDef>(kFakeSpan, "Pair", nullptr);

  auto* u8_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU8));
  auto* u16_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU16,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU16));
  auto* u32_type = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU32));

  auto* none =
      module.Make<SumVariant>(kFakeSpan, none_name,
                              SumVariant::PayloadKind::kUnit,
                              std::vector<TypeAnnotation*>{},
                              std::vector<StructMemberNode*>{});
  auto* left =
      module.Make<SumVariant>(kFakeSpan, left_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u8_type},
                              std::vector<StructMemberNode*>{});
  auto* pair =
      module.Make<SumVariant>(kFakeSpan, pair_name,
                              SumVariant::PayloadKind::kTuple,
                              std::vector<TypeAnnotation*>{u16_type, u32_type},
                              std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{none, left, pair}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  variants.emplace_back(*none, std::vector<std::unique_ptr<Type>>{});
  std::vector<std::unique_ptr<Type>> left_members;
  left_members.push_back(BitsType::MakeU8());
  variants.emplace_back(*left, std::move(left_members));
  std::vector<std::unique_ptr<Type>> pair_members;
  pair_members.push_back(std::make_unique<BitsType>(false, 16));
  pair_members.push_back(BitsType::MakeU32());
  variants.emplace_back(*pair, std::move(pair_members));
  return SumType(*sum_def, std::move(variants));
}

TEST(SumTypeEncodingTest, VisitsPayloadSlotsInDeclarationOrder) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeTuplePayloadSumType(module);
  SumTypeEncoding encoding(sum_type);

  std::vector<int64_t> slot_indexes;
  std::vector<int64_t> bit_counts;
  int64_t slot_index = 0;
  XLS_ASSERT_OK(encoding.ForEachPayloadType(
      [&](const Type& type) -> absl::Status {
        slot_indexes.push_back(slot_index++);
        XLS_ASSIGN_OR_RETURN(int64_t bit_count, GetBitCount(type));
        bit_counts.push_back(bit_count);
        return absl::OkStatus();
      }));

  EXPECT_THAT(slot_indexes, ElementsAre(0, 1, 2));
  EXPECT_THAT(bit_counts, ElementsAre(8, 16, 32));
}

TEST(SumTypeEncodingTest, TracksActiveSlotsForLaterVariants) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeTuplePayloadSumType(module);
  SumTypeEncoding encoding(sum_type);

  XLS_ASSERT_OK_AND_ASSIGN(SumTypeEncoding::VariantInfo pair_variant,
                           encoding.GetVariant("Pair"));
  EXPECT_EQ(pair_variant.variant_index, 2);
  EXPECT_EQ(pair_variant.payload_size(), 2);

  std::vector<int64_t> assembly_steps;
  XLS_ASSERT_OK(encoding.VisitPayloadAssemblyOrder(
      pair_variant,
      [&](int64_t active_index) -> absl::Status {
        assembly_steps.push_back(active_index);
        return absl::OkStatus();
      },
      [&](const Type& inactive_type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(int64_t bit_count, GetBitCount(inactive_type));
        assembly_steps.push_back(-bit_count);
        return absl::OkStatus();
      }));
  EXPECT_THAT(assembly_steps, ElementsAre(-8, 0, 1));

  std::vector<int64_t> active_slot_indexes;
  std::vector<int64_t> active_indexes;
  std::vector<int64_t> active_bit_counts;
  XLS_ASSERT_OK(encoding.ForEachActivePayloadSlot(
      pair_variant,
      [&](int64_t slot_index, int64_t active_index,
          const Type& type) -> absl::Status {
        active_slot_indexes.push_back(slot_index);
        active_indexes.push_back(active_index);
        XLS_ASSIGN_OR_RETURN(int64_t bit_count, GetBitCount(type));
        active_bit_counts.push_back(bit_count);
        return absl::OkStatus();
      }));
  EXPECT_THAT(active_slot_indexes, ElementsAre(1, 2));
  EXPECT_THAT(active_indexes, ElementsAre(0, 1));
  EXPECT_THAT(active_bit_counts, ElementsAre(16, 32));
}

TEST(SumTypeEncodingTest, VisitsStoredLeafTypesWithDenseTagFirst) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType sum_type = MakeTuplePayloadSumType(module);
  SumTypeEncoding encoding(sum_type);

  std::vector<int64_t> bit_counts;
  std::vector<std::optional<int64_t>> dense_max_values;
  XLS_ASSERT_OK(encoding.ForEachStoredLeafType(
      [&](const SumTypeEncoding::StoredLeafInfo& leaf) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(int64_t bit_count, GetBitCount(*leaf.type));
        bit_counts.push_back(bit_count);
        dense_max_values.push_back(leaf.dense_max_value);
        return absl::OkStatus();
      }));

  EXPECT_THAT(bit_counts, ElementsAre(2, 8, 16, 32));
  EXPECT_THAT(dense_max_values,
              ElementsAre(std::optional<int64_t>(2), std::nullopt,
                          std::nullopt, std::nullopt));
}

}  // namespace
}  // namespace xls::dslx
