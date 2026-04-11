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

#include "xls/dslx/type_system/type_info_to_proto.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "re2/re2.h"
#include "xls/common/golden_files.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type_info.pb.h"

namespace xls::dslx {
namespace {

std::string TestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

class TypeInfoToProtoWithBothTypecheckVersionsTest : public ::testing::Test {
 public:
  void DoRun(std::string_view program, TypeInfoProto* proto_out = nullptr,
             ImportData* import_data = nullptr) {
    std::optional<ImportData> local_import_data;
    if (import_data == nullptr) {
      local_import_data.emplace(CreateImportDataForTest());
      import_data = &local_import_data.value();
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "fake.x", "fake", import_data, nullptr));

    XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string nodes_text,
        ToHumanString(tip, *import_data, import_data->file_table()));

    std::string test_name(TestName());
    // Remove parametric test suite suffix.
    RE2::GlobalReplace(&test_name, R"(/\d+)", "");

    std::filesystem::path golden_file_path = absl::StrFormat(
        "xls/dslx/type_system/testdata/type_info_to_proto_test_%s.txt",
        test_name);
    ExpectEqualToGoldenFile(golden_file_path, nodes_text);

    if (proto_out != nullptr) {
      *proto_out = tip;
    }
  }
};

const AstNodeTypeInfoProto* FindSumTypeInfoNode(const TypeInfoProto& tip,
                                                std::string_view identifier) {
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    if (node.type().sum_type().has_sum_def() &&
        node.type().sum_type().sum_def().identifier() == identifier) {
      return &node;
    }
  }
  return nullptr;
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, IdentityFunction) {
  std::string program = R"(fn id(x: u32) -> u32 { x })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ParametricIdentityFunction) {
  std::string program = R"(
fn pid<N: u32>(x: bits[N]) -> bits[N] { x }
fn id(x: u32) -> u32 { pid<u32:32>(x) }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, UnitFunction) {
  std::string program = R"(fn f() -> () { () })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, ArrayFunction) {
  std::string program = R"(fn f() -> u8[2] { u8[2]:[u8:1, u8:2] })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, TokenFunction) {
  std::string program = R"(fn f(x: token) -> token { x })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       MakeStructInstanceFunction) {
  std::string program = R"(
struct S { x: u32 }
fn f() -> S { S { x: u32:42 } }
)";
  TypeInfoProto tip;
  DoRun(program, &tip);
  EXPECT_THAT(
      tip.ShortDebugString(),
      ::testing::ContainsRegex(
          R"(struct_def \{ span \{ .*? \} identifier: "S" member_names: "x" is_public: false \})"));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, MakeEnumFunction) {
  std::string program = R"(
enum E : u32 { A = 42 }
fn f() -> E { E::A }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, MakeSumFunction) {
  std::string program = R"(
sum Option {
  None,
  Some(u32),
}
fn f() -> Option { Option::None }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsReorderedSumVariantsInToHumanString) {
  std::string program = R"(
sum Option {
  None,
  Some(u32),
}
fn f() -> Option { Option::None }
)";

  ImportData import_data = CreateImportDataForTest();
  TypeInfoProto tip;
  DoRun(program, &tip, &import_data);

  int mutated_nodes = 0;
  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def() || sum_type->sum_def().identifier() != "Option") {
      continue;
    }
    ASSERT_EQ(sum_type->variants_size(), 2);
    sum_type->mutable_variants()->SwapElements(0, 1);
    ++mutated_nodes;
  }
  ASSERT_GT(mutated_nodes, 0);

  EXPECT_THAT(ToHumanString(tip, import_data, import_data.file_table()),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumEmptyPayloadKinds) {
  std::string program = R"(
sum E {
  None,
  EmptyTuple(),
  EmptyStruct { },
  Some(u32),
  Point { x: u32 },
}

fn f(x: bool) -> E {
  if x { E::EmptyTuple() } else { E::EmptyStruct { } }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));

  const SumTypeProto* sum_type = nullptr;
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    const SumTypeProto& candidate = node.type().sum_type();
    if (candidate.has_sum_def() && candidate.sum_def().identifier() == "E") {
      sum_type = &candidate;
      break;
    }
  }
  ASSERT_NE(sum_type, nullptr);
  ASSERT_TRUE(sum_type->has_sum_def());
  ASSERT_EQ(sum_type->sum_def().variants_size(), 5);
  EXPECT_EQ(sum_type->sum_def().variants(1).identifier(), "EmptyTuple");
  EXPECT_EQ(sum_type->sum_def().variants(1).kind(), SUM_VARIANT_KIND_TUPLE);
  EXPECT_EQ(sum_type->sum_def().variants(2).identifier(), "EmptyStruct");
  EXPECT_EQ(sum_type->sum_def().variants(2).kind(), SUM_VARIANT_KIND_STRUCT);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsReorderedSumVariantsInProtoImport) {
  std::string program = R"(
sum Option {
  None,
  Some(u32),
}

fn f(x: bool) -> Option {
  if x { Option::None } else { Option::Some(u32:42) }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));

  const AstNodeTypeInfoProto* sum_node = FindSumTypeInfoNode(tip, "Option");
  ASSERT_NE(sum_node, nullptr);

  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def() || sum_type->sum_def().identifier() != "Option") {
      continue;
    }
    sum_type->mutable_variants()->SwapElements(0, 1);
  }

  EXPECT_THAT(
      ToHumanString(*sum_node, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant order mismatch for `Option`")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsDuplicateAndMissingSumVariantsInProtoImport) {
  std::string program = R"(
sum Option {
  None,
  Some(u32),
}

fn f(x: bool) -> Option {
  if x { Option::None } else { Option::Some(u32:42) }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));

  const AstNodeTypeInfoProto* sum_node = FindSumTypeInfoNode(tip, "Option");
  ASSERT_NE(sum_node, nullptr);

  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def() || sum_type->sum_def().identifier() != "Option") {
      continue;
    }
    *sum_type->mutable_variants(1) = sum_type->variants(0);
  }

  EXPECT_THAT(
      ToHumanString(*sum_node, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant order mismatch for `Option`")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ImportModuleAndTypeAliasAnEnum) {
  std::string imported = R"(
pub enum Foo : u32 {
  A = 42,
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(imported, "my_imported_module.x", "my_imported_module",
                        &import_data));
  (void)tm;

  std::string program = R"(
import my_imported_module;

type MyFoo = my_imported_module::Foo;
)";
  DoRun(program, /*proto_out=*/nullptr, &import_data);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, ProcWithImpl) {
  std::string program = R"(
proc Foo { a: u32 }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, BitsConstructorTypeProto) {
  std::string program = R"(
fn distinct<COUNT: u32, N: u32, S: bool>(items: xN[S][N][COUNT], valid: bool[COUNT]) -> bool { fail!("unimplemented", zero!<bool>()) }

#[test]
fn test_simple_nondistinct() {
    assert_eq(distinct(u2[2]:[1, 1], bool[2]:[true, true]), false)
}
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SkipsSyntheticNoFileEntriesInHumanizedOutput) {
  std::string program = R"(
fn bool_update() -> bool[1] {
  update(bool[1]:[false], u1:0, true)
}

fn bit_update() -> u8 {
  bit_slice_update(u8:0, u3:0, true)
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string nodes_text,
      ToHumanString(tip, import_data, import_data.file_table()));

  EXPECT_THAT(nodes_text,
              ::testing::HasSubstr("update(bool[1]:[false], u1:0, true)"));
  EXPECT_THAT(nodes_text,
              ::testing::HasSubstr("bit_slice_update(u8:0, u3:0, true)"));
  EXPECT_THAT(nodes_text, ::testing::Not(::testing::HasSubstr("<no-file>")));
}

}  // namespace
}  // namespace xls::dslx
