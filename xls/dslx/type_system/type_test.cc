// Copyright 2020 The XLS Authors
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

#include "xls/dslx/type_system/type.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

const Pos kFakePos(Fileno(0), 0, 0);
const Span kFakeSpan(kFakePos, kFakePos);

// Creates a struct type of the following form in the module, and returns the
// `StructType` for it:
//
// ```dslx
// struct S {
//   x: u8,
//   y: u1,
// }
// ```
//
// Note that the `StructType` has to refer to a `StructDef` AST node which is
// why this helper is needed.
StructType CreateSimpleStruct(Module& module) {
  std::vector<StructMemberNode*> ast_members;
  ast_members.emplace_back(module.Make<StructMemberNode>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "x", nullptr), kFakeSpan,
      module.Make<BuiltinTypeAnnotation>(
          kFakeSpan, BuiltinType::kU8,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU8))));
  ast_members.emplace_back(module.Make<StructMemberNode>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "y", nullptr), kFakeSpan,
      module.Make<BuiltinTypeAnnotation>(
          kFakeSpan, BuiltinType::kU1,
          module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU1))));

  auto* struct_def = module.Make<StructDef>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "S", nullptr),
      std::vector<ParametricBinding*>{}, ast_members, /*is_public=*/false);
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  members.push_back(BitsType::MakeU1());
  return StructType(std::move(members), *struct_def);
}

// Retain the counter in payload clones so reconstructing a description cannot
// silently drop the repeated-work oracle.
class WidthCountingBitsType : public BitsType {
 public:
  WidthCountingBitsType(TypeDim size, int64_t& query_count)
      : BitsType(false, std::move(size)), query_count_(query_count) {}

  absl::StatusOr<TypeDim> GetTotalBitCount() const override {
    ++query_count_;
    return BitsType::GetTotalBitCount();
  }

  std::unique_ptr<Type> CloneToUnique() const override {
    return std::make_unique<WidthCountingBitsType>(size().Clone(),
                                                   query_count_);
  }

 private:
  int64_t& query_count_;
};

// Count actual predicate and rendering calls at a shared graph's leaves.
class DescriptionCountingBitsType : public BitsType {
 public:
  DescriptionCountingBitsType(int64_t& token_queries, int64_t& render_queries)
      : BitsType(false, 8),
        token_queries_(token_queries),
        render_queries_(render_queries) {}

  bool HasToken() const override {
    ++token_queries_;
    return false;
  }

  void AppendToStringInternal(FullyQualify fully_qualify,
                              const FileTable* file_table,
                              TypeStringContext& context,
                              std::string& output) const override {
    ++render_queries_;
    BitsType::AppendToStringInternal(fully_qualify, file_table, context,
                                     output);
  }

  std::unique_ptr<Type> CloneToUnique() const override {
    return std::make_unique<DescriptionCountingBitsType>(token_queries_,
                                                         render_queries_);
  }

 private:
  int64_t& token_queries_;
  int64_t& render_queries_;
};

// Count leaf comparisons through clones of independently built sum graphs.
class EqualityCountingBitsType : public BitsType {
 public:
  explicit EqualityCountingBitsType(int64_t& comparison_count)
      : BitsType(false, 1), comparison_count_(comparison_count) {}

  bool operator==(const Type& other) const override {
    ++comparison_count_;
    return BitsType::operator==(other);
  }

  std::unique_ptr<Type> CloneToUnique() const override {
    return std::make_unique<EqualityCountingBitsType>(comparison_count_);
  }

 private:
  int64_t& comparison_count_;
};

// Creates tuple constructors whose members are u8. Concrete payload Types are
// supplied separately by the tests, including deliberately invalid dimensions.
SumDef* CreateTupleSumDef(Module& module,
                          const std::vector<int64_t>& member_counts) {
  auto* u8 = module.Make<BuiltinTypeAnnotation>(
      kFakeSpan, BuiltinType::kU8,
      module.GetOrCreateBuiltinNameDef(BuiltinType::kU8));
  std::vector<SumVariant*> variants;
  for (int64_t i = 0; i < member_counts.size(); ++i) {
    variants.push_back(module.Make<SumVariant>(
        kFakeSpan,
        module.Make<NameDef>(kFakeSpan, "Case" + std::to_string(i), nullptr),
        SumVariant::PayloadShape::kTuple,
        std::vector<TypeAnnotation*>(member_counts[i], u8),
        std::vector<StructMemberNode*>{}));
  }
  auto* sum_def = module.Make<SumDef>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "S", nullptr),
      std::vector<ParametricBinding*>{}, std::move(variants),
      /*is_public=*/false);
  sum_def->name_def()->set_definer(sum_def);
  return sum_def;
}

TEST(TypeTest, TestU32) {
  BitsType t(false, 32);
  EXPECT_EQ("uN[32]", t.ToString());
  EXPECT_EQ("uN[32]", t.ToInlayHintString());
  EXPECT_EQ("ubits", t.GetDebugTypeName());
  EXPECT_EQ(false, t.is_signed());
  EXPECT_EQ(false, t.HasEnum());
  EXPECT_EQ(std::vector<TypeDim>{TypeDim::CreateU32(32)}, t.GetAllDims());
  EXPECT_EQ(t, *t.ToUBits());
  EXPECT_TRUE(IsBitsLikeWithNBitsAndSignedness(t, false, 32));
  EXPECT_FALSE(t.IsTuple());
}

TEST(TypeTest, TestUnit) {
  TupleType t({});
  EXPECT_EQ("()", t.ToString());
  EXPECT_EQ("()", t.ToInlayHintString());
  EXPECT_EQ("tuple", t.GetDebugTypeName());
  EXPECT_EQ(false, t.HasEnum());
  EXPECT_TRUE(t.GetAllDims().empty());
  EXPECT_TRUE(t.IsTuple());
  EXPECT_FALSE(IsBitsLikeWithNBitsAndSignedness(t, false, 0));

  Type* generic_type = &t;
  EXPECT_TRUE(generic_type->IsTuple());
  EXPECT_EQ(&generic_type->AsTuple(), &t);
}

TEST(TypeTest, TestMetaUnit) {
  MetaType meta_t(std::make_unique<BitsType>(false, 32));
  EXPECT_TRUE(meta_t.IsMeta());

  Type* generic_type = &meta_t;
  EXPECT_FALSE(generic_type->IsTuple());
  EXPECT_EQ(&generic_type->AsMeta(), &meta_t);
}

TEST(TypeTest, TestTwoTupleOfStruct) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  StructType s = CreateSimpleStruct(module);
  std::unique_ptr<TupleType> t2 =
      TupleType::Create2(s.CloneToUnique(), s.CloneToUnique());
  EXPECT_EQ("(S { x: uN[8], y: uN[1] }, S { x: uN[8], y: uN[1] })",
            t2->ToString());
  EXPECT_EQ("(S, S)", t2->ToInlayHintString());
  EXPECT_EQ("tuple", t2->GetDebugTypeName());
  EXPECT_EQ(false, t2->HasEnum());
}

TEST(TypeTest, TestArrayOfStruct) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  StructType s = CreateSimpleStruct(module);
  ArrayType a(s.CloneToUnique(), TypeDim::CreateU32(2));
  EXPECT_EQ("S { x: uN[8], y: uN[1] }[2]", a.ToString());
  EXPECT_EQ("S[2]", a.ToInlayHintString());
  EXPECT_EQ("array", a.GetDebugTypeName());
  EXPECT_EQ(false, a.HasEnum());
}

TEST(TypeTest, TestArrayOfU32) {
  ArrayType t(std::make_unique<BitsType>(false, 32), TypeDim::CreateU32(1));
  EXPECT_EQ("uN[32][1]", t.ToString());
  EXPECT_EQ("uN[32][1]", t.ToInlayHintString());
  EXPECT_EQ("array", t.GetDebugTypeName());
  EXPECT_EQ(false, t.HasEnum());
  std::vector<TypeDim> want_dims = {TypeDim::CreateU32(1),
                                    TypeDim::CreateU32(32)};
  EXPECT_EQ(want_dims, t.GetAllDims());
  EXPECT_FALSE(IsBitsLikeWithNBitsAndSignedness(t, false, 32));
}

TEST(TypeTest, TestEnum) {
  FileTable file_table;
  Module m("test", /*fs_path=*/std::nullopt, file_table);
  Pos fake_pos(Fileno(0), 0, 0);
  Span fake_span(fake_pos, fake_pos);
  auto* my_enum = m.Make<NameDef>(fake_span, "MyEnum", nullptr);
  auto* e = m.Make<EnumDef>(fake_span, my_enum, /*type=*/nullptr,
                            /*values=*/std::vector<EnumMember>{},
                            /*is_public=*/false);
  my_enum->set_definer(e);
  EnumType t(*e, /*bit_count=*/TypeDim::CreateU32(2),
             /*is_signed=*/false, {});
  EXPECT_TRUE(t.HasEnum());
  EXPECT_EQ(std::vector<TypeDim>{TypeDim::CreateU32(2)}, t.GetAllDims());
  EXPECT_EQ("MyEnum", t.ToString());
  EXPECT_EQ("MyEnum", t.ToInlayHintString());
  EXPECT_EQ("<no-file>:MyEnum", t.ToStringFullyQualified(file_table));

  ArrayType array(t.CloneToUnique(), TypeDim::CreateU32(2));
  EXPECT_EQ("MyEnum[2]", array.ToString());
  EXPECT_EQ("<no-file>:MyEnum[2]", array.ToStringFullyQualified(file_table));
}

TEST(TypeTest, FrozenDiscriminantsPreserveClonedIdentityAndFormatting) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  auto* name = module.Make<NameDef>(kFakeSpan, "E", nullptr);
  auto* a = module.Make<SumVariant>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "A", nullptr),
      SumVariant::PayloadShape::kUnit, std::vector<TypeAnnotation*>{},
      std::vector<StructMemberNode*>{});
  auto* b = module.Make<SumVariant>(
      kFakeSpan, module.Make<NameDef>(kFakeSpan, "B", nullptr),
      SumVariant::PayloadShape::kUnit, std::vector<TypeAnnotation*>{},
      std::vector<StructMemberNode*>{});
  auto* def =
      module.Make<SumDef>(kFakeSpan, name, std::vector<ParametricBinding*>{},
                          std::vector<SumVariant*>{a, b}, /*is_public=*/false);
  name->set_definer(def);
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeUnit(*a));
  variants.push_back(SumTypeVariant::MakeUnit(*b));
  SumType original(*def, std::move(variants), TypeDim::CreateU32(1),
                   {InterpValue::MakeUBits(1, 1), InterpValue::MakeUBits(1, 0)});
  std::unique_ptr<Type> clone = original.CloneToUnique();
  EXPECT_EQ(original, *clone);
  EXPECT_EQ(original.ToString(), "E { A | B }");
  EXPECT_EQ(clone->ToString(), original.ToString());
  const auto& cloned_sum = dynamic_cast<const SumType&>(*clone);
  EXPECT_EQ(cloned_sum.GetDiscriminant(0), original.GetDiscriminant(0));
  EXPECT_EQ(cloned_sum.GetDiscriminant(1), original.GetDiscriminant(1));
  EXPECT_EQ(&cloned_sum.variants(), &original.variants());
}

TEST(TypeTest, FunctionTypeU32ToS32) {
  std::vector<std::unique_ptr<Type>> params;
  params.push_back(std::make_unique<BitsType>(false, 32));
  FunctionType t(std::move(params), std::make_unique<BitsType>(true, 32));
  EXPECT_EQ(1, t.GetParams().size());
  EXPECT_EQ("uN[32]", t.GetParams()[0]->ToString());
  EXPECT_EQ("sN[32]", t.return_type().ToString());
}

TEST(TypeTest, FromInterpValueSbits) {
  auto s8_m1 = InterpValue::MakeSBits(8, -1);
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Type> ct,
                           Type::FromInterpValue(s8_m1));
  EXPECT_EQ(ct->ToString(), "sN[8]");
  EXPECT_EQ(ct->ToInlayHintString(), "sN[8]");
}

TEST(TypeTest, FromInterpValueArrayU2) {
  auto v = InterpValue::MakeArray({
                                      InterpValue::MakeUBits(2, 0b10),
                                      InterpValue::MakeUBits(2, 0b01),
                                      InterpValue::MakeUBits(2, 0b11),
                                  })
               .value();
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Type> ct, Type::FromInterpValue(v));
  EXPECT_EQ(ct->ToString(), "uN[2][3]");
  EXPECT_THAT(ct->GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(6)));
}

TEST(TypeTest, FromInterpValueTupleEmpty) {
  auto v = InterpValue::MakeTuple({});
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Type> ct, Type::FromInterpValue(v));
  EXPECT_EQ(ct->ToString(), "()");
  EXPECT_TRUE(ct->IsUnit());
  EXPECT_THAT(ct->GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(0)));
}

TEST(TypeTest, FromInterpValueTupleOfTwoNumbers) {
  auto v = InterpValue::MakeTuple({
      InterpValue::MakeUBits(2, 0b10),
      InterpValue::MakeSBits(3, -1),
  });
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Type> ct, Type::FromInterpValue(v));
  EXPECT_EQ(ct->ToString(), "(uN[2], sN[3])");
  EXPECT_EQ(ct->ToInlayHintString(), "(uN[2], sN[3])");
  EXPECT_FALSE(ct->IsUnit());
  EXPECT_THAT(ct->GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(5)));
}

TEST(TypeTest, StructTypeGetTotalBitCount) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  StructType s = CreateSimpleStruct(module);
  EXPECT_THAT(s.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(9)));
  EXPECT_THAT(s.GetAllDims(),
              ElementsAre(TypeDim::CreateU32(8), TypeDim::CreateU32(1)));
  EXPECT_FALSE(s.HasEnum());
}

TEST(TypeTest, EmptyStructTypeIsNotUnit) {
  FileTable file_table;
  std::filesystem::path fs_path = "relpath/to/test.x";
  Fileno fileno = file_table.GetOrCreate(fs_path.c_str());
  Module module("test", fs_path, file_table);
  Span fake_span(Pos(fileno, 0, 0), Pos(fileno, 0, 0));
  auto* struct_def = module.Make<StructDef>(
      fake_span, module.Make<NameDef>(fake_span, "S", nullptr),
      std::vector<ParametricBinding*>{}, std::vector<StructMemberNode*>{},
      /*is_public=*/false);
  std::vector<std::unique_ptr<Type>> members;
  StructType s(std::move(members), *struct_def);
  EXPECT_THAT(s.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(0)));
  EXPECT_TRUE(s.GetAllDims().empty());
  EXPECT_FALSE(s.HasEnum());
  EXPECT_FALSE(s.IsUnit());
  EXPECT_EQ(s.ToString(), "S {}");
  EXPECT_EQ(s.ToInlayHintString(), "S");
  EXPECT_EQ(s.ToStringFullyQualified(file_table), "relpath/to/test.x:S {}");
}

// Verifies one eager payload walk per immutable description, not per query or
// cloned wrapper. The widest constructor is last and has two payload members.
TEST(TypeTest, SumWidthQueriesReuseSharedDescription) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {0, 1, 2});
  int64_t query_count = 0;
  std::vector<SumTypeVariant> variants;
  for (const SumVariant* variant : sum_def->variants()) {
    std::vector<std::unique_ptr<Type>> members;
    for (int64_t i = 0; i < variant->payload_member_count(); ++i) {
      members.push_back(std::make_unique<WidthCountingBitsType>(
          TypeDim::CreateU32(8), query_count));
    }
    variants.push_back(SumTypeVariant::MakeTuple(*variant, std::move(members)));
  }
  SumType sum(*sum_def, std::move(variants));
  EXPECT_EQ(query_count, 3);
  std::unique_ptr<Type> clone = sum.CloneToUnique();
  for (int64_t i = 0; i < 3; ++i) {
    EXPECT_THAT(sum.GetMaxPayloadBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(16)));
    EXPECT_THAT(sum.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(18)));
    EXPECT_THAT(clone->AsSum().GetMaxPayloadBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(16)));
    EXPECT_THAT(clone->GetTotalBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(18)));
  }
  EXPECT_EQ(query_count, 3);

  std::vector<SumTypeVariant> reconstructed_variants;
  for (const SumTypeVariant& variant : sum.variants()) {
    reconstructed_variants.push_back(variant.Clone());
  }
  SumType reconstructed(*sum_def, std::move(reconstructed_variants));
  EXPECT_EQ(query_count, 6);
  EXPECT_THAT(reconstructed.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(16)));
  EXPECT_THAT(reconstructed.GetTotalBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(18)));
  EXPECT_EQ(query_count, 6);
}

// The retained graph grows by one description per level, not one per branch.
// Counts catch repeated traversal without relying on machine-dependent timing.
TEST(TypeTest, SharedSumTokenQueriesAndPrintingStayBounded) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {1, 1});
  for (int64_t depth : {4, 8}) {
    int64_t token_queries = 0;
    int64_t render_queries = 0;
    std::unique_ptr<Type> type = std::make_unique<DescriptionCountingBitsType>(
        token_queries, render_queries);
    for (int64_t level = 0; level < depth; ++level) {
      std::vector<SumTypeVariant> variants;
      for (const SumVariant* variant : sum_def->variants()) {
        std::vector<std::unique_ptr<Type>> members;
        members.push_back(type->CloneToUnique());
        variants.push_back(
            SumTypeVariant::MakeTuple(*variant, std::move(members)));
      }
      type = std::make_unique<SumType>(*sum_def, std::move(variants));
    }
    EXPECT_EQ(token_queries, 2);
    std::unique_ptr<Type> clone = type->CloneToUnique();
    EXPECT_EQ(&type->AsSum().variants(), &clone->AsSum().variants());
    ArrayType array(clone->CloneToUnique(), TypeDim::CreateU32(2));
    for (int64_t i = 0; i < 10; ++i) {
      EXPECT_FALSE(type->HasToken());
      EXPECT_FALSE(clone->HasToken());
      EXPECT_FALSE(array.HasToken());
    }
    EXPECT_EQ(token_queries, 2);
    const std::string rendered = type->ToString();
    EXPECT_EQ(render_queries, 2);
    EXPECT_LT(rendered.size(), 70 * depth);
    EXPECT_THAT(rendered, HasSubstr("@1="));
    EXPECT_EQ(clone->ToString(), rendered);
    EXPECT_EQ(render_queries, 4);
    EXPECT_EQ(array.ToString(), rendered + "[2]");
    EXPECT_EQ(render_queries, 6);
    EXPECT_THAT(type->GetTotalBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(8 + depth)));
  }
}

TEST(TypeTest, SharedSumPayloadWidthUsesMaximumRecursively) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  auto make_choice = [&](std::unique_ptr<Type> left,
                         std::unique_ptr<Type> right, int64_t tag_bits) {
    SumDef* def = CreateTupleSumDef(module, {1, 1});
    std::vector<SumTypeVariant> variants;
    std::vector<std::unique_ptr<Type>> first;
    first.push_back(std::move(left));
    variants.push_back(
        SumTypeVariant::MakeTuple(*def->variants()[0], std::move(first)));
    std::vector<std::unique_ptr<Type>> second;
    second.push_back(std::move(right));
    variants.push_back(
        SumTypeVariant::MakeTuple(*def->variants()[1], std::move(second)));
    return SumType(*def, std::move(variants), TypeDim::CreateU32(tag_bits));
  };
  SumType different =
      make_choice(BitsType::MakeU8(), std::make_unique<BitsType>(false, 16), 1);
  EXPECT_THAT(different.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(16)));
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(different),
              IsOkAndHolds(TypeDim::CreateU32(17)));

  SumType inner = make_choice(BitsType::MakeU8(), BitsType::MakeU8(), 1);
  SumType outer =
      make_choice(TupleType::Create2(inner.CloneToUnique(), BitsType::MakeU8()),
                  BitsType::MakeU8(), 3);
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(inner),
              IsOkAndHolds(TypeDim::CreateU32(9)));
  EXPECT_THAT(outer.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(17)));
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(outer),
              IsOkAndHolds(TypeDim::CreateU32(20)));

  ArrayType array(inner.CloneToUnique(), TypeDim::CreateU32(2));
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(array),
              IsOkAndHolds(TypeDim::CreateU32(18)));
  StructType shape = CreateSimpleStruct(module);
  std::vector<std::unique_ptr<Type>> fields;
  fields.push_back(array.CloneToUnique());
  fields.push_back(BitsType::MakeU1());
  StructType structure(std::move(fields), shape.nominal_type());
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(structure),
              IsOkAndHolds(TypeDim::CreateU32(19)));
  ArrayType bits(
      std::make_unique<BitsConstructorType>(TypeDim::CreateBool(true)),
      TypeDim::CreateU32(5));
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(bits),
              IsOkAndHolds(TypeDim::CreateU32(5)));

  SumDef* empty_def = CreateTupleSumDef(module, {});
  SumType empty(*empty_def, {});
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(empty),
              IsOkAndHolds(TypeDim::CreateU32(0)));
  ArrayType empty_array(inner.CloneToUnique(), TypeDim::CreateU32(0));
  EXPECT_THAT(internal::GetBitCountWithSharedSumPayload(empty_array),
              IsOkAndHolds(TypeDim::CreateU32(0)));
}

TEST(TypeTest, SharedSumPayloadWidthReusesDescriptionsAndPreservesErrors) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* def = CreateTupleSumDef(module, {1, 1});
  auto wrap = [&](const Type& type) {
    std::vector<SumTypeVariant> variants;
    for (const SumVariant* variant : def->variants()) {
      std::vector<std::unique_ptr<Type>> members;
      members.push_back(type.CloneToUnique());
      variants.push_back(
          SumTypeVariant::MakeTuple(*variant, std::move(members)));
    }
    return std::make_unique<SumType>(*def, std::move(variants));
  };

  int64_t width_queries = 0;
  std::unique_ptr<Type> current = std::make_unique<WidthCountingBitsType>(
      TypeDim::CreateU32(1), width_queries);
  for (int64_t depth = 0; depth < 12; ++depth) {
    current = wrap(*current);
  }
  EXPECT_EQ(width_queries, 2);
  for (int64_t i = 0; i < 2; ++i) {
    EXPECT_THAT(
        internal::GetBitCountWithSharedSumPayload(*current->CloneToUnique()),
        IsOkAndHolds(TypeDim::CreateU32(13)));
  }
  EXPECT_EQ(width_queries, 2);

  ArrayType invalid(BitsType::MakeU1(), TypeDim(InterpValue::MakeTuple({})));
  absl::Status expected = invalid.GetTotalBitCount().status();
  ASSERT_FALSE(expected.ok());
  std::unique_ptr<SumType> invalid_sum = wrap(invalid);
  EXPECT_EQ(invalid_sum->GetMaxPayloadBitCount().status(), expected);
  EXPECT_EQ(
      internal::GetBitCountWithSharedSumPayload(*wrap(*invalid_sum)).status(),
      expected);
}

// N0 has no values and both alternatives of Ni contain the same Ni-1
// description. Root still has the two values of its live u1 alternative.
TEST(TypeTest, SharedEmptySumGraphInhabitanceFindsLiveAlternative) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* empty_def = CreateTupleSumDef(module, {});
  SumDef* binary_def = CreateTupleSumDef(module, {1, 1});
  for (int64_t depth : {4, 12}) {
    SCOPED_TRACE(depth);
    std::unique_ptr<Type> empty =
        std::make_unique<SumType>(*empty_def, std::vector<SumTypeVariant>{});
    for (int64_t level = 0; level < depth; ++level) {
      std::vector<SumTypeVariant> variants;
      for (const SumVariant* variant : binary_def->variants()) {
        std::vector<std::unique_ptr<Type>> members;
        members.push_back(empty->CloneToUnique());
        variants.push_back(
            SumTypeVariant::MakeTuple(*variant, std::move(members)));
      }
      empty = std::make_unique<SumType>(*binary_def, std::move(variants));
    }
    EXPECT_THAT(TypeIsInhabited(*empty), IsOkAndHolds(false));
    EXPECT_THAT(TypeIsInhabited(*empty->CloneToUnique()), IsOkAndHolds(false));

    std::vector<SumTypeVariant> variants;
    std::vector<std::unique_ptr<Type>> dead_members;
    dead_members.push_back(empty->CloneToUnique());
    variants.push_back(SumTypeVariant::MakeTuple(*binary_def->variants()[0],
                                                 std::move(dead_members)));
    std::vector<std::unique_ptr<Type>> live_members;
    live_members.push_back(BitsType::MakeU1());
    variants.push_back(SumTypeVariant::MakeTuple(*binary_def->variants()[1],
                                                 std::move(live_members)));
    SumType root(*binary_def, std::move(variants));
    EXPECT_THAT(TypeIsInhabited(root), IsOkAndHolds(true));
    EXPECT_THAT(SumVariantIsInhabited(root.variants()[0]), IsOkAndHolds(false));
    EXPECT_THAT(SumVariantIsInhabited(root.variants()[1]), IsOkAndHolds(true));
  }
}

// Wrap<T> reaches T through both its type argument and its payload.
// Independently built graphs must not revisit the same pair exponentially,
// including when an ordinary aggregate lies between the sum and its child.
TEST(TypeTest, SumEqualityReusesSharedPairsThroughArgumentsAndPayloads) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* wrapper_def = CreateTupleSumDef(module, {1});
  int64_t comparison_count = 0;
  auto make_chain = [&](int64_t depth) {
    std::unique_ptr<Type> current =
        std::make_unique<EqualityCountingBitsType>(comparison_count);
    for (int64_t i = 0; i < depth; ++i) {
      std::vector<SumType::ParametricArgument> arguments;
      arguments.emplace_back(current->CloneToUnique());
      std::vector<std::unique_ptr<Type>> members;
      members.push_back(std::make_unique<ArrayType>(
          TupleType::Create2(current->CloneToUnique(), BitsType::MakeU1()),
          TypeDim::CreateU32(1)));
      std::vector<SumTypeVariant> variants;
      variants.push_back(SumTypeVariant::MakeTuple(*wrapper_def->variants()[0],
                                                   std::move(members)));
      current = std::make_unique<SumType>(
          *wrapper_def, std::move(variants), std::nullopt,
          std::vector<InterpValue>{}, std::move(arguments));
    }
    return current;
  };
  for (int64_t depth : {4, 12}) {
    SCOPED_TRACE(depth);
    auto lhs = make_chain(depth);
    auto rhs = make_chain(depth);
    for (int64_t repetition = 0; repetition < 2; ++repetition) {
      comparison_count = 0;
      EXPECT_EQ(*lhs, *rhs);
      EXPECT_GT(comparison_count, 0);
      EXPECT_LE(comparison_count, 2);
    }
    comparison_count = 0;
    EXPECT_EQ(*lhs, *lhs->CloneToUnique());
    EXPECT_EQ(comparison_count, 0);
  }
}

// Reusing an equal pair must not conflate a later comparison of the same left
// description with a different right description, even for the same SumDef.
TEST(TypeTest, SumEqualityDistinguishesBothDescriptionsInARepeatedPair) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* wrapper_def = CreateTupleSumDef(module, {1});
  auto wrap = [&](std::unique_ptr<Type> payload) {
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(std::move(payload));
    std::vector<SumTypeVariant> variants;
    variants.push_back(SumTypeVariant::MakeTuple(*wrapper_def->variants()[0],
                                                 std::move(members)));
    return std::make_unique<SumType>(*wrapper_def, std::move(variants));
  };
  auto lhs_child = wrap(BitsType::MakeU1());
  auto rhs_child = wrap(BitsType::MakeU1());
  auto different_child = wrap(BitsType::MakeU8());
  auto lhs = wrap(TupleType::Create2(lhs_child->CloneToUnique(),
                                     lhs_child->CloneToUnique()));
  auto rhs = wrap(TupleType::Create2(rhs_child->CloneToUnique(),
                                     different_child->CloneToUnique()));
  EXPECT_NE(*lhs, *rhs);
  EXPECT_NE(*rhs, *lhs);
  EXPECT_EQ(*lhs_child, *rhs_child);
  EXPECT_NE(*lhs_child, *different_child);
}

TEST(TypeTest, SumEqualityPreservesNominalPhantomAndDiscriminantIdentity) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  Module other_module("other", std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {0});
  SumDef* other_def = CreateTupleSumDef(other_module, {0});
  auto make_sum = [](const SumDef& def, uint32_t value_argument,
                     int64_t type_argument_width, uint32_t discriminant) {
    std::vector<SumTypeVariant> variants;
    variants.push_back(SumTypeVariant::MakeTuple(*def.variants()[0], {}));
    std::vector<SumType::ParametricArgument> arguments;
    arguments.emplace_back(InterpValue::MakeU32(value_argument));
    arguments.emplace_back(
        std::make_unique<BitsType>(false, type_argument_width));
    return std::make_unique<SumType>(
        def, std::move(variants), TypeDim::CreateU32(1),
        std::vector<InterpValue>{InterpValue::MakeUBits(1, discriminant)},
        std::move(arguments));
  };
  auto sum = make_sum(*sum_def, 7, 8, 0);
  EXPECT_EQ(*sum, *make_sum(*sum_def, 7, 8, 0));
  EXPECT_NE(*sum, *make_sum(*other_def, 7, 8, 0));
  EXPECT_NE(*sum, *make_sum(*sum_def, 9, 8, 0));
  EXPECT_NE(*sum, *make_sum(*sum_def, 7, 16, 0));
  EXPECT_NE(*sum, *make_sum(*sum_def, 7, 8, 1));
  EXPECT_NE(*sum, *BitsType::MakeU1());
}

// Two concrete instances of the same nominal sum can have different payload
// domains. Reusing an answer must follow the description, not the declaration.
TEST(TypeTest, SumInhabitanceDistinguishesConcreteDescriptions) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* empty_def = CreateTupleSumDef(module, {});
  SumDef* wrapper_def = CreateTupleSumDef(module, {1});
  auto wrap = [&](std::unique_ptr<Type> payload) {
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(std::move(payload));
    std::vector<SumTypeVariant> variants;
    variants.push_back(SumTypeVariant::MakeTuple(*wrapper_def->variants()[0],
                                                 std::move(members)));
    return std::make_unique<SumType>(*wrapper_def, std::move(variants));
  };
  auto live = wrap(BitsType::MakeU1());
  auto dead = wrap(
      std::make_unique<SumType>(*empty_def, std::vector<SumTypeVariant>{}));
  auto live_then_dead =
      TupleType::Create2(live->CloneToUnique(), dead->CloneToUnique());
  EXPECT_THAT(TypeIsInhabited(*live_then_dead), IsOkAndHolds(false));

  SumDef* choice_def = CreateTupleSumDef(module, {1, 1});
  std::vector<SumTypeVariant> variants;
  std::vector<std::unique_ptr<Type>> dead_members;
  dead_members.push_back(dead->CloneToUnique());
  variants.push_back(SumTypeVariant::MakeTuple(*choice_def->variants()[0],
                                               std::move(dead_members)));
  std::vector<std::unique_ptr<Type>> live_members;
  live_members.push_back(live->CloneToUnique());
  variants.push_back(SumTypeVariant::MakeTuple(*choice_def->variants()[1],
                                               std::move(live_members)));
  SumType choice(*choice_def, std::move(variants));
  EXPECT_THAT(TypeIsInhabited(choice), IsOkAndHolds(true));
}

TEST(TypeTest, SumInhabitancePreservesZeroLengthArrays) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* empty_def = CreateTupleSumDef(module, {});
  SumType empty(*empty_def, {});
  SumDef* choice_def = CreateTupleSumDef(module, {1, 1});
  std::vector<SumTypeVariant> variants;
  for (int64_t size : {0, 1}) {
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(TupleType::Create2(
        BitsType::MakeU1(),
        std::make_unique<ArrayType>(empty.CloneToUnique(),
                                    TypeDim::CreateU32(size))));
    variants.push_back(SumTypeVariant::MakeTuple(*choice_def->variants()[size],
                                                 std::move(members)));
  }
  SumType choice(*choice_def, std::move(variants));
  EXPECT_THAT(TypeIsInhabited(choice), IsOkAndHolds(true));
  EXPECT_THAT(SumVariantIsInhabited(choice.variants()[0]), IsOkAndHolds(true));
  EXPECT_THAT(SumVariantIsInhabited(choice.variants()[1]), IsOkAndHolds(false));
}

TEST(TypeTest, SumInhabitancePreservesShortCircuitAndErrors) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* empty_def = CreateTupleSumDef(module, {});
  SumType empty(*empty_def, {});
  ArrayType invalid_array(BitsType::MakeU1(),
                          TypeDim(InterpValue::MakeTuple({})));
  const auto invalid_size =
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Cannot convert non-bits type to int64_t"));
  EXPECT_THAT(TypeIsInhabited(invalid_array), invalid_size);
  ArrayType zero_elements(invalid_array.CloneToUnique(), TypeDim::CreateU32(0));
  EXPECT_THAT(TypeIsInhabited(zero_elements), IsOkAndHolds(true));
  auto dead_tuple =
      TupleType::Create2(empty.CloneToUnique(), invalid_array.CloneToUnique());
  EXPECT_THAT(TypeIsInhabited(*dead_tuple), IsOkAndHolds(false));
  auto invalid_tuple =
      TupleType::Create2(invalid_array.CloneToUnique(), empty.CloneToUnique());
  EXPECT_THAT(TypeIsInhabited(*invalid_tuple), invalid_size);

  SumDef* choice_def = CreateTupleSumDef(module, {2, 1});
  auto make_choice = [&](std::unique_ptr<Type> first,
                         std::unique_ptr<Type> second,
                         std::unique_ptr<Type> alternative) {
    std::vector<SumTypeVariant> variants;
    std::vector<std::unique_ptr<Type>> first_members;
    first_members.push_back(std::move(first));
    first_members.push_back(std::move(second));
    variants.push_back(SumTypeVariant::MakeTuple(*choice_def->variants()[0],
                                                 std::move(first_members)));
    std::vector<std::unique_ptr<Type>> second_members;
    second_members.push_back(std::move(alternative));
    variants.push_back(SumTypeVariant::MakeTuple(*choice_def->variants()[1],
                                                 std::move(second_members)));
    return SumType(*choice_def, std::move(variants));
  };
  SumType live_first = make_choice(BitsType::MakeU1(), BitsType::MakeU1(),
                                   invalid_tuple->CloneToUnique());
  EXPECT_THAT(TypeIsInhabited(live_first), IsOkAndHolds(true));
  EXPECT_THAT(SumVariantIsInhabited(live_first.variants()[1]), invalid_size);
  SumType dead_first = make_choice(
      empty.CloneToUnique(), invalid_array.CloneToUnique(), BitsType::MakeU1());
  EXPECT_THAT(SumVariantIsInhabited(dead_first.variants()[0]),
              IsOkAndHolds(false));
  EXPECT_THAT(TypeIsInhabited(dead_first), IsOkAndHolds(true));
  SumType invalid_first = make_choice(
      invalid_array.CloneToUnique(), empty.CloneToUnique(), BitsType::MakeU1());
  EXPECT_THAT(TypeIsInhabited(invalid_first), invalid_size);
}

TEST(TypeTest, SumTokenSummaryPreservesPayloadAndPhantomDistinction) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* payload_def = CreateTupleSumDef(module, {1});
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(std::make_unique<ArrayType>(
      TupleType::Create2(BitsType::MakeU8(), std::make_unique<TokenType>()),
      TypeDim::CreateU32(1)));
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeTuple(*payload_def->variants()[0],
                                               std::move(members)));
  SumType with_token(*payload_def, std::move(variants));
  EXPECT_TRUE(with_token.HasToken());
  EXPECT_TRUE(with_token.CloneToUnique()->HasToken());

  SumDef* phantom_def = CreateTupleSumDef(module, {0});
  std::vector<SumTypeVariant> phantom_variants;
  phantom_variants.push_back(
      SumTypeVariant::MakeTuple(*phantom_def->variants()[0], {}));
  std::vector<SumType::ParametricArgument> arguments;
  arguments.emplace_back(with_token.CloneToUnique());
  SumType phantom(*phantom_def, std::move(phantom_variants), std::nullopt, {},
                  std::move(arguments));
  EXPECT_FALSE(phantom.HasToken());
  EXPECT_FALSE(phantom.CloneToUnique()->HasToken());
}

TEST(TypeTest, SumTextReferencesRespectSharingAndNominalArguments) {
  FileTable file_table;
  Module module("test", std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {0});
  auto make_sum = [&](uint32_t phantom) {
    std::vector<SumTypeVariant> variants;
    variants.push_back(SumTypeVariant::MakeTuple(*sum_def->variants()[0], {}));
    std::vector<SumType::ParametricArgument> arguments;
    arguments.emplace_back(InterpValue::MakeU32(phantom));
    return std::make_unique<SumType>(*sum_def, std::move(variants),
                                     std::nullopt, std::vector<InterpValue>{},
                                     std::move(arguments));
  };
  std::unique_ptr<SumType> first = make_sum(1);
  EXPECT_EQ(first->ToString(), "S<u32:1> { Case0() }");
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(first->CloneToUnique());
  members.push_back(make_sum(2));
  members.push_back(first->CloneToUnique());
  TupleType shared(std::move(members));
  EXPECT_EQ(shared.ToString(),
            "(@1=S<u32:1> { Case0() }, S<u32:2> { Case0() }, @1)");
  auto independently_owned = TupleType::Create2(make_sum(1), make_sum(1));
  EXPECT_EQ(independently_owned->ToString(),
            "(S<u32:1> { Case0() }, S<u32:1> { Case0() })");
  std::vector<std::unique_ptr<Type>> params;
  params.push_back(
      TupleType::Create2(first->CloneToUnique(), first->CloneToUnique()));
  FunctionType function(std::move(params), first->CloneToUnique());
  EXPECT_EQ(function.ToStringFullyQualified(file_table),
            "((@1=S<u32:1> { Case0() }, @1)) -> "
            "<no-file>:S<u32:1> { Case0() }");
}

// A failed payload-width calculation must remain a query result. Construction
// and cloning are still permitted, and repeated errors must not retry the walk.
TEST(TypeTest, SumWidthErrorIsStoredWithoutRejectingConstruction) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {1});
  TypeDim invalid_width(InterpValue::MakeUBits(64, 8));
  absl::Status expected_error =
      TypeDim::CreateU32(0).Add(invalid_width).status();
  ASSERT_FALSE(expected_error.ok());
  int64_t query_count = 0;
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(std::make_unique<WidthCountingBitsType>(
      invalid_width.Clone(), query_count));
  std::vector<SumTypeVariant> variants;
  variants.push_back(
      SumTypeVariant::MakeTuple(*sum_def->variants()[0], std::move(members)));
  SumType sum(*sum_def, std::move(variants));
  EXPECT_EQ(query_count, 1);
  std::unique_ptr<Type> clone = sum.CloneToUnique();
  for (int64_t i = 0; i < 3; ++i) {
    EXPECT_THAT(sum.GetMaxPayloadBitCount(),
                StatusIs(expected_error.code(), expected_error.message()));
    EXPECT_THAT(sum.GetTotalBitCount(),
                StatusIs(expected_error.code(), expected_error.message()));
    EXPECT_THAT(clone->AsSum().GetMaxPayloadBitCount(),
                StatusIs(expected_error.code(), expected_error.message()));
    EXPECT_THAT(clone->GetTotalBitCount(),
                StatusIs(expected_error.code(), expected_error.message()));
  }
  EXPECT_EQ(query_count, 1);
}

// A successful payload width must not hide a later error adding the tag width.
TEST(TypeTest, SumTotalWidthPreservesTagAdditionError) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {1});
  std::vector<std::unique_ptr<Type>> members;
  members.push_back(BitsType::MakeU8());
  std::vector<SumTypeVariant> variants;
  variants.push_back(
      SumTypeVariant::MakeTuple(*sum_def->variants()[0], std::move(members)));
  TypeDim tag_width(InterpValue::MakeUBits(64, 2));
  absl::Status expected_error = tag_width.Add(TypeDim::CreateU32(8)).status();
  ASSERT_FALSE(expected_error.ok());
  SumType sum(*sum_def, std::move(variants), tag_width);
  EXPECT_THAT(sum.GetMaxPayloadBitCount(), IsOkAndHolds(TypeDim::CreateU32(8)));
  EXPECT_THAT(sum.GetTotalBitCount(),
              StatusIs(expected_error.code(), expected_error.message()));
  EXPECT_THAT(sum.CloneToUnique()->GetTotalBitCount(),
              StatusIs(expected_error.code(), expected_error.message()));
}

TEST(TypeTest, SumZeroWidthPayloadPreservesImplicitAndExplicitTags) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {0});
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeTuple(*sum_def->variants()[0], {}));
  SumType implicit(*sum_def, std::move(variants));
  EXPECT_THAT(implicit.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(0)));
  EXPECT_THAT(implicit.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(0)));

  std::vector<SumTypeVariant> tagged_variants;
  tagged_variants.push_back(implicit.variants()[0].Clone());
  SumType tagged(*sum_def, std::move(tagged_variants), TypeDim::CreateU32(3));
  EXPECT_NE(implicit, tagged);
  EXPECT_THAT(tagged.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(0)));
  EXPECT_THAT(tagged.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(3)));
}

// Payload accumulation and tag addition use wrapping u32 dimensions. The
// maximum must retain its unsigned value even when another payload wraps.
TEST(TypeTest, SumWidthPreservesUnsigned32BitArithmetic) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumDef* sum_def = CreateTupleSumDef(module, {1, 2});
  std::vector<SumTypeVariant> variants;
  for (const SumVariant* variant : sum_def->variants()) {
    std::vector<std::unique_ptr<Type>> members;
    for (int64_t i = 0; i < variant->payload_member_count(); ++i) {
      members.push_back(std::make_unique<BitsType>(
          false, TypeDim::CreateU32(i == 0 ? 0xffffffff : 1)));
    }
    variants.push_back(SumTypeVariant::MakeTuple(*variant, std::move(members)));
  }
  SumType sum(*sum_def, std::move(variants));
  EXPECT_THAT(sum.GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(0xffffffff)));
  EXPECT_THAT(sum.GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(0)));
  EXPECT_THAT(sum.CloneToUnique()->AsSum().GetMaxPayloadBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(0xffffffff)));
}

// -- TypeDimTest

TEST(TypeDimTest, TestArithmetic) {
  auto two = TypeDim::CreateU32(2);
  auto three = TypeDim::CreateU32(3);
  auto five = TypeDim::CreateU32(5);
  auto six = TypeDim::CreateU32(6);
  EXPECT_THAT(two.Add(three), IsOkAndHolds(five));
  EXPECT_THAT(two.Mul(three), IsOkAndHolds(six));
}

TEST(TypeDimTest, TestGetAs64BitsU64) {
  EXPECT_THAT(TypeDim::GetAs64Bits(InterpValue::MakeUBits(
                  /*bit_count=*/64, static_cast<uint64_t>(-1))),
              IsOkAndHolds(int64_t{-1}));
}

TEST(TypeDimTest, TestGetAs64BitsU128) {
  EXPECT_THAT(
      TypeDim::GetAs64Bits(
          InterpValue::MakeBits(/*is_signed=*/false, Bits::AllOnes(128))),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("cannot be represented as an unsigned 64-bit value")));
}

TEST(TypeDimTest, TestGetAs64BitsS128) {
  EXPECT_THAT(TypeDim::GetAs64Bits(InterpValue::MakeBits(/*is_signed=*/true,
                                                         Bits::AllOnes(128))),
              IsOkAndHolds(-1));
}

TEST(TypeTest, TestEqualityOfBitsConstructorType) {
  BitsConstructorType bct_unsigned0(TypeDim::CreateBool(false));
  BitsConstructorType bct_unsigned1(TypeDim::CreateBool(false));
  BitsConstructorType bct_signed0(TypeDim::CreateBool(true));
  BitsConstructorType bct_signed1(TypeDim::CreateBool(true));
  EXPECT_EQ(bct_unsigned0, bct_unsigned1);
  EXPECT_EQ(bct_signed0, bct_signed1);
  EXPECT_NE(bct_unsigned0, bct_signed0);
  EXPECT_NE(bct_unsigned1, bct_signed1);
}

TEST(TypeTest, TestEqualityOfBitsConstructorTypeArrays) {
  BitsConstructorType bct_unsigned0(TypeDim::CreateBool(false));
  BitsConstructorType bct_unsigned1(TypeDim::CreateBool(false));

  BitsConstructorType bct_signed0(TypeDim::CreateBool(true));
  BitsConstructorType bct_signed1(TypeDim::CreateBool(true));

  ArrayType array_u8_0(bct_unsigned0.CloneToUnique(), TypeDim::CreateU32(8));
  ArrayType array_s8_0(bct_signed0.CloneToUnique(), TypeDim::CreateU32(8));

  ArrayType array_u8_1(bct_unsigned1.CloneToUnique(), TypeDim::CreateU32(8));
  ArrayType array_s8_1(bct_signed1.CloneToUnique(), TypeDim::CreateU32(8));

  EXPECT_EQ(array_u8_0, array_u8_1);
  EXPECT_EQ(array_s8_0, array_s8_1);
  EXPECT_NE(array_u8_0, array_s8_0);
  EXPECT_NE(array_u8_1, array_s8_1);
}

// Verifies: equal bit arguments select the same sum cache bucket.
// Catches: hashing a value tag or xN notation as semantic identity.
TEST(TypeTest, SumArgumentHashUsesSemanticBitEquality) {
  std::vector<SumType::ParametricArgument> lhs;
  lhs.emplace_back(InterpValue::MakeU32(7));
  lhs.emplace_back(BitsType::MakeU8());
  std::vector<SumType::ParametricArgument> rhs;
  rhs.emplace_back(InterpValue::MakeSBits(32, 7));
  rhs.emplace_back(std::make_unique<ArrayType>(
      std::make_unique<BitsConstructorType>(TypeDim::CreateBool(false)),
      TypeDim::CreateU32(8)));

  ASSERT_EQ(std::get<InterpValue>(lhs[0]), std::get<InterpValue>(rhs[0]));
  ASSERT_EQ(*std::get<std::unique_ptr<const Type>>(lhs[1]),
            *std::get<std::unique_ptr<const Type>>(rhs[1]));
  EXPECT_EQ(SumType::HashParametricArguments(lhs),
            SumType::HashParametricArguments(rhs));
}

// Verifies: equal eager arrays and symbolic ranges select the same sum bucket.
// Catches: hashing an array's storage representation instead of its elements.
TEST(TypeTest, SumArgumentHashUsesLogicalArrayValues) {
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue array,
                           InterpValue::MakeArray({InterpValue::MakeU32(1),
                                                   InterpValue::MakeU32(2)}));
  InterpValue range = InterpValue::MakeSymbolicRange(InterpValue::MakeU32(1),
                                                     InterpValue::MakeU32(3));
  ASSERT_EQ(array, range);
  std::vector<SumType::ParametricArgument> lhs;
  lhs.emplace_back(std::move(array));
  std::vector<SumType::ParametricArgument> rhs;
  rhs.emplace_back(std::move(range));
  EXPECT_EQ(SumType::HashParametricArguments(lhs),
            SumType::HashParametricArguments(rhs));
}

}  // namespace
}  // namespace xls::dslx
