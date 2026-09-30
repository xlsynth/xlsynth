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

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/ir/name_uniquer.h"

namespace xls::dslx::verilog_sum {
namespace {

SumType MakePhantomSum(const SumDef& definition,
                       std::vector<NominalParametricArgument> arguments) {
  std::vector<std::unique_ptr<Type>> payload;
  payload.push_back(BitsType::MakeU1());
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeTuple(*definition.variants().front(),
                                               std::move(payload)));
  return SumType(definition, std::move(variants), std::nullopt, {},
                 std::move(arguments));
}

// Verifies: variant and family names are stable across declaration order.
// Catches: order-dependent collisions and unwanted names for tag-only sums.
TEST(VerilogSumNamingTest, VariantCollisionsUseSourceOrder) {
  constexpr std::string_view kProgram = R"(
pub enum Forward { FooBar(), Foo_Bar(), HTTPRequest() }
pub enum Reverse { HTTPRequest(), Foo_Bar(), FooBar() }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "names.x", "names", &import_data));
  const auto sums = tm.module->GetSumDefs();
  ASSERT_EQ(sums.size(), 2);
  const std::map<std::string, std::string> expected{
      {"FooBar", "foo_bar"},
      {"Foo_Bar", "foo_bar__1"},
      {"HTTPRequest", "http_request"}};
  EXPECT_EQ(VariantSuffixes(*sums[0]), expected);
  EXPECT_EQ(VariantSuffixes(*sums[1]), expected);

  const auto full = FamilyNameRequests(*sums[1], "Event", true);
  EXPECT_EQ(full.at("constructor:FooBar"), "Event_make_foo_bar");
  EXPECT_EQ(full.at("constructor:Foo_Bar"), "Event_make_foo_bar__1");
  EXPECT_EQ(full.at("tag:Foo_Bar"), "Event_tag_Foo_Bar");
  EXPECT_EQ(full.at("view:Foo_Bar"), "Event_foo_bar__1_view_t");
  EXPECT_EQ(full.at("payload"), "Event_payload_t");

  const auto tag_only = FamilyNameRequests(*sums[1], "Event", false);
  EXPECT_EQ(tag_only.at("getter"), "Event_get_tag");
  EXPECT_EQ(tag_only.at("tag"), "Event_tag_t");
  EXPECT_EQ(tag_only.at("constructor:Foo_Bar"), "Event_make_foo_bar__1");
  EXPECT_FALSE(tag_only.contains("payload"));
  EXPECT_FALSE(tag_only.contains("view:Foo_Bar"));

  NameUniquer members("__");
  EXPECT_EQ(MemberName(members, "byte"), "byte_");
  EXPECT_EQ(MemberName(members, "byte_"), "byte___1");
}

// Verifies: resolved type and value arguments produce stable distinct names.
// Catches: collisions between bit widths, signedness, and array payloads.
TEST(VerilogSumNamingTest, TypeAndValueSpecializationsHaveStableIdentities) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Sized<N: u32> { Value(uN[N]) }
enum Box<T: type> { Value(T) }
enum Signed<N: s8> { Value(u8) }
fn sizes(a: Sized<u32:8>, b: Sized<u32:16>) -> Sized<u32:8> { a }
fn boxes(a: Box<u8>, b: Box<s8>, c: Box<u8[2]>) -> Box<u8> { a }
fn negative(a: Signed<s8:-1>) -> Signed<s8:-1> { a }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "names.x", "names", &import_data));
  auto sizes = tm.module->GetFunction("sizes");
  ASSERT_TRUE(sizes.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * sizes_type,
                           tm.type_info->GetItemAs<FunctionType>(*sizes));
  XLS_ASSERT_OK_AND_ASSIGN(std::string narrow,
                           TypeIdentity(*sizes_type->params()[0]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string wide,
                           TypeIdentity(*sizes_type->params()[1]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string repeated,
                           TypeIdentity(sizes_type->return_type()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string narrow_name,
      SpecializationName(sizes_type->params()[0]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string wide_name,
      SpecializationName(sizes_type->params()[1]->AsSum()));
  EXPECT_EQ(narrow, repeated);
  EXPECT_NE(narrow, wide);
  EXPECT_NE(narrow_name, wide_name);
  EXPECT_EQ(narrow_name, "__value_3a_5_3a_u32_3a_8");
  EXPECT_EQ(wide_name, "__value_3a_6_3a_u32_3a_16");

  auto boxes = tm.module->GetFunction("boxes");
  ASSERT_TRUE(boxes.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * boxes_type,
                           tm.type_info->GetItemAs<FunctionType>(*boxes));
  XLS_ASSERT_OK_AND_ASSIGN(std::string unsigned_box,
                           TypeIdentity(*boxes_type->params()[0]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string signed_box,
                           TypeIdentity(*boxes_type->params()[1]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string array_box,
                           TypeIdentity(*boxes_type->params()[2]));
  EXPECT_EQ(unsigned_box,
            "9425692d067db1bcf379f789971521beafd57c3142c3b836a4b95f62e4734165");
  EXPECT_NE(unsigned_box, signed_box);
  EXPECT_NE(unsigned_box, array_box);
  EXPECT_NE(signed_box, array_box);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string unsigned_name,
      SpecializationName(boxes_type->params()[0]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string signed_name,
      SpecializationName(boxes_type->params()[1]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string array_name,
      SpecializationName(boxes_type->params()[2]->AsSum()));
  EXPECT_EQ(unsigned_name, "__type_3a_u8");
  EXPECT_EQ(signed_name, "__type_3a_s8");
  EXPECT_EQ(array_name, "__type_3a_array_3a_2_5b_u8_5d_");

  auto negative = tm.module->GetFunction("negative");
  ASSERT_TRUE(negative.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * negative_type,
                           tm.type_info->GetItemAs<FunctionType>(*negative));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string negative_name,
      SpecializationName(negative_type->params()[0]->AsSum()));
  EXPECT_EQ(negative_name, "__value_3a_5_3a_s8_3a__2d_1");
}

// Verifies: equivalent DSLX bit spellings keep their familiar numeric suffix.
// Catches: changing from u8 to uN[8], xN, or a type alias producing a hash.
TEST(VerilogSumNamingTest, NumericBindingSpellingsKeepReadableSuffixes) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
type U = uN[8];
type UU = U;
type S = sN[8];
enum UShort<N: u8> { Value(u1) }
enum ULong<N: uN[8]> { Value(u1) }
enum UBits<N: bits[8]> { Value(u1) }
enum UGeneric<N: xN[false][8]> { Value(u1) }
enum UAlias<N: UU> { Value(u1) }
enum SShort<N: s8> { Value(u1) }
enum SLong<N: sN[8]> { Value(u1) }
enum SGeneric<N: xN[true][8]> { Value(u1) }
enum SAlias<N: S> { Value(u1) }
enum Dependent<T: type, N: T> { Value(u1) }
enum Dynamic<SIGNED: bool, V: xN[SIGNED][8]> { Value(u1) }
fn fixture(a: UShort<u8:1>, b: ULong<u8:1>, c: UBits<u8:1>,
           d: UGeneric<u8:1>, e: UAlias<u8:1>, f: SShort<s8:-1>,
           g: SLong<s8:-1>, h: SGeneric<s8:-1>, i: SAlias<s8:-1>,
           j: Dependent<u8, u8:1>, k: Dependent<s8, s8:-1>,
           l: Dynamic<false, u8:1>, m: Dynamic<true, s8:-1>) { () }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "names.x", "names", &import_data));
  auto fixture = tm.module->GetFunction("fixture");
  ASSERT_TRUE(fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*fixture));
  IdentityBuilder identities;
  ASSERT_EQ(types->params().size(), 13);
  for (size_t i = 0; i < 9; ++i) {
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string name,
        identities.SpecializationName(types->params()[i]->AsSum()));
    EXPECT_EQ(name,
              i < 5 ? "__value_3a_4_3a_u8_3a_1" : "__value_3a_5_3a_s8_3a__2d_1")
        << i;
  }
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string dependent_unsigned,
      identities.SpecializationName(types->params()[9]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string dependent_signed,
      identities.SpecializationName(types->params()[10]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string dynamic_unsigned,
      identities.SpecializationName(types->params()[11]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string dynamic_signed,
      identities.SpecializationName(types->params()[12]->AsSum()));
  EXPECT_EQ(dependent_unsigned, "__type_3a_u8__value_3a_4_3a_u8_3a_1");
  EXPECT_EQ(dependent_signed, "__type_3a_s8__value_3a_5_3a_s8_3a__2d_1");
  EXPECT_EQ(dynamic_unsigned, "__value_3a_4_3a_u1_3a_0__value_3a_4_3a_u8_3a_1");
  EXPECT_EQ(dynamic_signed,
            "__value_3a_4_3a_u1_3a_1__value_3a_5_3a_s8_3a__2d_1");
}

// Struct names keep the binding identifier and render even signed values as
// unsigned decimal bits.
TEST(VerilogSumNamingTest, StructSpecializationsKeepReadableSuffixes) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
struct Sized<WIDTH: u32> { value: uN[WIDTH] }
struct Record<T: type, DELTA: s8> { value: T }
fn fixture(a: Sized<u32:8>, b: Sized<u32:16>, c: Record<u8, s8:-1>) { () }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "records.x", "records", &import_data));
  auto fixture = tm.module->GetFunction("fixture");
  ASSERT_TRUE(fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*fixture));
  ASSERT_EQ(types->params().size(), 3);
  IdentityBuilder identities;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string narrow,
      identities.StructSpecializationName(types->params()[0]->AsStruct()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string wide,
      identities.StructSpecializationName(types->params()[1]->AsStruct()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string negative,
      identities.StructSpecializationName(types->params()[2]->AsStruct()));
  EXPECT_EQ(narrow, "__binding_3a_5_3a_WIDTH1_3a_8");
  EXPECT_EQ(wide, "__binding_3a_5_3a_WIDTH2_3a_16");
  EXPECT_EQ(negative, "__type_3a_u8__binding_3a_5_3a_DELTA3_3a_255");
}

// Numeric identity ignores runtime tags; enum annotations must make the same
// public-name decision for raw bits and enum-tagged values with those bits.
TEST(VerilogSumNamingTest, EnumAndRawBitsHaveTheSamePublicName) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Code: u8 { One = 1 }
type CodeAlias = Code;
enum Direct<V: Code> { Value(u1) }
enum Aliased<V: CodeAlias> { Value(u1) }
enum Dependent<T: type, V: T> { Value(u1) }
enum Dynamic<SIGNED: bool, V: xN[SIGNED][8]> { Value(u1) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "numeric.x", "numeric", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(EnumDef * code,
                           tm.module->GetMemberOrError<EnumDef>("Code"));
  const InterpValue raw = InterpValue::MakeU8(1);
  const InterpValue tagged =
      InterpValue::MakeEnum(raw.GetBitsOrDie(), false, code);
  const EnumType enum_type(*code, TypeDim::CreateU32(8), false, {tagged});
  struct Case {
    std::string_view definition;
    const Type* preceding_type;
    std::optional<bool> preceding_sign;
    std::string_view name;
  };
  const std::unique_ptr<Type> bits_type = BitsType::MakeU8();
  const Case cases[] = {
      {"Direct", nullptr, std::nullopt,
       "__hadb8c0816a88cc047b66e929c57bc7215c1c3248a9f74a996291195ac8439aaf"},
      {"Aliased", nullptr, std::nullopt,
       "__h45f7eea673c8cc1103791d26c89859fdee42bd47984c688045e1985ada3a6054"},
      {"Dependent", &enum_type, std::nullopt,
       "__hf2d987ac4d81f6d2d3bcfadbc2a5eda9245a5644c35a2c77f4075ea582ba9a8d"},
      {"Dependent", bits_type.get(), std::nullopt,
       "__type_3a_u8__value_3a_4_3a_u8_3a_1"},
      {"Dynamic", nullptr, false,
       "__value_3a_4_3a_u1_3a_0__value_3a_4_3a_u8_3a_1"},
      {"Dynamic", nullptr, true,
       "__value_3a_4_3a_u1_3a_1__value_3a_4_3a_s8_3a_1"},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.definition);
    XLS_ASSERT_OK_AND_ASSIGN(
        SumDef * definition,
        tm.module->GetMemberOrError<SumDef>(test.definition));
    auto specialized = [&](const InterpValue& value) {
      std::vector<NominalParametricArgument> arguments;
      if (test.preceding_type != nullptr) {
        arguments.emplace_back(test.preceding_type->CloneToUnique());
      } else if (test.preceding_sign.has_value()) {
        arguments.emplace_back(InterpValue::MakeBool(*test.preceding_sign));
      }
      arguments.emplace_back(value);
      return MakePhantomSum(*definition, std::move(arguments));
    };
    const SumType raw_sum = specialized(raw);
    const SumType tagged_sum = specialized(tagged);
    EXPECT_TRUE(
        raw_sum.HasSameParametricArguments(tagged_sum.parametric_arguments()));

    // Each representation must produce the expected name before its equal
    // counterpart can populate the nominal cache.
    IdentityBuilder raw_first;
    XLS_ASSERT_OK_AND_ASSIGN(std::string raw_uncached,
                             raw_first.SpecializationName(raw_sum));
    XLS_ASSERT_OK_AND_ASSIGN(std::string tagged_after_raw,
                             raw_first.SpecializationName(tagged_sum));
    IdentityBuilder tagged_first;
    XLS_ASSERT_OK_AND_ASSIGN(std::string tagged_uncached,
                             tagged_first.SpecializationName(tagged_sum));
    XLS_ASSERT_OK_AND_ASSIGN(std::string raw_after_tagged,
                             tagged_first.SpecializationName(raw_sum));
    EXPECT_EQ(raw_uncached, test.name);
    EXPECT_EQ(tagged_after_raw, test.name);
    EXPECT_EQ(tagged_uncached, test.name);
    EXPECT_EQ(raw_after_tagged, test.name);

    XLS_ASSERT_OK_AND_ASSIGN(std::string raw_key,
                             raw_first.TypeIdentity(raw_sum));
    XLS_ASSERT_OK_AND_ASSIGN(std::string tagged_key,
                             tagged_first.TypeIdentity(tagged_sum));
    EXPECT_EQ(raw_key, tagged_key);
  }
}

// A token nested in a value must reject naming instead of generating an
// unstable public identifier. Token type arguments are separately supported.
TEST(VerilogSumNamingTest, RejectsTokenInsideValueArgument) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
struct Value { byte: u8, sequence: token }
enum Marker<V: Value> { Only(u1) }
const ARGUMENT = Value { byte: u8:1, sequence: token() };
fn accept(value: Marker<ARGUMENT>) { () }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "tokens.x", "tokens", &import_data));
  const auto function = tm.module->GetFunction("accept");
  ASSERT_TRUE(function.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * function_type,
                           tm.type_info->GetItemAs<FunctionType>(*function));
  const SumType& sum = function_type->params().front()->AsSum();
  IdentityBuilder identities;
  const auto result = identities.SpecializationName(sum);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Cannot export a SystemVerilog sum specialization whose value "
            "argument contains a token: tokens do not have a stable generated "
            "name");
}

// Verifies: unused token types and concrete proc argument identities can name
// sums, independently of the public exporter.
TEST(VerilogSumNamingTest, OpaqueTypeArgumentsRetainTheirSpecializations) {
  constexpr std::string_view kProgram = R"(#![feature(explicit_state_access)]
#![feature(generics)]
proc Worker<N: u32 = {u32:1}, T: type = u8> {}
type One = Worker<u32:1, u8>;
type Defaulted = Worker;
type Two = Worker<u32:2, u8>;
type Wide = Worker<u32:1, u16>;
enum Marker<T: type> { Empty, Only(u1) }
fn fixture(a: Marker<One>, b: Marker<Defaulted>, c: Marker<Two>,
           d: Marker<Wide>, e: Marker<token>) { () }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "opaque.x", "opaque", &import_data));
  auto fixture = tm.module->GetFunction("fixture");
  ASSERT_TRUE(fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*fixture));
  IdentityBuilder identities;
  std::vector<std::string> names;
  for (const auto& type : types->params()) {
    XLS_ASSERT_OK_AND_ASSIGN(std::string name,
                             identities.SpecializationName(type->AsSum()));
    names.push_back(std::move(name));
  }
  ASSERT_EQ(names.size(), 5);
  EXPECT_EQ(names[0], names[1]);
  EXPECT_EQ((std::set<std::string>(names.begin(), names.end())).size(), 4);
  EXPECT_EQ(names[4], "__type_3a_token");
}

// Verifies: eager and lazy arrays use logical elements, and repeats reuse them.
// Catches: storage- or order-dependent names and repeated aggregate traversal.
TEST(VerilogSumNamingTest, AggregateValuesUseLogicalElements) {
  auto get_name = [](std::string_view value) -> absl::StatusOr<std::string> {
    std::string source = absl::StrCat(R"(#![feature(generics)]
struct Values { xs: u32[2] }
enum Marker<V: Values> { Empty, Only(u1) }
const VALUE = Values { xs: )",
                                      value, R"( };
fn marker(x: Marker<VALUE>) -> Marker<VALUE> { x }
)");
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSIGN_OR_RETURN(
        TypecheckedModule tm,
        ParseAndTypecheck(source, "values.x", "values", &import_data));
    XLS_ASSIGN_OR_RETURN(FunctionType * marker,
                         tm.type_info->GetItemAs<FunctionType>(
                             *tm.module->GetFunction("marker")));
    IdentityBuilder identities;
    XLS_ASSIGN_OR_RETURN(std::string name, identities.SpecializationName(
                                               marker->params()[0]->AsSum()));
    const auto computations =
        identities.value_identity_computations_for_testing();
    EXPECT_GT(computations, 0);
    XLS_ASSIGN_OR_RETURN(
        std::string repeated,
        identities.SpecializationName(marker->return_type().AsSum()));
    EXPECT_EQ(repeated, name);
    EXPECT_EQ(identities.value_identity_computations_for_testing(),
              computations);
    return name;
  };
  XLS_ASSERT_OK_AND_ASSIGN(std::string eager, get_name("[u32:0, u32:1]"));
  XLS_ASSERT_OK_AND_ASSIGN(std::string exclusive, get_name("u32:0..u32:2"));
  XLS_ASSERT_OK_AND_ASSIGN(std::string inclusive, get_name("u32:0..=u32:1"));
  XLS_ASSERT_OK_AND_ASSIGN(std::string reversed, get_name("[u32:1, u32:0]"));
  EXPECT_EQ(eager, exclusive);
  EXPECT_EQ(eager, inclusive);
  EXPECT_NE(eager, reversed);
  EXPECT_EQ(
      eager,
      "__h7b543c898eabdc61e9458e98728ea9557082ccbe1648e9495afe9d17bfb21760");
}

// A shared symbolic range must not be traversed for every otherwise distinct
// specialization. Its public hash must match the same eager sequence and differ
// from a shifted symbolic sequence regardless of encounter order.
TEST(VerilogSumNamingTest, DistinctSpecializationsReuseSymbolicRangeValues) {
  constexpr std::string_view kDefinitions = R"(#![feature(generics)]
struct Values { xs: u32[64] }
enum Marker<N: u32, V: Values> { Only(u1) }
)";
  const std::string range_program = absl::StrCat(kDefinitions, R"(
const EXCLUSIVE = Values { xs: u32:0..u32:64 };
const OTHER_EXCLUSIVE = Values { xs: u32:0..u32:64 };
const SHIFTED = Values { xs: u32:1..u32:65 };
fn fixture(a: Marker<u32:1, EXCLUSIVE>, b: Marker<u32:2, EXCLUSIVE>,
           c: Marker<u32:3, OTHER_EXCLUSIVE>, d: Marker<u32:2, SHIFTED>) { () }
)");
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(range_program, "ranges.x", "ranges", &import_data));
  auto fixture = tm.module->GetFunction("fixture");
  ASSERT_TRUE(fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*fixture));
  ASSERT_EQ(types->params().size(), 4);
  auto sequence = [](const Type& type) -> const InterpValue& {
    const auto& arguments = type.AsSum().specialization_arguments();
    return std::get<InterpValue>(arguments[1]).GetValuesOrDie().front();
  };
  for (const auto& type : types->params()) {
    ASSERT_TRUE(sequence(*type).GetRangeData().has_value());
  }

  IdentityBuilder identities;
  XLS_ASSERT_OK_AND_ASSIGN(std::string first, identities.SpecializationName(
                                                  types->params()[0]->AsSum()));
  const auto after_first = identities.value_identity_computations_for_testing();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string second,
      identities.SpecializationName(types->params()[1]->AsSum()));
  const auto after_second =
      identities.value_identity_computations_for_testing();
  XLS_ASSERT_OK_AND_ASSIGN(std::string third, identities.SpecializationName(
                                                  types->params()[2]->AsSum()));
  const auto after_third = identities.value_identity_computations_for_testing();
  EXPECT_NE(first, second);
  EXPECT_NE(first, third);
  EXPECT_NE(second, third);
  EXPECT_LE(after_second, after_first + 5);
  EXPECT_LE(after_third, after_second + 5);
  EXPECT_EQ(second.size(), 67);
  EXPECT_EQ(second.substr(0, 3), "__h");

  XLS_ASSERT_OK_AND_ASSIGN(
      std::string shifted,
      identities.SpecializationName(types->params()[3]->AsSum()));
  EXPECT_NE(second, shifted);
  IdentityBuilder shifted_first;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string shifted_uncached,
      shifted_first.SpecializationName(types->params()[3]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string second_after_shifted,
      shifted_first.SpecializationName(types->params()[1]->AsSum()));
  EXPECT_EQ(shifted, shifted_uncached);
  EXPECT_EQ(second, second_after_shifted);

  const std::string eager_program = absl::StrCat(kDefinitions, R"(
const EAGER = Values { xs: u32[64]:[
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
] };
fn fixture(a: Marker<u32:2, EAGER>) { () }
)");
  ImportData eager_import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule eager_tm,
                           ParseAndTypecheck(eager_program, "ranges.x",
                                             "ranges", &eager_import_data));
  auto eager_fixture = eager_tm.module->GetFunction("fixture");
  ASSERT_TRUE(eager_fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * eager_types,
      eager_tm.type_info->GetItemAs<FunctionType>(*eager_fixture));
  ASSERT_EQ(eager_types->params().size(), 1);
  ASSERT_FALSE(sequence(*eager_types->params()[0]).GetRangeData().has_value());
  IdentityBuilder eager_identities;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string eager,
      eager_identities.SpecializationName(eager_types->params()[0]->AsSum()));
  EXPECT_EQ(second, eager);
}

// A nominal argument is part of a type even when it does not occur in its
// fields. The two Box types must not silently share one family spelling.
TEST(VerilogSumNamingTest, RetainsPhantomStructArguments) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
struct Phantom<N: u32> { value: u8 }
enum Box<T: type> { Value(T) }
fn boxes(a: Box<Phantom<u32:1>>, b: Box<Phantom<u32:2>>)
    -> Box<Phantom<u32:1>> { a }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "phantom.x", "phantom", &import_data));
  auto boxes = tm.module->GetFunction("boxes");
  ASSERT_TRUE(boxes.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*boxes));
  IdentityBuilder identities;
  XLS_ASSERT_OK_AND_ASSIGN(std::string first_key,
                           identities.TypeIdentity(*types->params()[0]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string second_key,
                           identities.TypeIdentity(*types->params()[1]));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string first_name,
      identities.SpecializationName(types->params()[0]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string second_name,
      identities.SpecializationName(types->params()[1]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string repeated_name,
      identities.SpecializationName(types->return_type().AsSum()));
  EXPECT_NE(first_key, second_key);
  EXPECT_NE(first_name, second_name);
  EXPECT_EQ(first_name, repeated_name);
}

// Verifies: nested sum hashes preserve argument order with bounded naming work.
// Catches: ignored arguments and exponential or repeated sum traversal.
TEST(VerilogSumNamingTest, RepeatedNestedSumsHaveBoundedNames) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Pair<T: type, U: type> { Left(T), Right(U) }
type A0 = u8;
type A1 = Pair<A0, A0>;
type A2 = Pair<A1, A1>;
type A3 = Pair<A2, A2>;
type A4 = Pair<A3, A3>;
type A5 = Pair<A4, A4>;
type A6 = Pair<A5, A5>;
type A7 = Pair<A6, A6>;
type A8 = Pair<A7, A7>;
fn nested(a: A8, b: A7, c: Pair<A7, u8>, d: Pair<A7, u16>, e: Pair<u8, A7>)
    -> A8 { a }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "nested.x", "nested", &import_data));
  auto nested = tm.module->GetFunction("nested");
  ASSERT_TRUE(nested.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*nested));
  IdentityBuilder identities;
  XLS_ASSERT_OK_AND_ASSIGN(std::string large, identities.SpecializationName(
                                                  types->params()[0]->AsSum()));
  const auto computations = identities.sum_identity_computations_for_testing();
  EXPECT_EQ(computations, 8);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string repeated,
      identities.SpecializationName(types->return_type().AsSum()));
  EXPECT_EQ(large, repeated);
  EXPECT_EQ(identities.sum_identity_computations_for_testing(), computations);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string smaller,
      identities.SpecializationName(types->params()[1]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string narrow_second,
      identities.SpecializationName(types->params()[2]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string wide_second,
      identities.SpecializationName(types->params()[3]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string swapped,
      identities.SpecializationName(types->params()[4]->AsSum()));
  const std::set<std::string> distinct{large, smaller, narrow_second,
                                       wide_second, swapped};
  EXPECT_EQ(distinct.size(), 5);
  for (const std::string& name : distinct) {
    EXPECT_EQ(name.size(), 67);
    EXPECT_EQ(name.substr(0, 3), "__h");
  }
}

// Verifies: phantom channel arguments retain their direction and payload type.
// Catches: unsupported channels or merged specializations.
TEST(VerilogSumNamingTest, RetainsPhantomChannelArguments) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Marker<T: type> { Empty, Only(u1) }
fn fixture(a: Marker<chan<u8> in>, b: Marker<chan<u8> out>,
           c: Marker<chan<u16> in>) -> Marker<chan<u8> in> { a }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "channels.x", "channels", &import_data));
  auto fixture = tm.module->GetFunction("fixture");
  ASSERT_TRUE(fixture.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * types,
                           tm.type_info->GetItemAs<FunctionType>(*fixture));
  IdentityBuilder identities;
  std::set<std::string> keys;
  std::set<std::string> names;
  for (const auto& type : types->params()) {
    XLS_ASSERT_OK_AND_ASSIGN(std::string key, identities.TypeIdentity(*type));
    XLS_ASSERT_OK_AND_ASSIGN(std::string name,
                             identities.SpecializationName(type->AsSum()));
    keys.insert(key);
    names.insert(name);
  }
  EXPECT_EQ(keys.size(), 3);
  EXPECT_EQ(names.size(), 3);
  XLS_ASSERT_OK_AND_ASSIGN(std::string first_key,
                           identities.TypeIdentity(*types->params()[0]));
  XLS_ASSERT_OK_AND_ASSIGN(std::string repeated_key,
                           identities.TypeIdentity(types->return_type()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string first_name,
      identities.SpecializationName(types->params()[0]->AsSum()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string repeated_name,
      identities.SpecializationName(types->return_type().AsSum()));
  EXPECT_EQ(first_key, repeated_key);
  EXPECT_EQ(first_name, repeated_name);
}

// Verifies: a shared nested record graph gets a bounded, repeatable sum name.
// Catches: exponential record traversal and recomputation on repeated queries.
TEST(VerilogSumNamingTest, RepeatedNestedStructsHaveBoundedNamesAndWork) {
  constexpr int kDepth = 20;
  constexpr std::string_view kProgram = R"(#![feature(generics)]
struct P<T: type, U: type> { bit: u1 }
enum Box<T: type> { Value(T) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "nested.x", "nested", &import_data));
  const auto records = tm.module->GetStructDefs();
  const auto sums = tm.module->GetSumDefs();
  ASSERT_EQ(records.size(), 1);
  ASSERT_EQ(sums.size(), 1);

  // Build the shared type graph directly so the work count measures naming,
  // independently of source typechecking for a deeply nested specialization.
  std::unique_ptr<Type> current = BitsType::MakeU8();
  for (int i = 0; i < kDepth; ++i) {
    std::vector<NominalParametricArgument> arguments;
    arguments.emplace_back(current->CloneToUnique());
    arguments.emplace_back(std::move(current));
    std::vector<std::unique_ptr<Type>> members;
    members.push_back(BitsType::MakeU1());
    StructType record(std::move(members), *records.front(), {},
                      std::move(arguments));
    current = record.CloneToUnique();
  }
  std::vector<std::unique_ptr<Type>> payload;
  payload.push_back(current->CloneToUnique());
  std::vector<SumTypeVariant> variants;
  variants.push_back(SumTypeVariant::MakeTuple(
      *sums.front()->variants().front(), std::move(payload)));
  std::vector<NominalParametricArgument> arguments;
  arguments.emplace_back(std::move(current));
  SumType box(*sums.front(), std::move(variants), std::nullopt, {},
              std::move(arguments));

  IdentityBuilder identities;
  XLS_ASSERT_OK_AND_ASSIGN(std::string name,
                           identities.SpecializationName(box));
  const auto computations =
      identities.struct_identity_computations_for_testing();
  EXPECT_EQ(name.size(), 67);
  EXPECT_EQ(name.substr(0, 3), "__h");
  EXPECT_EQ(computations, kDepth);
  const std::unique_ptr<Type> cloned = box.CloneToUnique();
  XLS_ASSERT_OK_AND_ASSIGN(std::string repeated,
                           identities.SpecializationName(cloned->AsSum()));
  EXPECT_EQ(name, repeated);
  EXPECT_EQ(identities.struct_identity_computations_for_testing(),
            computations);
}

}  // namespace
}  // namespace xls::dslx::verilog_sum
