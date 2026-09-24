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

#include "xls/dslx/type_system/zip_types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {
namespace {

using ::testing::ElementsAre;
using ::testing::FieldsAre;

enum class CallbackKind : uint8_t {
  kAggregateStart,
  kAggregateNext,
  kAggregateEnd,
  kMatchedLeaf,
  kMismatch,
};

struct CallbackData {
  CallbackKind kind;
  const Type* lhs = nullptr;
  const Type* lhs_parent = nullptr;
  const Type* rhs = nullptr;
  const Type* rhs_parent = nullptr;
  std::optional<AggregatePair> aggregates;
};

std::ostream& operator<<(std::ostream& os, CallbackKind kind) {
  std::string kind_str;
  switch (kind) {
    case CallbackKind::kAggregateStart:
      kind_str = "aggregate-start";
      break;
    case CallbackKind::kAggregateNext:
      kind_str = "aggregate-next";
      break;
    case CallbackKind::kAggregateEnd:
      kind_str = "aggregate-end";
      break;
    case CallbackKind::kMatchedLeaf:
      kind_str = "matched-leaf";
      break;
    case CallbackKind::kMismatch:
      kind_str = "mismatch";
      break;
  }
  os << kind_str;
  return os;
}

// Convenience for nicer matcher output.
std::ostream& operator<<(std::ostream& os, const CallbackData& data) {
  std::string lhs_str = "(null)";
  if (data.lhs != nullptr) {
    lhs_str = data.lhs->ToString();
  }
  std::string rhs_str = "(null)";
  if (data.rhs != nullptr) {
    rhs_str = data.rhs->ToString();
  }
  os << "{.kind=" << data.kind
     << absl::StreamFormat(", .lhs=%s, .rhs=%s}", lhs_str, rhs_str);
  return os;
}

// Trivial implementation of the callbacks abstract interface that just collects
// events as they are triggered in an underlying vector.
class ZipTypesCallbacksCollector : public ZipTypesCallbacks {
 public:
  ~ZipTypesCallbacksCollector() override = default;

  absl::Status NoteAggregateStart(const AggregatePair& pair) override {
    data_.push_back(CallbackData{.kind = CallbackKind::kAggregateStart,
                                 .aggregates = pair});
    return absl::OkStatus();
  }
  absl::Status NoteAggregateNext(const AggregatePair& pair) override {
    data_.push_back(
        CallbackData{.kind = CallbackKind::kAggregateNext, .aggregates = pair});
    return absl::OkStatus();
  }
  absl::Status NoteAggregateEnd(const AggregatePair& pair) override {
    data_.push_back(
        CallbackData{.kind = CallbackKind::kAggregateEnd, .aggregates = pair});
    return absl::OkStatus();
  }
  absl::Status NoteMatchedLeafType(const Type& lhs, const Type* lhs_parent,
                                   const Type& rhs,
                                   const Type* rhs_parent) override {
    data_.push_back(CallbackData{.kind = CallbackKind::kMatchedLeaf,
                                 .lhs = &lhs,
                                 .lhs_parent = lhs_parent,
                                 .rhs = &rhs,
                                 .rhs_parent = rhs_parent});
    return absl::OkStatus();
  }
  absl::Status NoteTypeMismatch(const Type& lhs, const Type* lhs_parent,
                                const Type& rhs,
                                const Type* rhs_parent) override {
    data_.push_back(CallbackData{.kind = CallbackKind::kMismatch,
                                 .lhs = &lhs,
                                 .lhs_parent = lhs_parent,
                                 .rhs = &rhs,
                                 .rhs_parent = rhs_parent});
    return absl::OkStatus();
  }

  absl::Span<const CallbackData> data() const { return data_; }

 private:
  std::vector<CallbackData> data_;
};

TEST(ZipTypesTest, DifferentBitsTypes) {
  auto lhs = BitsType::MakeU32();
  auto rhs = BitsType::MakeS32();

  ZipTypesCallbacksCollector collector;
  XLS_ASSERT_OK(ZipTypes(*lhs, *rhs, collector));

  EXPECT_EQ(collector.data().size(), 1);
  EXPECT_EQ(collector.data()[0].kind, CallbackKind::kMismatch);
  EXPECT_EQ(collector.data()[0].lhs, lhs.get());
  EXPECT_EQ(collector.data()[0].rhs, rhs.get());
}

// This is a special case for our type system (as represented in the C++
// objects, that is) -- bits types are type-compatible-with but structurally not
// identical to bits constructors in an array.
TEST(ZipTypesTest, BitsConstructorVsBitsType) {
  auto lhs = BitsType::MakeU32();
  auto rhs =
      std::make_unique<ArrayType>(std::make_unique<BitsConstructorType>(
                                      /*is_signed=*/TypeDim::CreateBool(false)),
                                  TypeDim::CreateU32(32));

  EXPECT_TRUE(lhs->CompatibleWith(*rhs));
  EXPECT_TRUE(rhs->CompatibleWith(*lhs));

  ZipTypesCallbacksCollector collector;
  XLS_ASSERT_OK(ZipTypes(*lhs, *rhs, collector));

  EXPECT_THAT(
      collector.data(),
      ElementsAre(FieldsAre(CallbackKind::kMatchedLeaf, lhs.get(), nullptr,
                            rhs.get(), nullptr, std::nullopt)));
}

// Verifies: Tuple traversal reports members and aggregate boundaries in order.
// Catches: Missing separators or incorrect parents around a mismatched member.
TEST(ZipTypesTest, TupleWithOneDifferingElement) {
  std::unique_ptr<TupleType> lhs =
      TupleType::Create2(BitsType::MakeU32(), BitsType::MakeU64());
  std::unique_ptr<TupleType> rhs =
      TupleType::Create2(BitsType::MakeU32(), BitsType::MakeS32());

  ZipTypesCallbacksCollector collector;
  XLS_ASSERT_OK(ZipTypes(*lhs, *rhs, collector));

  ASSERT_EQ(collector.data().size(), 5);

  std::pair<const TupleType*, const TupleType*> aggregates =
      std::make_pair(lhs.get(), rhs.get());
  EXPECT_THAT(collector.data()[0],
              FieldsAre(CallbackKind::kAggregateStart, nullptr, nullptr,
                        nullptr, nullptr, AggregatePair{aggregates}));
  EXPECT_THAT(
      collector.data()[1],
      FieldsAre(CallbackKind::kMatchedLeaf, &lhs->GetMemberType(0), lhs.get(),
                &rhs->GetMemberType(0), rhs.get(), std::nullopt));
  EXPECT_THAT(collector.data()[2],
              FieldsAre(CallbackKind::kAggregateNext, nullptr, nullptr, nullptr,
                        nullptr, AggregatePair{aggregates}));
  EXPECT_THAT(
      collector.data()[3],
      FieldsAre(CallbackKind::kMismatch, &lhs->GetMemberType(1), lhs.get(),
                &rhs->GetMemberType(1), rhs.get(), std::nullopt));
  EXPECT_THAT(collector.data()[4],
              FieldsAre(CallbackKind::kAggregateEnd, nullptr, nullptr, nullptr,
                        nullptr, AggregatePair{aggregates}));
}

// Verifies: A sum and its shared clone emit every payload callback in order.
// Catches: Shared-storage shortcuts that skip callbacks or conflate parents.
TEST(ZipTypesTest, SharedSumCloneTraversesEveryPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  const Span span = Span::Fake();
  auto* sum_name = module.Make<NameDef>(span, "Choice", nullptr);
  auto* u32 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU32,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU32));
  auto* u64 = module.Make<BuiltinTypeAnnotation>(
      span, BuiltinType::kU64,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU64));
  auto* pair = module.Make<SumVariant>(
      span, module.Make<NameDef>(span, "Pair", nullptr),
      SumVariant::PayloadShape::kTuple, std::vector<TypeAnnotation*>{u32, u64},
      std::vector<StructMemberNode*>{});
  auto* single = module.Make<SumVariant>(
      span, module.Make<NameDef>(span, "Single", nullptr),
      SumVariant::PayloadShape::kTuple, std::vector<TypeAnnotation*>{u32},
      std::vector<StructMemberNode*>{});
  auto* sum_def = module.Make<SumDef>(
      span, sum_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{pair, single}, /*is_public=*/false);
  sum_name->set_definer(sum_def);

  std::vector<SumTypeVariant> variants;
  std::vector<std::unique_ptr<Type>> pair_members;
  pair_members.push_back(BitsType::MakeU32());
  pair_members.push_back(BitsType::MakeU64());
  variants.push_back(SumTypeVariant::MakeTuple(*pair, std::move(pair_members)));
  std::vector<std::unique_ptr<Type>> single_members;
  single_members.push_back(BitsType::MakeU32());
  variants.push_back(
      SumTypeVariant::MakeTuple(*single, std::move(single_members)));
  SumType lhs(*sum_def, std::move(variants));
  std::unique_ptr<Type> clone = lhs.CloneToUnique();
  const auto* rhs = dynamic_cast<const SumType*>(clone.get());
  ASSERT_NE(rhs, nullptr);
  ASSERT_NE(&lhs, rhs);
  ASSERT_EQ(&lhs.variants(), &rhs->variants());

  ZipTypesCallbacksCollector collector;
  XLS_ASSERT_OK(ZipTypes(lhs, *rhs, collector));

  const AggregatePair aggregates = std::make_pair(&lhs, rhs);
  EXPECT_THAT(
      collector.data(),
      ElementsAre(
          FieldsAre(CallbackKind::kAggregateStart, nullptr, nullptr, nullptr,
                    nullptr, aggregates),
          FieldsAre(CallbackKind::kMatchedLeaf,
                    &lhs.variants()[0].GetMemberType(0), &lhs,
                    &rhs->variants()[0].GetMemberType(0), rhs, std::nullopt),
          FieldsAre(CallbackKind::kAggregateNext, nullptr, nullptr, nullptr,
                    nullptr, aggregates),
          FieldsAre(CallbackKind::kMatchedLeaf,
                    &lhs.variants()[0].GetMemberType(1), &lhs,
                    &rhs->variants()[0].GetMemberType(1), rhs, std::nullopt),
          FieldsAre(CallbackKind::kAggregateNext, nullptr, nullptr, nullptr,
                    nullptr, aggregates),
          FieldsAre(CallbackKind::kMatchedLeaf,
                    &lhs.variants()[1].GetMemberType(0), &lhs,
                    &rhs->variants()[1].GetMemberType(0), rhs, std::nullopt),
          FieldsAre(CallbackKind::kAggregateEnd, nullptr, nullptr, nullptr,
                    nullptr, aggregates)));
}

}  // namespace
}  // namespace xls::dslx
