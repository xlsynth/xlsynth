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

#include "xls/dslx/translators/dslx_to_verilog.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "re2/re2.h"
#include "xls/common/golden_files.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/virtualizable_file_system.h"

namespace xls::dslx {

class DslxTypeToVerilogManagerTestPeer {
 public:
  static bool CanAddWithoutNameChanges(const DslxTypeToVerilogManager& manager,
                                       const SumType& sum) {
    return manager.CanAddDirectSumWithoutNameChanges(sum);
  }

  static bool HasCommittedGraph(const DslxTypeToVerilogManager& manager,
                                const SumType& sum) {
    return manager.sum_payload_graphs_.Contains(sum);
  }

  static absl::Status MarkThenReject(DslxTypeToVerilogManager& manager,
                                     const SumType& sum) {
    return manager.WithSumPayloadGraphs([&] {
      absl::Status status = manager.MarkSumPayloadNominals(
          sum, /*newly_emitted_names_displace_enum_members=*/false);
      if (status.ok()) {
        return absl::InvalidArgumentError("later emission rejected");
      } else {
        return status;
      }
    });
  }
};

namespace {

constexpr std::string_view kTestdataPath = "xls/dslx/translators/testdata";

int CountOccurrences(std::string_view text, std::string_view needle) {
  int count = 0;
  for (size_t offset = text.find(needle); offset != std::string_view::npos;
       offset = text.find(needle, offset + needle.size())) {
    ++count;
  }
  return count;
}

const SumType& ConcreteSum(const TypecheckedModule& module,
                           std::string_view name) {
  const TypeDefinition definition =
      module.module->GetTypeDefinition(name).value();
  const Type* type =
      module.type_info->GetItem(TypeDefinitionToAstNode(definition)).value();
  return type->AsMeta().wrapped()->AsSum();
}

class DslxToVerilogTest : public ::testing::Test {
 public:
  static std::string TestName() {
    // If we try to run the program it can't have the '/' in its name. Remove
    // them so this pattern works.
    std::string name =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    RE2::GlobalReplace(&name, R"(\/\d+)", "");
    return name;
  }

  std::filesystem::path GoldenFilePath(std::string_view file_ext) {
    return absl::StrFormat("%s/dslx_to_verilog_test_%s.%s", kTestdataPath,
                           TestName(), file_ext);
  }
};

TEST_F(DslxToVerilogTest, BasicTypesInFunctions) {
  constexpr std::string_view program =
      R"(
struct Point {
  x: u16,
  y: u32,
}

enum Option : u5 {
  ZERO = 0,
  ONE = 1,
}

type AliasType = Point;
type AliasType1 = Point[1];

fn add_point_elements(p : Point, o : Option, v : u5, a : Point[3], b: u34[5], c: bits[9], d: bits[431], e: AliasType, f: AliasType[1], g: AliasType1) -> (u16, u32, u64) {
  let additional = if o == Option::ZERO { u5:0  } else  { v };
  let sum = p.x as u64 + p.y as u64 + additional as u64;
  (p.x, p.y, sum)
}
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  Function* func = tm.module->GetFunction("add_point_elements").value();

  for (Param* p : func->params()) {
    XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionParam(
        func, &import_data, p->name_def()->identifier()));
  }

  XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionOutput(
      func, &import_data, "user_defined_output_type_t"));

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, BasicTypeDefinition) {
  constexpr std::string_view program =
      R"(
struct Point {
  x: u16,
  y: u32,
}

enum Option : u5 {
  ZERO = 0,
  ONE = 1,
}

type AliasType = Point;
type AliasType1 = Point[1];
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(def, &import_data));
  }

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

// Verifies: Sum declarations emit the expected public SystemVerilog API.
// Catches: Incorrect tags, payload layouts, or helper definitions.
TEST_F(DslxToVerilogTest, SemanticSumTypeDefinition) {
  constexpr std::string_view program =
      R"(
pub enum MaybeWord {
  None,
  Some(u32),
  Pair { lo: u8, hi: u8 },
}

pub enum ExplicitTagWidth : u5 {
  None = 0,
  Some(u8) = 1,
}

pub enum Singleton {
  Only(u16),
}
)";

  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(def, &import_data));
  }

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

// Verifies: Definitions and function aliases reuse one nested sum API.
// Catches: Duplicate helpers and outer types emitted before their dependencies.
TEST_F(DslxToVerilogTest, NestedSumFunctionParameterAndOutput) {
  constexpr std::string_view program = R"(
enum Inner { A(u8), B(u8) }
enum Outer { A(Inner), B(Inner) }
fn identity(x: Outer) -> Outer { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* function = tm.module->GetFunction("identity").value();
  const std::vector<TypeDefinition> definitions =
      tm.module->GetTypeDefinitions();
  ASSERT_EQ(definitions.size(), 2);

  // The direct definition and both function entry points must share one family
  // even if the first request is an alias with an unrelated public name.
  for (bool direct_first : {false, true}) {
    SCOPED_TRACE(direct_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    if (direct_first) {
      XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(definitions[1],
                                                             &import_data));
    } else {
      XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionOutput(
          function, &import_data, "output_t"));
    }
    XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionParam(
        function, &import_data, "x", "input_t"));
    if (direct_first) {
      XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionOutput(
          function, &import_data, "output_t"));
    } else {
      XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(definitions[1],
                                                             &import_data));
    }

    const std::string emitted = type_to_verilog.Emit();
    EXPECT_EQ(CountOccurrences(emitted, "} Inner_tag_t;"), 1) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "} Outer_tag_t;"), 1) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "Outer_get_tag ("), 1) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "Outer_make_a ("), 1) << emitted;
    EXPECT_NE(emitted.find("typedef Outer input_t;"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("typedef Outer output_t;"), std::string::npos)
        << emitted;
    EXPECT_EQ(emitted.find("input_t_get_tag"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("output_t_get_tag"), std::string::npos) << emitted;
    EXPECT_LT(emitted.find("} Inner;"), emitted.find("} Outer;")) << emitted;
  }
}

// Verifies: Sum views preserve field names, order, padding, and signed tags.
// Catches: Lost named or positional fields and padding names that collide.
TEST_F(DslxToVerilogTest, SumViewsPreserveSourceNamesAndCompilerStorage) {
  constexpr std::string_view program = R"(
pub enum Message {
  None,
  Byte(u8),
  Pair { hi: u8, lo: u8 },
  Positional(u8, u8),
}
pub enum Sparse: s3 {
  Empty = 0,
  Negative(u8) = -1,
  Positive(u8) = 2,
}
pub enum Collision {
  Padding { xls_padding: u8 },
  Wide(u16),
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(
        type_to_verilog.AddTypeForTypeDefinition(definition, &import_data));
  }

  const std::string emitted = type_to_verilog.Emit();
  for (std::string_view declaration :
       {"Message_tag_None = 2'h0",    "Message_tag_Byte = 2'h1",
        "Message_tag_Pair = 2'h2",    "Message_tag_Positional = 2'h3",
        "} Message_tag_t;",           "} Message_none_view_t;",
        "} Message_byte_view_t;",     "} Message_pair_view_t;",
        "} Message_payload_t;",       "Message_pair_view_t as_pair;",
        "logic [15:0] bits;",         "logic [7:0] xls_padding;",
        "logic [7:0] index_0;",       "logic [7:0] index_1;",
        "Message_make_none (",        "Message_make_byte (",
        "Message_make_pair (",        "Message_get_tag (",
        "Sparse_tag_Negative = 3'h7", "Sparse_tag_Positive = 3'h2"}) {
    EXPECT_NE(emitted.find(declaration), std::string::npos)
        << declaration << '\n'
        << emitted;
  }
  EXPECT_TRUE(RE2::PartialMatch(emitted, R"(enum logic signed \[2:0\])"))
      << emitted;
  EXPECT_LT(emitted.find("logic [7:0] hi;"), emitted.find("logic [7:0] lo;"))
      << emitted;
  EXPECT_EQ(emitted.find("Message_tag_BYTE"), std::string::npos) << emitted;

  const size_t view_end = emitted.find("} Collision_padding_view_t;");
  ASSERT_NE(view_end, std::string::npos) << emitted;
  const size_t view_start = emitted.rfind("typedef struct packed {", view_end);
  ASSERT_NE(view_start, std::string::npos) << emitted;
  const std::string view = emitted.substr(view_start, view_end - view_start);
  EXPECT_EQ(CountOccurrences(view, " xls_padding;"), 1) << view;
  EXPECT_EQ(CountOccurrences(view, "padding"), 2) << view;
}

// Verifies: Fixed union members cannot hide later package view typedefs.
// Catches: Renaming public as_* members or qualifying types before a collision.
TEST_F(DslxToVerilogTest, SumUnionMembersPreserveShadowedViewTypes) {
  for (bool shadowing_member_first : {false, true}) {
    SCOPED_TRACE(shadowing_member_first);
    const std::string_view program =
        shadowing_member_first ? "pub enum as_x { X_Y_View_T(u1), Y(u2) }"
                               : "pub enum as_x { Y(u2), X_Y_View_T(u1) }";
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinitions().front(), &import_data));
    const std::string emitted = manager.Emit();
    std::string members;
    ASSERT_TRUE(RE2::PartialMatch(
        emitted, R"(typedef union packed \{([^}]+)\} as_x_payload_t;)",
        &members))
        << emitted;
    EXPECT_NE(members.find("logic [1:0] bits;"), std::string::npos) << members;
    const size_t shadow =
        members.find("  as_x_x_y_view_t_view_t as_x_y_view_t;");
    const std::string_view referenced_type =
        shadowing_member_first ? "  test_pkg::as_x_y_view_t as_y;"
                               : "  as_x_y_view_t as_y;";
    const size_t referenced = members.find(referenced_type);
    ASSERT_NE(shadow, std::string::npos) << members;
    ASSERT_NE(referenced, std::string::npos) << members;
    EXPECT_EQ(shadow < referenced, shadowing_member_first) << members;
    EXPECT_EQ(CountOccurrences(members, "test_pkg::"), shadowing_member_first)
        << members;
  }
}

// Verifies: Tuples in reused ordinary sum payloads preserve type lookup.
// Catches: An earlier fixed index_0 field hiding the later package typedef.
TEST_F(DslxToVerilogTest, SumReusedOrdinaryStructProtectsNestedTupleTypeNames) {
  constexpr std::string_view program = R"(
pub struct index_0 { x: u8 }
pub struct Holder {
    nested: (u8, index_0),
    deep: (u8, (u8, index_0)[2]),
}
pub struct Standalone { nested: (u8, index_0) }
pub enum E { Item(Holder) }
pub enum Direct { Item((u8, index_0)) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  const TypeDefinition holder_type =
      tm.module->GetTypeDefinition("Holder").value();
  const TypeDefinition standalone_type =
      tm.module->GetTypeDefinition("Standalone").value();
  auto declaration = [](const std::string& emitted, std::string_view name) {
    const size_t end = emitted.find(absl::StrFormat("} %s;", name));
    const size_t begin = end == std::string::npos
                             ? std::string::npos
                             : emitted.rfind("typedef struct packed {", end);
    return begin == std::string::npos ? std::string{}
                                      : emitted.substr(begin, end - begin);
  };

  for (bool export_ordinary_first : {false, true}) {
    SCOPED_TRACE(export_ordinary_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    if (export_ordinary_first) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition("index_0").value(), &import_data));
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(holder_type, &import_data));
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(standalone_type, &import_data));
      const std::string ordinary = declaration(manager.Emit(), "Holder");
      EXPECT_EQ(CountOccurrences(ordinary, "test_pkg::index_0"), 0) << ordinary;
      EXPECT_EQ(CountOccurrences(ordinary, "index_0 index_1;"), 2) << ordinary;
    }
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinition("E").value(), &import_data));
    if (!export_ordinary_first) {
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(holder_type, &import_data));
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(standalone_type, &import_data));
    }

    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("} index_0;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("Holder value;"), std::string::npos) << emitted;
    const std::string holder = declaration(emitted, "Holder");
    EXPECT_TRUE(RE2::PartialMatch(
        holder,
        R"(logic \[7:0\] index_0;\s+test_pkg::index_0 index_1;\s*\} nested;)"))
        << holder;
    EXPECT_TRUE(RE2::PartialMatch(
        holder,
        R"(logic \[7:0\] index_0;\s+test_pkg::index_0 index_1;\s*\}\s*\[1:0\] index_1;\s*\} deep;)"))
        << holder;
    const std::string standalone = declaration(emitted, "Standalone");
    EXPECT_TRUE(RE2::PartialMatch(
        standalone, R"(logic \[7:0\] index_0;\s+index_0 index_1;)"))
        << standalone;

    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinition("Direct").value(), &import_data));
    const std::string direct =
        declaration(manager.Emit(), "Direct_item_view_t");
    EXPECT_TRUE(RE2::PartialMatch(
        direct, R"(logic \[7:0\] index_0;\s+test_pkg::index_0 index_1;)"))
        << direct;
  }
}

// Verifies: Sum payloads retain signed types without changing standalone enums.
// Catches: Lost signedness, duplicate enums, or rewrites of unsigned enums.
TEST_F(DslxToVerilogTest, SumScopedSignedTypesLeaveStandaloneEnumUnchanged) {
  constexpr std::string_view program = R"(
pub enum SignedCode: s8 { NEG = -1, ZERO = 0 }
pub enum UnsignedCode: u8 { Good = 0, Bad = 1 }
pub struct Record { item: s8, flag: s1 }
pub type Alias = Record;
pub enum Message {
  None, Scalar(s8), Bit(s1), Record(Alias), Tuple((s8, u8)), Array(s8[2]),
  Error(SignedCode), Status(UnsignedCode),
}
pub enum Second { None, Error(SignedCode) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(
        type_to_verilog.AddTypeForTypeDefinition(definition, &import_data));
  }

  const std::string emitted = type_to_verilog.Emit();
  EXPECT_TRUE(RE2::PartialMatch(
      emitted, R"((?s)enum logic \[7:0\]\s*\{\s*NEG = .*?\} SignedCode;)"))
      << emitted;
  EXPECT_TRUE(RE2::PartialMatch(
      emitted,
      R"((?s)enum logic signed \[7:0\]\s*\{\s*Message_SignedCode_enum_NEG = .*?\} Message_SignedCode_value_t;)"))
      << emitted;
  EXPECT_TRUE(RE2::PartialMatch(
      emitted,
      R"((?s)enum logic signed \[7:0\]\s*\{\s*Second_SignedCode_enum_NEG = .*?\} Second_SignedCode_value_t;)"))
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "Message_SignedCode_enum_NEG ="), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "Second_SignedCode_enum_NEG ="), 1)
      << emitted;
  for (std::string_view pattern :
       {R"(logic signed \[7:0\] value;)", R"(logic signed( \[0:0\])? value;)",
        R"(logic signed \[7:0\] item;)", R"(logic signed( \[0:0\])? flag;)",
        R"(logic signed \[7:0\] index_0;)",
        R"(Message_SignedCode_value_t value;)"}) {
    const RE2 regex{std::string(pattern)};
    EXPECT_TRUE(RE2::PartialMatch(emitted, regex)) << pattern << '\n'
                                                   << emitted;
  }
  EXPECT_NE(emitted.find("} Message_array_view_t;"), std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("logic [7:0] item;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("} Message_Record_value_t;"), std::string::npos)
      << emitted;
  EXPECT_NE(
      emitted.find("Message_make_record (input Message_Record_value_t value)"),
      std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("UnsignedCode value;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("Message_make_status (input UnsignedCode value)"),
            std::string::npos)
      << emitted;
  EXPECT_EQ(emitted.find("Message_UnsignedCode_value_t"), std::string::npos)
      << emitted;
}

void CheckPrivateSameValuedPayloadEnum(std::string_view program,
                                       bool is_signed) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data));
  const std::string emitted = manager.Emit();
  ::testing::Test::RecordProperty("generated_systemverilog", emitted);
  const std::string prefix = is_signed ? "Message_E_enum_" : "";
  const std::string type = is_signed ? "Message_E_value_t" : "E";
  const std::string base = is_signed ? "logic signed" : "logic";
  const std::string first = is_signed ? "2'h3" : "2'h0";
  const RE2 enumeration("(?s)typedef enum " + base + " \\[1:0\\]\\s*\\{\\s*" +
                        prefix + "A = " + first + ",\\s*" + prefix +
                        "C = 2'h1\\s*\\} " + type + ";");
  EXPECT_TRUE(RE2::PartialMatch(emitted, enumeration)) << emitted;
  EXPECT_NE(
      emitted.find("parameter " + type + " " + prefix + "B = " + prefix + "A;"),
      std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find(type + " value;"), std::string::npos) << emitted;
  if (is_signed) {
    EXPECT_EQ(emitted.find("} E;"), std::string::npos) << emitted;
  } else {
    EXPECT_EQ(emitted.find("Message_E_value_t"), std::string::npos) << emitted;
  }
}

// Verifies: A private unsigned payload retains each name and the ordinary enum.
// Catches: Duplicate native values or a replacement that changes nominal type.
TEST_F(DslxToVerilogTest, PrivateUnsignedSameValuedPayloadEnum) {
  CheckPrivateSameValuedPayloadEnum(R"(
enum E: u2 { A = 0, B = 0, C = 1 }
pub enum Message { None, Value(E) }
)",
                                    /*is_signed=*/false);
}

// Verifies: A private signed payload retains each name in its signed companion.
// Catches: Duplicate native values or leaking the private legacy declaration.
TEST_F(DslxToVerilogTest, PrivateSignedSameValuedPayloadEnum) {
  CheckPrivateSameValuedPayloadEnum(R"(
enum E: s2 { A = -1, B = -1, C = 1 }
pub enum Message { None, Value(E) }
)",
                                    /*is_signed=*/true);
}

// Verifies: The same ordinary enum is legalized before any consumer in either
// export order, with distinct signed companions and unchanged ordinary aliases.
// Catches: Late aliases, merging signed companions, or changing standalone E.
TEST_F(DslxToVerilogTest, SameValuedOrdinaryPayloadEnumInEitherExportOrder) {
  for (bool is_signed : {false, true}) {
    SCOPED_TRACE(is_signed);
    const std::string program =
        absl::StrFormat(R"(
pub enum E: %s { A = %s, B = %s, C = 1, D = 1, Z = %s }
pub type EAlias = E;
pub enum Message { None, Value(E) }
pub enum Second { None, Value(E) }
)",
                        is_signed ? "s2" : "u2", is_signed ? "-1" : "0",
                        is_signed ? "-1" : "0", is_signed ? "-1" : "0");
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    for (bool sum_first : {false, true}) {
      SCOPED_TRACE(sum_first);
      XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                               DslxTypeToVerilogManager::Create("test_pkg"));
      for (std::string_view name :
           sum_first ? std::vector<std::string_view>{"Message", "Second", "E",
                                                     "EAlias"}
                     : std::vector<std::string_view>{"E", "EAlias", "Message",
                                                     "Second"}) {
        XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
            tm.module->GetTypeDefinition(name).value(), &import_data));
      }
      const std::string emitted = manager.Emit();
      const std::string bits = is_signed ? "2'h3" : "2'h0";
      const RE2 ordinary("(?s)typedef enum logic \\[1:0\\]\\s*\\{\\s*A = " +
                         bits + ",\\s*C = 2'h1\\s*\\} E;");
      EXPECT_TRUE(RE2::PartialMatch(emitted, ordinary)) << emitted;
      for (const auto& [alias, original] :
           {std::pair<std::string_view, std::string_view>{"B", "A"},
            {"D", "C"},
            {"Z", "A"}}) {
        const std::string declaration =
            absl::StrFormat("parameter E %s = %s;", alias, original);
        EXPECT_EQ(CountOccurrences(emitted, declaration), 1) << emitted;
        EXPECT_LT(emitted.find("} E;"), emitted.find(declaration)) << emitted;
        EXPECT_LT(emitted.find(declaration), emitted.find("typedef E EAlias;"))
            << emitted;
        if (is_signed) {
          for (std::string_view family : {"Message", "Second"}) {
            const std::string companion = absl::StrFormat(
                "parameter %s_E_value_t %s_E_enum_%s = %s_E_enum_%s;", family,
                family, alias, family, original);
            EXPECT_EQ(CountOccurrences(emitted, companion), 1) << emitted;
          }
        }
      }
      if (is_signed) {
        EXPECT_NE(emitted.find("Message_E_value_t value;"), std::string::npos)
            << emitted;
        EXPECT_NE(emitted.find("Second_E_value_t value;"), std::string::npos)
            << emitted;
      } else {
        EXPECT_EQ(CountOccurrences(emitted, " E value;"), 2) << emitted;
        EXPECT_EQ(emitted.find("_E_value_t"), std::string::npos) << emitted;
      }
    }
  }
}

// Verifies: Renaming either side of an unsigned synonym updates the declaration
// and its value reference, even after the sum has already emitted the enum.
TEST_F(DslxToVerilogTest, SameValuedPayloadEnumTracksPackageNameCollisions) {
  constexpr std::string_view program = R"(
enum E: u2 { A = 0, B = 0, C = 1 }
pub enum Message { None, Value(E) }
fn identity(value: u1) -> u1 { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* identity = tm.module->GetFunction("identity").value();
  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    if (sum_first) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinitions().back(), &import_data));
    }
    XLS_ASSERT_OK(
        manager.AddTypeForFunctionParam(identity, &import_data, "value", "A"));
    XLS_ASSERT_OK(
        manager.AddTypeForFunctionParam(identity, &import_data, "value", "B"));
    if (!sum_first) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinitions().back(), &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("E_A = 2'h0"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("parameter E E_B = E_A;"), std::string::npos)
        << emitted;
    EXPECT_EQ(emitted.find("parameter E B ="), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find(" E_B = A;"), std::string::npos) << emitted;
  }
}

// Verifies: An unrelated sum does not rewrite historically exported ordinary
// enums, even when those standalone declarations have duplicate native values.
TEST_F(DslxToVerilogTest, UnrelatedSumPreservesSameValuedOrdinaryEnums) {
  constexpr std::string_view program = R"(
pub enum Unsigned: u2 { UA = 0, UB = 0 }
pub enum Signed: s2 { SA = -1, SB = -1 }
pub enum Message { None, Value(u2) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    for (std::string_view name :
         sum_first
             ? std::vector<std::string_view>{"Message", "Unsigned", "Signed"}
             : std::vector<std::string_view>{"Unsigned", "Signed", "Message"}) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition(name).value(), &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_TRUE(RE2::PartialMatch(
        emitted, R"((?s)UA = 2'h0,\s*UB = 2'h0\s*\} Unsigned;)"))
        << emitted;
    EXPECT_TRUE(RE2::PartialMatch(emitted,
                                  R"((?s)SA = 2'h3,\s*SB = 2'h3\s*\} Signed;)"))
        << emitted;
    EXPECT_EQ(emitted.find("parameter"), std::string::npos) << emitted;
  }
}

// Verifies: Sum names avoid package symbols and SystemVerilog keywords.
// Catches: Entry-order-dependent naming and collisions after case conversion.
TEST_F(DslxToVerilogTest,
       SumNamesRespectPackageSymbolsAndSystemVerilogKeywords) {
  constexpr std::string_view program = R"(
pub enum Message {
  Item { byte: u8, wire: u8 },
  FooBar(u8),
  Foo_Bar(u8),
}
pub type Message_tag_t = u16;
pub type Message_get_tag = u8;
pub enum Symbols: u1 { Message_tag_Item = 0 }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  for (bool sums_first : {false, true}) {
    SCOPED_TRACE(sums_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    std::vector<TypeDefinition> definitions = tm.module->GetTypeDefinitions();
    if (!sums_first) {
      std::reverse(definitions.begin(), definitions.end());
    }
    for (const TypeDefinition& definition : definitions) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("} Message_tag_t__1;"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_get_tag__1 ("), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_tag_Item__1 = 2'h0"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_tag_Item = 1'h0"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("logic [15:0] Message_tag_t;"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("logic [7:0] byte_;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("logic [7:0] wire_;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find(" as_foo_bar;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find(" as_foo_bar__1;"), std::string::npos) << emitted;
  }
}

// Verifies: Tag-only and inferred-singleton sums omit unneeded storage.
// Catches: Extra payload or tag storage and dropped explicit singleton tags.
TEST_F(DslxToVerilogTest, TagOnlyAndInferredSingletonHaveNoFakeStorage) {
  constexpr std::string_view program = R"(
pub enum TagOnly: u3 { Empty() = 1, EmptyRecord {} = 5 }
pub enum Singleton { Only(u8) }
pub enum ExplicitSingleton: u3 { Only(u8) = 5 }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(
        type_to_verilog.AddTypeForTypeDefinition(definition, &import_data));
  }

  const std::string emitted = type_to_verilog.Emit();
  EXPECT_TRUE(RE2::PartialMatch(
      emitted, R"((?s)struct packed \{\s*TagOnly_tag_t tag;\s*\} TagOnly;)"))
      << emitted;
  EXPECT_EQ(emitted.find("TagOnly_payload_t"), std::string::npos) << emitted;
  EXPECT_EQ(emitted.find("TagOnly_empty_view_t"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("TagOnly_make_empty ("), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("TagOnly_make_empty_record ("), std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("} Singleton_tag_t;"), std::string::npos) << emitted;
  EXPECT_EQ(emitted.find(" Singleton_tag_t tag;"), std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("Singleton_get_tag ("), std::string::npos) << emitted;
  EXPECT_NE(
      emitted.find("function automatic Singleton_tag_t Singleton_get_tag"),
      std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("ExplicitSingleton_tag_t tag;"), std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("ExplicitSingleton_tag_Only = 3'h5"),
            std::string::npos)
      << emitted;
}

// Verifies: Imported same-named sums keep distinct names across export orders.
// Catches: Merged declarations, duplicate helpers, or order-dependent aliases.
TEST_F(DslxToVerilogTest, ImportedSameNamedSumsHaveStableDistinctFamilies) {
  constexpr std::string_view program = R"(
import a;
import b;
pub type Left = a::Duplicate;
pub type Right = b::Duplicate;
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files;
  files[std::filesystem::path("/a.x")] =
      "pub enum Duplicate { Empty, Item(u8) }";
  files[std::filesystem::path("/b.x")] =
      "pub enum Duplicate { Empty, Item(u16) }";
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  const std::vector<TypeDefinition> definitions =
      tm.module->GetTypeDefinitions();
  ASSERT_EQ(definitions.size(), 2);

  std::set<std::string> forward_families;
  std::pair<std::string, std::string> forward_aliases;
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(
        definitions[reverse ? 1 : 0], &import_data));
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(
        definitions[reverse ? 0 : 1], &import_data));
    const std::string emitted = type_to_verilog.Emit();
    constexpr std::string_view suffix = "_tag_t;";
    std::set<std::string> families;
    for (size_t end = emitted.find(suffix); end != std::string::npos;
         end = emitted.find(suffix, end + suffix.size())) {
      const size_t line_start = emitted.rfind('\n', end);
      const size_t closure = emitted.rfind("} ", end);
      if (closure != std::string::npos &&
          (line_start == std::string::npos || closure > line_start)) {
        families.insert(emitted.substr(closure + 2, end - closure - 2));
      }
    }
    ASSERT_EQ(families.size(), 2) << emitted;
    for (const std::string& family : families) {
      EXPECT_EQ(CountOccurrences(emitted, family + "_get_tag ("), 1) << emitted;
      EXPECT_EQ(CountOccurrences(emitted, family + "_make_item ("), 1)
          << emitted;
    }
    std::pair<std::string, std::string> aliases;
    ASSERT_TRUE(
        RE2::PartialMatch(emitted, R"(typedef ([^ ]+) Left;)", &aliases.first))
        << emitted;
    ASSERT_TRUE(RE2::PartialMatch(emitted, R"(typedef ([^ ]+) Right;)",
                                  &aliases.second))
        << emitted;
    EXPECT_EQ(aliases.first, "a_Duplicate") << emitted;
    EXPECT_EQ(aliases.second, "b_Duplicate") << emitted;
    EXPECT_EQ(emitted.find("Left_get_tag"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("Right_get_tag"), std::string::npos) << emitted;
    if (reverse) {
      EXPECT_EQ(families, forward_families) << emitted;
      EXPECT_EQ(aliases, forward_aliases) << emitted;
    } else {
      forward_families = std::move(families);
      forward_aliases = std::move(aliases);
    }
  }
}

// Verifies: Explicit source spellings name an alias when sums are qualified.
// Catches: Treating an explicitly requested spelling as an omitted name.
TEST_F(DslxToVerilogTest, ExplicitImportedSumSourceNameIsUsedOrRejected) {
  constexpr std::string_view program = R"(
import a;
import b;
pub type Left = a::Duplicate;
pub type Right = b::Duplicate;
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files;
  files[std::filesystem::path("/a.x")] = R"(
pub enum Duplicate { Empty, Item(u8) }
pub fn identity(value: Duplicate) -> Duplicate { value }
)";
  files[std::filesystem::path("/b.x")] = R"(
pub enum Duplicate { Empty, Item(u16) }
pub fn identity(value: Duplicate) -> Duplicate { value }
)";
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * a,
                           import_data.Get(ImportTokens({"a"})));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * b,
                           import_data.Get(ImportTokens({"b"})));

  enum class Export { kDefinition, kParameter, kOutput };
  for (Export entry :
       {Export::kDefinition, Export::kParameter, Export::kOutput}) {
    SCOPED_TRACE(static_cast<int>(entry));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    manager.PrepareForModules({{tm.module, tm.type_info}});
    auto add = [&](Module* module, std::optional<std::string_view> alias) {
      if (entry == Export::kDefinition) {
        return manager.AddTypeForTypeDefinition(
            module->GetTypeDefinitions().front(), &import_data, alias);
      } else if (entry == Export::kParameter) {
        return manager.AddTypeForFunctionParam(
            module->GetFunction("identity").value(), &import_data, "value",
            alias);
      } else {
        return manager.AddTypeForFunctionOutput(
            module->GetFunction("identity").value(), &import_data, alias);
      }
    };

    XLS_ASSERT_OK(add(&a->module(), std::nullopt));
    XLS_ASSERT_OK(add(&b->module(), std::nullopt));
    const std::string canonical = manager.Emit();
    EXPECT_EQ(CountOccurrences(canonical, "} a_Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(canonical, "} b_Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(canonical, " Duplicate;"), 0);

    XLS_ASSERT_OK(add(&a->module(), "Duplicate"));
    XLS_ASSERT_OK(add(&a->module(), "Duplicate"));
    const std::string aliased = manager.Emit();
    EXPECT_EQ(CountOccurrences(aliased, "typedef a_Duplicate Duplicate;"), 1)
        << aliased;
    EXPECT_EQ(CountOccurrences(aliased, "} a_Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(aliased, "} b_Duplicate;"), 1);

    const auto conflict = add(&b->module(), "Duplicate");
    EXPECT_FALSE(conflict.ok()) << conflict;
    EXPECT_NE(conflict.message().find("Duplicate"), std::string_view::npos)
        << conflict;
    EXPECT_EQ(manager.Emit(), aliased);

    XLS_ASSERT_OK(add(&a->module(), ""));
    EXPECT_EQ(CountOccurrences(manager.Emit(), "typedef a_Duplicate _;"), 1);

    if (entry == Export::kDefinition) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition("Left").value(), &import_data));
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition("Right").value(), &import_data));
      const std::string source_aliases = manager.Emit();
      EXPECT_EQ(CountOccurrences(source_aliases, "typedef a_Duplicate Left;"),
                1)
          << source_aliases;
      EXPECT_EQ(CountOccurrences(source_aliases, "typedef b_Duplicate Right;"),
                1)
          << source_aliases;
      EXPECT_EQ(CountOccurrences(source_aliases, "} a_Duplicate;"), 1);
      EXPECT_EQ(CountOccurrences(source_aliases, "} b_Duplicate;"), 1);
    }
  }
}

// Verifies: Concrete sum types get distinct names regardless of export order.
// Catches: Names tied to export order or repeated exports creating new types.
TEST_F(DslxToVerilogTest, ConcreteSumSpecializationsKeepNamesAcrossEntryOrder) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Option<N: u32> { None, Some(uN[N]) }
fn narrow(value: Option<u32:8>) -> Option<u32:8> { value }
fn wide(value: Option<u32:16>) -> Option<u32:16> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* narrow = tm.module->GetFunction("narrow").value();
  Function* wide = tm.module->GetFunction("wide").value();
  std::pair<std::string, std::string> expected;
  for (bool narrow_first : {true, false}) {
    SCOPED_TRACE(narrow_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    auto add = [&](Function* function, std::string_view alias) {
      XLS_EXPECT_OK(manager.AddTypeForFunctionParam(function, &import_data,
                                                    "value", alias));
    };
    if (narrow_first) {
      add(narrow, "Narrow");
      add(wide, "Wide");
    } else {
      add(wide, "Wide");
      add(narrow, "Narrow");
    }
    XLS_ASSERT_OK(
        manager.AddTypeForFunctionOutput(narrow, &import_data, "Again"));
    const std::string emitted = manager.Emit();
    std::pair<std::string, std::string> actual;
    std::string repeated;
    ASSERT_TRUE(
        RE2::PartialMatch(emitted, R"(typedef ([^ ]+) Narrow;)", &actual.first))
        << emitted;
    ASSERT_TRUE(
        RE2::PartialMatch(emitted, R"(typedef ([^ ]+) Wide;)", &actual.second))
        << emitted;
    ASSERT_TRUE(
        RE2::PartialMatch(emitted, R"(typedef ([^ ]+) Again;)", &repeated))
        << emitted;
    EXPECT_NE(actual.first, actual.second);
    EXPECT_EQ(actual.first, repeated);
    if (narrow_first) {
      expected = actual;
    } else {
      EXPECT_EQ(actual, expected);
    }
  }
}

// Verifies: Nominal arguments with equal layouts still name distinct sum types.
// Catches: Discarding a struct's phantom argument, even inside a tuple.
TEST_F(DslxToVerilogTest, SumNamesDistinguishPhantomStructArguments) {
  constexpr std::string_view program = R"(#![feature(generics)]
struct Phantom<N: u32> { value: u8 }
enum Box<T: type> { Value(T) }
pub type One = Box<Phantom<u32:1>>;
pub type Two = Box<Phantom<u32:2>>;
pub type TupleOne = Box<(Phantom<u32:1>, u1)>;
pub type TupleTwo = Box<(Phantom<u32:2>, u1)>;
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (std::string_view alias : {"One", "Two", "TupleOne", "TupleTwo"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
  }
  const std::string emitted = manager.Emit();
  std::set<std::string> families;
  for (std::string_view alias : {"One", "Two", "TupleOne", "TupleTwo"}) {
    std::string family;
    ASSERT_TRUE(RE2::PartialMatch(
        emitted, absl::StrFormat("typedef ([A-Za-z_][A-Za-z_0-9]*) %s;", alias),
        &family))
        << alias;
    EXPECT_TRUE(families.insert(family).second) << alias << ": " << family;
    EXPECT_EQ(CountOccurrences(emitted, "} " + family + "_tag_t;"), 1);
  }
}

// Verifies: A phantom channel's direction distinguishes otherwise equal sums.
// Catches: Rejecting or discarding a channel used only as a type argument.
TEST_F(DslxToVerilogTest, SumNamesDistinguishPhantomChannelDirections) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Marker<T: type> { Empty, Only(u1) }
pub type Receive = Marker<chan<u8> in>;
pub type Send = Marker<chan<u8> out>;
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (std::string_view alias : {"Receive", "Send"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
  }
  const std::string emitted = manager.Emit();
  std::string receive_family;
  std::string send_family;
  ASSERT_TRUE(RE2::PartialMatch(emitted,
                                R"(typedef ([A-Za-z_][A-Za-z_0-9]*) Receive;)",
                                &receive_family));
  ASSERT_TRUE(RE2::PartialMatch(
      emitted, R"(typedef ([A-Za-z_][A-Za-z_0-9]*) Send;)", &send_family));
  EXPECT_NE(receive_family, send_family);
  EXPECT_EQ(CountOccurrences(emitted, "} " + receive_family + "_tag_t;"), 1);
  EXPECT_EQ(CountOccurrences(emitted, "} " + send_family + "_tag_t;"), 1);
}

// Verifies: Unused proc type arguments select stable families for packed sums.
// Catches: Rejecting an alias or merging proc declarations, arguments or
// tuples.
TEST_F(DslxToVerilogTest, SumNamesDistinguishPhantomProcTypes) {
  constexpr std::string_view program = R"(#![feature(explicit_state_access)]
#![feature(generics)]
proc P { state: u1, }
impl P {
  fn new() -> Self { P { state: u1:0 } }
  fn next(self) { () }
}
proc Q { state: u1, }
impl Q {
  fn new() -> Self { Q { state: u1:0 } }
  fn next(self) { () }
}
proc Phantom<N: u32 = {u32:1}, T: type = u8> {}
type ProcAlias = P;
type SameProc = P;
type OtherProc = Q;
type GenericFirst = Phantom<u32:1, u8>;
type GenericDefault = Phantom;
type GenericValue = Phantom<u32:2, u8>;
type GenericType = Phantom<u32:1, u16>;
enum Marker<T: type> { Empty, Only(u1) }
pub type First = Marker<ProcAlias>;
pub type Repeated = Marker<SameProc>;
pub type Other = Marker<OtherProc>;
pub type Nested = Marker<(P,)>;
pub type Binding = Marker<GenericFirst>;
pub type Defaulted = Marker<GenericDefault>;
pub type OtherValue = Marker<GenericValue>;
pub type OtherType = Marker<GenericType>;
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  std::vector<std::string> families;
  for (std::string_view alias :
       {"First", "Repeated", "Other", "Nested", "Binding", "Defaulted",
        "OtherValue", "OtherType"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    std::string family;
    ASSERT_TRUE(RE2::PartialMatch(
        manager.Emit(),
        absl::StrFormat("typedef ([A-Za-z_][A-Za-z_0-9]*) %s;", alias),
        &family))
        << alias;
    families.push_back(std::move(family));
  }
  EXPECT_EQ(families[0], families[1]);
  EXPECT_EQ(families[4], families[5]);
  EXPECT_EQ((std::set<std::string>(families.begin(), families.end())).size(),
            6);
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} " + families[0] + "_tag_t;"),
            1);
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} " + families[4] + "_tag_t;"),
            1);
}

// Negative test: Opaque tokens in a value argument must not produce names that
// depend on a process address or merge distinct compiler specializations.
TEST_F(DslxToVerilogTest, SumNamesRejectTokenBearingValueArguments) {
  constexpr std::string_view program = R"(#![feature(generics)]
struct V { t: token }
enum Marker<A: V> { Empty, Only(u1) }
const FIRST = V { t: token() };
const SECOND = V { t: token() };
pub type One = Marker<FIRST>;
pub type Two = Marker<SECOND>;
enum TypeMarker<T: type> { Empty, Only(u1) }
pub type TokenType = TypeMarker<token>;
pub type Ordinary = u8;
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition ordinary,
                           tm.module->GetTypeDefinition("Ordinary"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(ordinary, &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition token_type,
                           tm.module->GetTypeDefinition("TokenType"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(token_type, &import_data));
  const std::string before = manager.Emit();
  EXPECT_TRUE(RE2::PartialMatch(
      before, R"(typedef [A-Za-z_][A-Za-z_0-9]* TokenType;)"));
  for (std::string_view alias : {"One", "Two"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    const auto status =
        manager.AddTypeForTypeDefinition(definition, &import_data);
    EXPECT_FALSE(status.ok());
    EXPECT_NE(status.message().find("value argument contains a token"),
              std::string_view::npos);
    EXPECT_EQ(manager.Emit(), before);
  }
}

// Verifies: Equal aggregate values name the same sum in isolation and together.
// Catches: A stored range, its empty bounds, or alias order changing the API.
TEST_F(DslxToVerilogTest, SumNamesCanonicalizeEqualAggregateValues) {
  constexpr std::string_view kPreamble = R"(#![feature(generics)]
struct Values { xs: u32[2] }
enum Marker<V: Values> { Empty, Only(u1) }
const RANGE = Values { xs: u32:0..u32:2 };
const ARRAY = Values { xs: [u32:0, u32:1] };
const INCLUSIVE = Values { xs: u32:0..=u32:1 };
)";
  constexpr std::string_view kRange = "pub type RangeOutput = Marker<RANGE>;\n";
  constexpr std::string_view kArray = "pub type ArrayOutput = Marker<ARRAY>;\n";
  constexpr std::string_view kInclusive =
      "pub type InclusiveOutput = Marker<INCLUSIVE>;\n";
  constexpr std::string_view kEmptyPreamble = R"(#![feature(generics)]
struct Values { xs: u32[0] }
enum Marker<V: Values> { Empty, Only(u1) }
const EAGER = Values { xs: u32[0]:[] };
const ZERO = Values { xs: u32:0..u32:0 };
const ONE = Values { xs: u32:1..u32:1 };
)";
  constexpr std::string_view kEager = "pub type Eager = Marker<EAGER>;\n";
  constexpr std::string_view kZero = "pub type Zero = Marker<ZERO>;\n";
  constexpr std::string_view kOne = "pub type One = Marker<ONE>;\n";
  struct TestCase {
    std::string_view preamble;
    std::string aliases_source;
    std::vector<std::string_view> aliases;
  };
  const std::vector<TestCase> cases = {
      {kPreamble, std::string(kRange), {"RangeOutput"}},
      {kPreamble, std::string(kArray), {"ArrayOutput"}},
      {kPreamble, std::string(kInclusive), {"InclusiveOutput"}},
      {kPreamble,
       std::string(kRange) + std::string(kArray),
       {"RangeOutput", "ArrayOutput"}},
      {kPreamble,
       std::string(kArray) + std::string(kRange),
       {"ArrayOutput", "RangeOutput"}},
      {kPreamble,
       std::string(kInclusive) + std::string(kRange) + std::string(kArray),
       {"InclusiveOutput", "RangeOutput", "ArrayOutput"}},
      {kEmptyPreamble,
       std::string(kEager) + std::string(kZero) + std::string(kOne),
       {"Eager", "Zero", "One"}},
      {kEmptyPreamble,
       std::string(kZero) + std::string(kOne) + std::string(kEager),
       {"Zero", "One", "Eager"}}};
  absl::flat_hash_map<std::string_view, std::string> canonical_families;
  for (const TestCase& test_case : cases) {
    SCOPED_TRACE(test_case.aliases_source);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(
            std::string(test_case.preamble) + test_case.aliases_source,
            "test_module.x", "test_module", &import_data, nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    for (std::string_view alias : test_case.aliases) {
      XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                               tm.module->GetTypeDefinition(alias));
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    }
    const std::string emitted = manager.Emit();
    for (std::string_view alias : test_case.aliases) {
      std::string family;
      ASSERT_TRUE(RE2::PartialMatch(
          emitted,
          absl::StrFormat("typedef ([A-Za-z_][A-Za-z_0-9]*) %s;", alias),
          &family));
      const auto canonical =
          canonical_families.try_emplace(test_case.preamble, family).first;
      EXPECT_EQ(family, canonical->second) << alias;
      EXPECT_EQ(CountOccurrences(emitted, "} " + family + "_tag_t;"), 1);
    }
  }
}

// Verifies: Reusing nested types keeps identifiers and the package manageable.
// Catches: Duplicating the entire argument name at every nesting level.
TEST_F(DslxToVerilogTest, SumNamesAndPackageStayBoundedForRepeatedNestedTypes) {
  for (int depth : {4, 8, 12}) {
    SCOPED_TRACE(depth);
    std::string program = R"(#![feature(generics)]
enum Pair<T: type, U: type> { Left(T), Right(U) }
type A0 = u8;
)";
    for (int level = 1; level <= depth; ++level) {
      program += absl::StrFormat("type A%d = Pair<A%d, A%d>;\n", level,
                                 level - 1, level - 1);
    }
    program += absl::StrFormat("pub type Out = A%d;\n", depth);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition("Out"));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    const std::string emitted = manager.Emit();
    ASSERT_TRUE(
        RE2::PartialMatch(emitted, R"(typedef [A-Za-z_][A-Za-z_0-9]* Out;)"));

    size_t longest_token = 0;
    size_t token_size = 0;
    for (char c : emitted) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '$') {
        longest_token = std::max(longest_token, ++token_size);
      } else {
        token_size = 0;
      }
    }
    // Generous bounds allow formatting changes while rejecting exponential
    // identifiers and total output for a source with only `depth` unique sums.
    EXPECT_LE(longest_token, 512);
    ASSERT_LE(emitted.size(), 16 * 1024 * depth);
  }
}

// Verifies: Nested arrays keep their source dimensions and signed leaf type.
// Catches: Equal-width arrays with transposed SystemVerilog indexing.
TEST_F(DslxToVerilogTest, SumNestedArrayDimensionsAreOutermostFirst) {
  constexpr std::string_view program = R"(
pub enum Matrix { Unsigned(u8[2][3]), Signed(s8[2][3]) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinitions().front(), &import_data));
  const std::string emitted = manager.Emit();
  EXPECT_NE(emitted.find("logic [2:0][1:0][7:0] value;"), std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("Matrix_s8_value_t [2:0][1:0] value;"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("input logic [2:0][1:0][7:0] value"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find("input Matrix_s8_value_t [2:0][1:0] value"),
            std::string::npos)
      << emitted;
}

// Verifies: Unsigned payload types handle SV keywords and enum name collisions.
// Catches: Invalid HDL generated for otherwise valid private payload types.
TEST_F(DslxToVerilogTest, SumUnsignedNominalsSanitizeAmbiguousMembers) {
  constexpr std::string_view program = R"(
struct Record { byte: u8, wire: u8 }
enum Keywords: u8 { byte = 0, wire = 1 }
enum Left: u1 { Common = 0 }
enum Right: u1 { Common = 0 }
pub enum Message { Named(Record), Keyword(Keywords), L(Left), R(Right) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinitions().back(), &import_data));
  const std::string emitted = manager.Emit();
  EXPECT_NE(emitted.find("logic [7:0] byte_;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("logic [7:0] wire_;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("byte_ = 8'h00"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("wire_ = 8'h01"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("Left_Common = 1'h0"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("Right_Common = 1'h0"), std::string::npos) << emitted;
  EXPECT_FALSE(RE2::PartialMatch(emitted, R"(\n\s+Common = )")) << emitted;
}

// Verifies: A later sum can reclaim its canonical spelling from a payload enum.
// Catches: Failing to refresh existing enum names for a primitive-payload sum.
TEST_F(DslxToVerilogTest, SumReallocatesOccupiedPayloadEnumMember) {
  constexpr std::string_view program = R"(
enum E: u1 { Item = 0 }
pub enum Carrier { V(E) }
pub enum E_Item__1 { V(u1) }
fn identity(value: u1) -> u1 { value }
)";
  for (bool independent_sum_first : {false, true}) {
    SCOPED_TRACE(independent_sum_first);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    const std::vector<TypeDefinition> definitions =
        tm.module->GetTypeDefinitions();
    ASSERT_EQ(definitions.size(), 3);
    if (independent_sum_first) {
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(definitions[2], &import_data));
    }
    XLS_ASSERT_OK(
        manager.AddTypeForTypeDefinition(definitions[1], &import_data));
    ASSERT_TRUE(tm.module->GetFunction("identity").has_value());
    Function* identity = tm.module->GetFunction("identity").value();
    XLS_ASSERT_OK(manager.AddTypeForFunctionParam(identity, &import_data,
                                                  "value", "Item"));
    XLS_ASSERT_OK(manager.AddTypeForFunctionParam(identity, &import_data,
                                                  "value", "E_Item"));
    if (!independent_sum_first) {
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(definitions[2], &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("} E_Item__1;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("E_Item__2 = 1'h0"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("E_Item__1 ="), std::string::npos) << emitted;
  }
}

// Verifies: an unrelated enum keeps its source spelling in either export order.
// Catches: missed refreshes or displacement of the wrong shared-name literal.
TEST_F(DslxToVerilogTest, SumPayloadEnumDefersToUnrelatedOrdinaryEnum) {
  constexpr std::string_view program = R"(
enum Payload: u1 { Common = 0 }
pub enum Unrelated: u1 { Common = 1 }
pub enum Message { V(Payload) }
)";
  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    const std::vector<TypeDefinition> definitions =
        tm.module->GetTypeDefinitions();
    ASSERT_EQ(definitions.size(), 3);
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        definitions[sum_first ? 2 : 1], &import_data));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        definitions[sum_first ? 1 : 2], &import_data));
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("Payload_Common = 1'h0"), std::string::npos)
        << emitted;
    EXPECT_TRUE(RE2::PartialMatch(emitted, R"(\n\s+Common = 1'h1)")) << emitted;
    EXPECT_EQ(emitted.find("Unrelated_Common"), std::string::npos) << emitted;
  }
}

// Verifies: Adding an unrelated sum does not alter historical ordinary output.
// Catches: Applying sum-specific member cleanup to all package declarations.
TEST_F(DslxToVerilogTest, SumPreservesUnrelatedOrdinaryNominalNames) {
  constexpr std::string_view ordinary = R"(
pub struct Plain { byte: u8 }
pub enum PlainCode: u8 { wire = 0 }
pub enum Left: u1 { Common = 0 }
pub enum Right: u1 { Common = 0 }
fn first(value: u8) -> u8 { value }
fn second(value: u16) -> u16 { value }
)";
  constexpr std::string_view unrelated_sum = R"(
pub enum Message { None, Byte(u8) }
)";
  for (bool include_sum : {false, true}) {
    SCOPED_TRACE(include_sum);
    std::string program(ordinary);
    if (include_sum) {
      program += unrelated_sum;
    }
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    }
    Function* first = tm.module->GetFunction("first").value();
    Function* second = tm.module->GetFunction("second").value();
    XLS_ASSERT_OK(manager.AddTypeForFunctionParam(first, &import_data, "value",
                                                  "Shared"));
    XLS_ASSERT_OK(manager.AddTypeForFunctionParam(first, &import_data, "value",
                                                  "Shared"));
    XLS_ASSERT_OK(manager.AddTypeForFunctionParam(second, &import_data, "value",
                                                  "Shared"));
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("logic [7:0] byte;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("wire = 8'h00"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("logic [7:0] byte_;"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("wire_ = 8'h00"), std::string::npos) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "Common = 1'h0"), 2) << emitted;
    EXPECT_EQ(emitted.find("Left_Common"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("Right_Common"), std::string::npos) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] Shared;"), 2)
        << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "typedef logic [15:0] Shared;"), 1)
        << emitted;
  }
}

// Verifies: A sum repairs the same nominal even if it was already exported.
// Catches: Public type spelling depending on exporter call order.
TEST_F(DslxToVerilogTest, SumRepairsPreviouslyExportedNominalDependencies) {
  constexpr std::string_view program = R"(
pub struct Record { byte: u8 }
pub enum Code: u8 { wire = 0 }
pub enum Message { None, Pair((Record, Code)[2]) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    for (std::string_view name :
         sum_first
             ? std::vector<std::string_view>{"Message", "Record", "Code"}
             : std::vector<std::string_view>{"Record", "Code", "Message"}) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition(name).value(), &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("logic [7:0] byte_;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("wire_ = 8'h00"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("logic [7:0] byte;"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("wire = 8'h00"), std::string::npos) << emitted;
  }
}

// Verifies: Exporting only a function's sum-typed output repairs its payload.
// Catches: Name preparation implemented only for explicit sum declarations.
TEST_F(DslxToVerilogTest, FunctionSumRepairsItsNominalDependencies) {
  constexpr std::string_view program = R"(
struct Record { byte: u8 }
enum Message { None, Value(Record) }
fn identity(message: Message) -> Message { message }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinitions().front(), &import_data));
  std::optional<Function*> function = tm.module->GetFunction("identity");
  ASSERT_TRUE(function.has_value());
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(*function, &import_data));
  const std::string emitted = manager.Emit();
  EXPECT_NE(emitted.find("logic [7:0] byte_;"), std::string::npos) << emitted;
  EXPECT_EQ(emitted.find("logic [7:0] byte;"), std::string::npos) << emitted;
}

// Verifies: Reordering sums preserves ownership of tags and helpers.
// Catches: Assigning collision suffixes in emission order instead of by owner.
TEST_F(DslxToVerilogTest, SumGeneratedPackageNamesRetainTheirOwners) {
  const std::string first = R"(
pub enum Message: u2 { Empty = 0, tag_Item() = 1 }
pub enum A { Empty, BGetTag(u8) }
)";
  const std::string second = R"(
pub enum Message_tag: u2 { Empty = 0, Item() = 2 }
pub enum A_make_b { Empty, Item(u16) }
)";
  std::pair<std::string, std::string> forward_functions;
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    const std::string program = reverse ? second + first : first + second;
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
      XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("Message_tag_tag_Item = 2'h1"), std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_tag_tag_Item__1 = 2'h2"), std::string::npos)
        << emitted;
    std::pair<std::string, std::string> functions;
    ASSERT_TRUE(RE2::PartialMatch(
        emitted,
        R"(function automatic A ([A-Za-z0-9_]+) \(input logic \[7:0\] value\))",
        &functions.first))
        << emitted;
    ASSERT_TRUE(RE2::PartialMatch(
        emitted,
        R"(function automatic A_make_b_tag_t ([A-Za-z0-9_]+) \(input A_make_b value\))",
        &functions.second))
        << emitted;
    EXPECT_NE(functions.first, functions.second);
    if (reverse) {
      EXPECT_EQ(functions, forward_functions);
    } else {
      forward_functions = functions;
    }
  }
}

// Negative test: aliases that claim generated operations fail without emission.
TEST_F(DslxToVerilogTest, SumFunctionOutputAliasesCannotClaimOperations) {
  constexpr std::string_view program = R"(
pub enum A { Empty, BGetTag(u8) }
pub enum A_make_b { Empty, Item(u16) }
fn identity(x: A) -> A { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
  }
  const std::optional<Function*> identity = tm.module->GetFunction("identity");
  ASSERT_TRUE(identity.has_value());
  const std::string before = manager.Emit();

  for (std::string_view name : {"A_make_b_get_tag", "A_make_b_get_tag__1"}) {
    SCOPED_TRACE(name);
    ASSERT_EQ(CountOccurrences(before, absl::StrCat(" ", name, " (")), 1);
    const absl::Status status =
        manager.AddTypeForFunctionOutput(*identity, &import_data, name);
    EXPECT_FALSE(status.ok()) << status;
    EXPECT_NE(status.message().find(name), std::string_view::npos) << status;
    EXPECT_EQ(manager.Emit(), before);
  }
}

// Verifies: Signed cast names keep module identity after reordering.
// Catches: The first visited same-named payload stealing the other's cast type.
TEST_F(DslxToVerilogTest, SumSignedCompanionsRetainPayloadModuleIdentity) {
  const std::string first = "Left(a::Code) = 1,";
  const std::string second = "Right(b::Code) = 2,";
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    const std::string program = "import a; import b; pub enum Message: u2 { " +
                                (reverse ? second + first : first + second) +
                                " }";
    absl::flat_hash_map<std::filesystem::path, std::string> files;
    files[std::filesystem::path("/a.x")] =
        "pub enum Code: s8 { NEG = -1, ZERO = 0 }";
    files[std::filesystem::path("/b.x")] =
        "pub enum Code: s8 { NEG = -2, ZERO = 0 }";
    auto vfs =
        std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
    ImportData import_data = CreateImportDataForTest(std::move(vfs));
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                          nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinitions().front(), &import_data));
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("Message_a_Code_enum_NEG = 8'hff"),
              std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_b_Code_enum_NEG = 8'hfe"),
              std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_make_left (input Message_a_Code_value_t"),
              std::string::npos)
        << emitted;
    EXPECT_NE(emitted.find("Message_make_right (input Message_b_Code_value_t"),
              std::string::npos)
        << emitted;
  }
}

// Negative test: a reserved signed-array name rejects aliases without emission.
TEST_F(DslxToVerilogTest, SumAliasCannotTakeUnemittedSignedArrayCompanion) {
  constexpr std::string_view program = R"(
pub enum Message { Empty, Samples(s8[2]) }
pub enum Other { Empty, Item(u8) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  const std::string before = manager.Emit();

  const auto conflict = manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Other").value(), &import_data,
      "Message_s8_value_t");
  ASSERT_FALSE(conflict.ok()) << conflict;
  EXPECT_NE(conflict.message().find("Message_s8_value_t"),
            std::string_view::npos)
      << conflict;
  EXPECT_EQ(manager.Emit(), before);

  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data));
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} Message;"), 1);
}

// Verifies: Repeated aliases work for one sum and report conflicts for another.
// Catches: Repeating a conflict creating fresh suffixes in the package.
TEST_F(DslxToVerilogTest, SumConflictingExplicitAliasIsRejectedIdempotently) {
  constexpr std::string_view program = R"(
enum First { None, Item(u8) }
enum Second { None, Item(u16) }
fn first(value: First) -> First { value }
fn second(value: Second) -> Second { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  Function* first = tm.module->GetFunction("first").value();
  Function* second = tm.module->GetFunction("second").value();
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionParam(first, &import_data, "value", "Same"));
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionParam(first, &import_data, "value", "Same"));
  const auto conflict =
      manager.AddTypeForFunctionParam(second, &import_data, "value", "Same");
  EXPECT_FALSE(conflict.ok());
  const auto repeated =
      manager.AddTypeForFunctionParam(second, &import_data, "value", "Same");
  EXPECT_EQ(repeated, conflict);
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, " Same;"), 1) << emitted;
  EXPECT_EQ(emitted.find("Same__"), std::string::npos) << emitted;
}

TEST_F(DslxToVerilogTest, GenericOnlySumDoesNotReserveUnspecializedName) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
fn ordinary(value: u8) -> u8 { value }
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* ordinary = tm.module->GetFunction("ordinary").value();
  Function* concrete = tm.module->GetFunction("concrete").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(
      probe.AddTypeForFunctionOutput(concrete, &import_data, "Concrete"));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &canonical));
  ASSERT_NE(canonical, "Message");

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(ordinary, &import_data, "Message"));
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(concrete, &import_data, "Concrete"));
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] Message;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "typedef " + canonical + " Concrete;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "} " + canonical + ";"), 1) << emitted;
  XLS_ASSERT_OK(probe.AddTypeForFunctionParam(ordinary, &import_data, "value",
                                              "Message"));
  EXPECT_EQ(CountOccurrences(probe.Emit(), "typedef logic [7:0] Message;"), 1)
      << probe.Emit();
}

TEST_F(DslxToVerilogTest, RealSumSymbolsTakePriorityOverGenericNominalNames) {
  constexpr std::string_view generic_program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
fn ordinary(value: u8) -> u8 { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule generic,
      ParseAndTypecheck(generic_program, "a.x", "a", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule fixed,
      ParseAndTypecheck("enum Message { Empty, Item(u8) }", "b.x", "b",
                        &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule other,
      ParseAndTypecheck("enum Other { Empty, Item(u8) }", "other.x", "other",
                        &import_data, nullptr));
  Function* ordinary = generic.module->GetFunction("ordinary").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules(
      {{generic.module, generic.type_info}, {fixed.module, fixed.type_info}});
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(ordinary, &import_data, "a_Message"));
  const std::string before = manager.Emit();
  for (std::string_view symbol : {"b_Message", "b_Message_tag_t"}) {
    const auto rejected =
        manager.AddTypeForFunctionOutput(ordinary, &import_data, symbol);
    EXPECT_FALSE(rejected.ok()) << rejected;
    EXPECT_EQ(manager.Emit(), before);
  }
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      fixed.module->GetTypeDefinition("Message").value(), &import_data));
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} b_Message;"), 1)
      << manager.Emit();

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager aliased,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(aliased.AddTypeForTypeDefinition(
      other.module->GetTypeDefinition("Other").value(), &import_data,
      "Message"));
  aliased.PrepareForModules({{generic.module, generic.type_info}});
  const std::string before_conflict = aliased.Emit();
  ASSERT_EQ(CountOccurrences(before_conflict, "typedef Other Message;"), 1)
      << before_conflict;
  EXPECT_FALSE(
      aliased.AddTypeForFunctionOutput(ordinary, &import_data, "Message").ok());
  EXPECT_EQ(aliased.Emit(), before_conflict);
}

TEST_F(DslxToVerilogTest,
       OrdinaryTupleEnumLiteralConflictDoesNotEmitEarlierNestedSum) {
  constexpr std::string_view program = R"(
enum Outer { Empty, Item(u16) }
enum Inner { Empty, Item(u8) }
enum Code: u1 { TOKEN = 0 }
fn ordinary(value: (Inner, Code)) -> (Inner, Code) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Outer").value(), &import_data, "TOKEN"));
  const std::string before = manager.Emit();
  ASSERT_EQ(CountOccurrences(before, "typedef Outer TOKEN;"), 1) << before;
  ASSERT_EQ(before.find("} Inner;"), std::string::npos) << before;
  ASSERT_EQ(before.find("TOKEN ="), std::string::npos) << before;
  const auto rejected = manager.AddTypeForFunctionOutput(
      tm.module->GetFunction("ordinary").value(), &import_data, "TupleOut");
  EXPECT_FALSE(rejected.ok()) << rejected;
  EXPECT_NE(rejected.message().find("TOKEN"), std::string_view::npos)
      << rejected;
  EXPECT_EQ(manager.Emit(), before);
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Inner").value(), &import_data));
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} Inner;"), 1) << manager.Emit();
}

TEST_F(DslxToVerilogTest,
       NamedArrayOfSumRejectsGeneratedSpecializationAliasAtomically) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
pub type Batch = Message<u32:8>[2];
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
fn nested(value: Batch) -> Batch { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* nested = tm.module->GetFunction("nested").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(probe.AddTypeForFunctionOutput(
      tm.module->GetFunction("concrete").value(), &import_data, "Concrete"));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &canonical));

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  const std::string before = manager.Emit();
  const auto rejected =
      manager.AddTypeForFunctionOutput(nested, &import_data, canonical);
  EXPECT_FALSE(rejected.ok()) << rejected;
  EXPECT_NE(rejected.message().find(canonical), std::string_view::npos)
      << rejected;
  EXPECT_EQ(manager.Emit(), before);
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(nested, &import_data, "Allowed"));
  const std::string allowed = manager.Emit();
  EXPECT_EQ(CountOccurrences(allowed, "} " + canonical + ";"), 1) << allowed;
  EXPECT_EQ(CountOccurrences(allowed, " Allowed;"), 1) << allowed;
}

TEST_F(DslxToVerilogTest,
       CachedOrdinaryAliasesStillCheckNewDependenciesAndFixedSumNames) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
struct Existing { field: Message<u32:8> }
type ExistingAlias = Existing;
struct Later { field: Message<u32:16> }
fn existing(value: Existing) -> Existing { value }
fn existing_alias(value: ExistingAlias) -> ExistingAlias { value }
fn later(value: Later) -> Later { value }
fn later_sum(value: Message<u32:16>) -> Message<u32:16> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* existing = tm.module->GetFunction("existing").value();
  Function* existing_alias = tm.module->GetFunction("existing_alias").value();
  Function* later = tm.module->GetFunction("later").value();
  Function* later_sum = tm.module->GetFunction("later_sum").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(
      probe.AddTypeForFunctionOutput(later_sum, &import_data, "Concrete"));
  std::string later_name;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &later_name));

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(existing, &import_data, "First"));
  XLS_ASSERT_OK(manager.AddTypeForFunctionParam(existing, &import_data, "value",
                                                "Second"));
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(existing_alias, &import_data,
                                                 "AliasFirst"));
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(existing_alias, &import_data,
                                                 "AliasSecond"));
  const std::string before = manager.Emit();
  for (std::string_view alias :
       {"First", "Second", "AliasFirst", "AliasSecond"}) {
    EXPECT_EQ(CountOccurrences(before, " " + std::string(alias) + ";"), 1);
  }
  ASSERT_EQ(before.find("} " + later_name + ";"), std::string::npos);
  EXPECT_FALSE(
      manager.AddTypeForFunctionOutput(later, &import_data, later_name).ok());
  EXPECT_EQ(manager.Emit(), before);
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(later, &import_data, "LaterAllowed"));
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(later_sum, &import_data, "Pinned"));
  const std::string after = manager.Emit();
  EXPECT_EQ(CountOccurrences(after, "} " + later_name + ";"), 1);
  for (const std::string& conflict : {std::string("Pinned"), later_name}) {
    EXPECT_FALSE(
        manager.AddTypeForFunctionOutput(existing, &import_data, conflict)
            .ok());
    EXPECT_EQ(manager.Emit(), after);
  }
}

TEST_F(DslxToVerilogTest, OrdinaryFunctionOutputCannotClaimPlannedSumNames) {
  constexpr std::string_view program = R"(
pub enum Message { Empty, Item(u8) }
fn ordinary(value: u8) -> u8 { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  Function* function = tm.module->GetFunction("ordinary").value();
  for (std::string_view alias : {"Message", "Message_tag_t"}) {
    SCOPED_TRACE(alias);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    manager.PrepareForModules({{tm.module, tm.type_info}});
    const std::string before = manager.Emit();
    const auto rejected =
        manager.AddTypeForFunctionOutput(function, &import_data, alias);
    EXPECT_FALSE(rejected.ok()) << rejected;
    EXPECT_EQ(manager.Emit(), before);

    XLS_EXPECT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinition("Message").value(), &import_data));
    const std::string emitted = manager.Emit();
    EXPECT_EQ(CountOccurrences(emitted, " Message;"), 1) << emitted;
    EXPECT_LE(CountOccurrences(emitted, " Message_tag_t;"), 1) << emitted;
  }

  constexpr std::string_view first_program = R"(
pub type Envelope_tag_t = u8;
pub enum Envelope { Empty, Item(u8) }
fn ordinary(value: Envelope_tag_t) -> Envelope_tag_t { value }
)";
  ImportData cross_module_imports = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule first,
                           ParseAndTypecheck(first_program, "first.x", "first",
                                             &cross_module_imports, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule second,
      ParseAndTypecheck("pub type Envelope_tag_t = u8;", "second.x", "second",
                        &cross_module_imports, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules(
      {{first.module, first.type_info}, {second.module, second.type_info}});
  Function* ordinary = first.module->GetFunction("ordinary").value();
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(ordinary, &cross_module_imports));
  const std::string ordinary_only = manager.Emit();
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(ordinary, &cross_module_imports));
  EXPECT_EQ(manager.Emit(), ordinary_only);
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      first.module->GetTypeDefinition("Envelope").value(),
      &cross_module_imports));
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] Envelope_tag_t;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, " Envelope_tag_t;"), 1) << emitted;
}

TEST_F(DslxToVerilogTest, RejectedOrdinaryFunctionOutputDoesNotEmitNestedSum) {
  constexpr std::string_view program = R"(
enum Outer { Empty, Item(u16) }
enum Inner { Empty, Item(u8) }
fn ordinary(value: (Inner, u8)) -> (Inner, u8) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Outer").value(), &import_data, "Taken"));
  const std::string before = manager.Emit();
  ASSERT_EQ(CountOccurrences(before, "typedef Outer Taken;"), 1) << before;
  ASSERT_EQ(before.find("} Inner;"), std::string::npos) << before;

  const auto rejected = manager.AddTypeForFunctionOutput(
      tm.module->GetFunction("ordinary").value(), &import_data, "Taken");
  EXPECT_FALSE(rejected.ok()) << rejected;
  EXPECT_NE(rejected.message().find("Taken"), std::string_view::npos)
      << rejected;
  EXPECT_EQ(manager.Emit(), before);

  constexpr std::string_view parametric_program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
fn nested(value: (Message<u32:8>, u8)) -> (Message<u32:8>, u8) { value }
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule parametric,
      ParseAndTypecheck(parametric_program, "parametric.x", "parametric",
                        &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(probe.AddTypeForFunctionOutput(
      parametric.module->GetFunction("concrete").value(), &import_data,
      "Concrete"));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &canonical));

  Function* nested = parametric.module->GetFunction("nested").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager pristine,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  pristine.PrepareForModules({{parametric.module, parametric.type_info}});
  XLS_ASSERT_OK(
      pristine.AddTypeForFunctionOutput(nested, &import_data, "Allowed"));
  for (const std::string& alias : {canonical, canonical + "_tag_t"}) {
    SCOPED_TRACE(alias);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager ordered,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    ordered.PrepareForModules({{parametric.module, parametric.type_info}});
    const std::string before = ordered.Emit();
    const auto rejected =
        ordered.AddTypeForFunctionOutput(nested, &import_data, alias);
    EXPECT_FALSE(rejected.ok()) << rejected;
    EXPECT_NE(rejected.message().find(alias), std::string_view::npos)
        << rejected;
    EXPECT_EQ(ordered.Emit(), before);
    XLS_ASSERT_OK(
        ordered.AddTypeForFunctionOutput(nested, &import_data, "Allowed"));
    EXPECT_EQ(ordered.Emit(), pristine.Emit());
  }

  constexpr std::string_view crowded_program = R"(#![feature(generics)]
type Message__value_3a_5_3a_u32_3a_8 = u8;
enum Message<N: u32> { Empty, Item(uN[N]) }
fn nested(value: (Message__value_3a_5_3a_u32_3a_8,
                  Message__value_3a_5_3a_u32_3a_8, Message<u32:8>))
    -> (Message__value_3a_5_3a_u32_3a_8,
        Message__value_3a_5_3a_u32_3a_8, Message<u32:8>) { value }
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule crowded,
                           ParseAndTypecheck(crowded_program, "crowded.x",
                                             "crowded", &import_data, nullptr));
  Function* crowded_nested = crowded.module->GetFunction("nested").value();
  const std::string crowded_alias = canonical + "__2";
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager crowded_pristine,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(crowded_pristine.AddTypeForFunctionOutput(
      crowded_nested, &import_data, "Allowed"));
  ASSERT_EQ(
      CountOccurrences(crowded_pristine.Emit(), "} " + crowded_alias + ";"), 1)
      << crowded_pristine.Emit();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager crowded_rejected,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  crowded_rejected.PrepareForModules({{crowded.module, crowded.type_info}});
  const std::string crowded_before = crowded_rejected.Emit();
  const auto rejected_crowded = crowded_rejected.AddTypeForFunctionOutput(
      crowded_nested, &import_data, crowded_alias);
  EXPECT_FALSE(rejected_crowded.ok()) << rejected_crowded;
  EXPECT_EQ(crowded_rejected.Emit(), crowded_before);
  XLS_ASSERT_OK(crowded_rejected.AddTypeForFunctionOutput(
      crowded_nested, &import_data, "Allowed"));
  EXPECT_EQ(crowded_rejected.Emit(), crowded_pristine.Emit());
}

TEST_F(DslxToVerilogTest,
       DynamicSumNamesPreserveEarlierUnemittedOrdinaryReservations) {
  constexpr std::string_view program = R"(#![feature(generics)]
type Message__value_3a_5_3a_u32_3a_8 = u8;
enum Message<N: u32> { Empty, Item(uN[N]) }
enum Other { Empty, Value(u1) }
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  const std::string ordinary_name = "Message__value_3a_5_3a_u32_3a_8";
  const TypeDefinition ordinary =
      tm.module->GetTypeDefinition(ordinary_name).value();
  const std::string invisible_family = ordinary_name + "__1";
  const std::string family = ordinary_name + "__2";
  const std::string invisible_tag = family + "_tag_t";
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(ordinary, &import_data));
  const std::string before = manager.Emit();
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(ordinary, &import_data));
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(ordinary, &import_data, invisible_tag));
  ASSERT_EQ(manager.Emit(), before);

  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(
      tm.module->GetFunction("concrete").value(), &import_data, "Output"));
  const std::string after = manager.Emit();
  EXPECT_EQ(CountOccurrences(after, "} " + family + ";"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, "} " + invisible_tag + "__1;"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, " " + invisible_tag + "__1 tag;"), 1)
      << after;
  EXPECT_EQ(CountOccurrences(after, "typedef " + family + " Output;"), 1)
      << after;

  const TypeDefinition other = tm.module->GetTypeDefinition("Other").value();
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(other, &import_data, invisible_family));
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(other, &import_data, invisible_tag));
  const std::string with_fixed_aliases = manager.Emit();
  EXPECT_EQ(CountOccurrences(with_fixed_aliases,
                             "typedef Other " + invisible_family + ";"),
            1)
      << with_fixed_aliases;
  EXPECT_EQ(CountOccurrences(with_fixed_aliases,
                             "typedef Other " + invisible_tag + ";"),
            1)
      << with_fixed_aliases;
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(other, &import_data, invisible_tag));
  EXPECT_EQ(manager.Emit(), with_fixed_aliases);
}

TEST_F(DslxToVerilogTest, RejectedOrdinaryAliasDoesNotEmitNestedSumPayload) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
enum Outer { Empty, Item(Message<u32:8>) }
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
fn nested(value: (Outer, u8)) -> (Outer, u8) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(probe.AddTypeForFunctionOutput(
      tm.module->GetFunction("concrete").value(), &import_data, "Concrete"));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &canonical));
  Function* nested = tm.module->GetFunction("nested").value();
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager pristine,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(
      pristine.AddTypeForFunctionOutput(nested, &import_data, "Allowed"));
  for (const std::string& alias : {canonical, canonical + "_tag_t"}) {
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    manager.PrepareForModules({{tm.module, tm.type_info}});
    const std::string before = manager.Emit();
    const auto rejected =
        manager.AddTypeForFunctionOutput(nested, &import_data, alias);
    EXPECT_FALSE(rejected.ok()) << rejected;
    EXPECT_EQ(manager.Emit(), before);
    XLS_ASSERT_OK(
        manager.AddTypeForFunctionOutput(nested, &import_data, "Allowed"));
    EXPECT_EQ(manager.Emit(), pristine.Emit());
  }
}

TEST_F(DslxToVerilogTest,
       ReprojectedOrdinaryNominalPreservesFixedFunctionAlias) {
  constexpr std::string_view first_program = R"(
pub enum Message__value_3a_5_3a_u32_3a_8_tag_t: u8 { First = 0 }
)";
  constexpr std::string_view second_program = R"(#![feature(generics)]
pub enum Message__value_3a_5_3a_u32_3a_8_tag_t: u8 { Second = 0 }
pub enum Carrier { Entry(Message__value_3a_5_3a_u32_3a_8_tag_t), Empty }
enum Message<N: u32> { Empty, Item(uN[N]) }
fn plain(value: u8) -> u8 { value }
fn generic(value: Message<u32:8>) -> Message<u32:8> { value }
)";
  const std::string ordinary = "Message__value_3a_5_3a_u32_3a_8_tag_t";
  const std::string fixed = ordinary + "__1";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule first,
                           ParseAndTypecheck(first_program, "first.x", "first",
                                             &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule second,
                           ParseAndTypecheck(second_program, "second.x",
                                             "second", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules(
      {{first.module, first.type_info}, {second.module, second.type_info}});
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      first.module->GetTypeDefinition(ordinary).value(), &import_data));
  Function* plain = second.module->GetFunction("plain").value();
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(plain, &import_data, fixed));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      second.module->GetTypeDefinition(ordinary).value(), &import_data));
  ASSERT_EQ(CountOccurrences(manager.Emit(), " " + fixed + ";"), 2)
      << manager.Emit();

  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      second.module->GetTypeDefinition("Carrier").value(), &import_data));
  ASSERT_EQ(CountOccurrences(manager.Emit(), " " + fixed + ";"), 1)
      << manager.Emit();
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(
      second.module->GetFunction("generic").value(), &import_data));
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(plain, &import_data, fixed));
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] " + fixed + ";"), 2)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, " " + fixed + ";"), 2) << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "} " + ordinary + "__2;"), 1) << emitted;
}

TEST_F(DslxToVerilogTest,
       KnownOrdinarySumWrappersStillCheckNewSpecializations) {
  constexpr std::string_view program = R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
type Plain = u8;
fn narrow(value: (Message<u32:8>, u8)) -> (Message<u32:8>, u8) { value }
fn narrow_sum(value: Message<u32:8>) -> Message<u32:8> { value }
fn plain(value: (Plain, u8)) -> (Plain, u8) { value }
fn mixed(value: (Message<u32:8>, Message<u32:16>))
    -> (Message<u32:8>, Message<u32:16>) { value }
fn wide(value: Message<u32:16>) -> Message<u32:16> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager probe,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(probe.AddTypeForFunctionOutput(
      tm.module->GetFunction("wide").value(), &import_data, "Wide"));
  std::string wide_name;
  ASSERT_TRUE(
      RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Wide;)", &wide_name));

  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  Function* narrow = tm.module->GetFunction("narrow").value();
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(narrow, &import_data));
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(
      tm.module->GetFunction("narrow_sum").value(), &import_data, "Unrelated"));
  for (int i = 0; i < 32; ++i) {
    XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(
        narrow, &import_data, absl::StrFormat("Repeated%d", i)));
  }
  Function* plain = tm.module->GetFunction("plain").value();
  for (int i = 0; i < 8; ++i) {
    XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(
        plain, &import_data, absl::StrFormat("Plain%d", i)));
  }
  const std::string before = manager.Emit();
  EXPECT_EQ(CountOccurrences(before, " Repeated"), 32) << before;
  EXPECT_EQ(CountOccurrences(before, "} Plain"), 8) << before;
  EXPECT_EQ(CountOccurrences(before, "typedef logic [7:0] Plain;"), 1)
      << before;

  Function* mixed = tm.module->GetFunction("mixed").value();
  const auto rejected =
      manager.AddTypeForFunctionOutput(mixed, &import_data, wide_name);
  EXPECT_FALSE(rejected.ok()) << rejected;
  EXPECT_EQ(manager.Emit(), before);
  XLS_ASSERT_OK(manager.AddTypeForFunctionOutput(mixed, &import_data, "Mixed"));
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} " + wide_name + ";"), 1)
      << manager.Emit();
}

TEST_F(DslxToVerilogTest, KnownSumsKeepOrdinaryDependencyAliasChecks) {
  constexpr std::string_view program = R"(
enum Message { Empty, Item(u8) }
type Fresh = u8;
type Taken = u16;
fn attempt(value: (Message, Fresh, Taken)) -> (Message, Fresh, Taken) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  const TypeDefinition message =
      tm.module->GetTypeDefinition("Message").value();
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(message, &import_data));
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(message, &import_data, "Taken"));
  const std::string before = manager.Emit();

  const auto rejected = manager.AddTypeForFunctionOutput(
      tm.module->GetFunction("attempt").value(), &import_data, "Attempt");
  EXPECT_FALSE(rejected.ok()) << rejected;
  EXPECT_EQ(manager.Emit(), before);
  EXPECT_EQ(CountOccurrences(manager.Emit(), " Fresh;"), 0) << manager.Emit();
}

TEST_F(DslxToVerilogTest, DirectSumDependencyConflictIsAtomicAndRepeatable) {
  constexpr std::string_view program = R"(
enum Seed { Empty, Item(u1) }
type Fresh = u8;
type Taken = u16;
struct Holder { fresh: Fresh, taken: Taken }
enum Outer { Item(Holder) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Seed").value(), &import_data, "Taken"));
  const std::string before = manager.Emit();
  ASSERT_EQ(CountOccurrences(before, "typedef Seed Taken;"), 1) << before;
  ASSERT_EQ(CountOccurrences(before, " Fresh;"), 0) << before;

  const TypeDefinition outer = tm.module->GetTypeDefinition("Outer").value();
  const absl::Status expected = absl::InvalidArgumentError(
      "SystemVerilog alias `Taken` for sum family `Seed` conflicts with an "
      "existing package symbol");
  for (int attempt = 0; attempt < 2; ++attempt) {
    SCOPED_TRACE(attempt);
    EXPECT_EQ(manager.AddTypeForTypeDefinition(outer, &import_data), expected);
    EXPECT_EQ(manager.Emit(), before);
  }
}

TEST_F(DslxToVerilogTest, IndependentRecordSumsIgnoreUnrelatedPackageNames) {
  std::string program = R"(
enum Old: u8 { Anchor = 0 }
enum Prelude { None, Some(Old) }
struct Anchor { value: u8 }
enum Overlap { None, Some(Anchor) }
struct AliasCollision { value: u8 }
enum AliasOverlap { None, Some(AliasCollision) }
)";
  constexpr int kCount = 8;
  for (int i = 0; i < kCount; ++i) {
    program += absl::StrFormat(
        "struct Record%d { value: u8 }\nenum Item%d { None, Some(Record%d) }\n",
        i, i, i);
  }
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  TypeDefinition prelude = tm.module->GetTypeDefinition("Prelude").value();
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(prelude, &import_data));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(prelude, &import_data,
                                                 "AliasCollision"));

  for (int i = 0; i < kCount; ++i) {
    const std::string name = absl::StrFormat("Item%d", i);
    EXPECT_TRUE(DslxTypeToVerilogManagerTestPeer::CanAddWithoutNameChanges(
        manager, ConcreteSum(tm, name)));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinition(name).value(), &import_data));
  }
  const std::string independent = manager.Emit();
  EXPECT_TRUE(RE2::PartialMatch(independent, R"(\n\s+Anchor = 8'h00)"))
      << independent;
  EXPECT_EQ(CountOccurrences(independent, "typedef Prelude AliasCollision;"),
            1);
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(CountOccurrences(independent, absl::StrFormat("} Record%d;", i)),
              1);
    EXPECT_EQ(CountOccurrences(independent, absl::StrFormat("} Item%d;", i)),
              1);
  }

  EXPECT_FALSE(DslxTypeToVerilogManagerTestPeer::CanAddWithoutNameChanges(
      manager, ConcreteSum(tm, "AliasOverlap")));
  EXPECT_FALSE(manager
                   .AddTypeForTypeDefinition(
                       tm.module->GetTypeDefinition("AliasOverlap").value(),
                       &import_data)
                   .ok());
  EXPECT_EQ(manager.Emit(), independent);
  EXPECT_FALSE(DslxTypeToVerilogManagerTestPeer::CanAddWithoutNameChanges(
      manager, ConcreteSum(tm, "Overlap")));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Overlap").value(), &import_data));
  EXPECT_TRUE(RE2::PartialMatch(manager.Emit(), R"(\n\s+Old_Anchor = 8'h00)"))
      << manager.Emit();
}

TEST_F(DslxToVerilogTest, PayloadGraphsCommitPerRequestAndPerSpecialization) {
  constexpr std::string_view program = R"(#![feature(generics)]
struct Small { value: u8 }
struct Large { value: u16 }
enum Inner<T: type> { None, Some(T) }
type Narrow = Inner<Small>;
type Wide = Inner<Large>;
enum Seed { None, Some(u8) }
type Taken = u8;
struct Broken { taken: Taken }
enum Failing { Pair(Wide, Broken) }
enum Working { Pair(Narrow, Wide) }
enum Leaf { None, Some(u8) }
enum Mid { None, Some(Leaf) }
enum Top { None, Some(Mid) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  manager.PrepareForModules({{tm.module, tm.type_info}});
  const SumType& narrow = ConcreteSum(tm, "Narrow");
  const SumType& wide = ConcreteSum(tm, "Wide");
  ASSERT_EQ(&narrow.nominal_type(), &wide.nominal_type());
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Narrow").value(), &import_data));
  EXPECT_TRUE(
      DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(manager, narrow));
  EXPECT_FALSE(
      DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(manager, wide));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Seed").value(), &import_data, "Taken"));
  const std::string before = manager.Emit();
  TypeDefinition failing = tm.module->GetTypeDefinition("Failing").value();
  const absl::Status rejected =
      manager.AddTypeForTypeDefinition(failing, &import_data);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(manager.AddTypeForTypeDefinition(failing, &import_data), rejected);
  EXPECT_EQ(manager.Emit(), before);
  EXPECT_FALSE(DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(
      manager, ConcreteSum(tm, "Failing")));
  EXPECT_FALSE(
      DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(manager, wide));

  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Working").value(), &import_data));
  EXPECT_TRUE(
      DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(manager, wide));
  EXPECT_TRUE(DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(
      manager, ConcreteSum(tm, "Working")));
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} Small;"), 1);
  EXPECT_EQ(CountOccurrences(manager.Emit(), "} Large;"), 1);

  // A failure after ordinary marking, during later emission, must also discard
  // the entire graph batch. A following public export can then commit it.
  const std::string before_late_failure = manager.Emit();
  EXPECT_EQ(DslxTypeToVerilogManagerTestPeer::MarkThenReject(
                manager, ConcreteSum(tm, "Top")),
            absl::InvalidArgumentError("later emission rejected"));
  EXPECT_EQ(manager.Emit(), before_late_failure);
  for (std::string_view name : {"Leaf", "Mid", "Top"}) {
    EXPECT_FALSE(DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(
        manager, ConcreteSum(tm, name)));
  }
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Top").value(), &import_data));
  for (std::string_view name : {"Leaf", "Mid", "Top"}) {
    EXPECT_TRUE(DslxTypeToVerilogManagerTestPeer::HasCommittedGraph(
        manager, ConcreteSum(tm, name)));
    EXPECT_EQ(CountOccurrences(manager.Emit(), absl::StrFormat("} %s;", name)),
              1);
  }
}

TEST_F(DslxToVerilogTest, UnemittedOrdinaryAliasDoesNotBlockSumProjection) {
  constexpr std::string_view program = R"(
pub struct byte { f: u8 }
pub enum Code: u8 { wire = 0 }
pub enum Message { None, Pair(Code, byte) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  const TypeDefinition record = tm.module->GetTypeDefinition("byte").value();
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(record, &import_data));
  const std::string ordinary_record = manager.Emit();
  ASSERT_EQ(CountOccurrences(ordinary_record, "} byte;"), 1) << ordinary_record;
  ASSERT_EQ(CountOccurrences(ordinary_record, "} byte_;"), 0)
      << ordinary_record;
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(record, &import_data, "byte_"));
  ASSERT_EQ(manager.Emit(), ordinary_record);
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Code").value(), &import_data));
  const std::string before = manager.Emit();
  ASSERT_TRUE(RE2::PartialMatch(before, R"(\n\s+wire = 8'h00)")) << before;

  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data));
  const std::string after = manager.Emit();
  EXPECT_EQ(CountOccurrences(after, "} byte_;"), 1) << after;
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+wire_ = 8'h00)")) << after;
  EXPECT_EQ(CountOccurrences(after, "} Message;"), 1) << after;
}

TEST_F(DslxToVerilogTest, UnemittedImportedAliasDoesNotBlockSumProjection) {
  constexpr std::string_view program = R"(
import a;
import b;
pub type OtherRecord = b::Record;
pub enum Code: u8 { Common = 0 }
enum Other: u8 { Common = 0 }
pub enum Message { None, Pair(Code, Other, a::Record) }
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files;
  files[std::filesystem::path("/a.x")] = "pub struct Record { f: u8 }";
  files[std::filesystem::path("/b.x")] = "pub struct Record { f: u8 }";
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      ImportedInfo * imported,
      tm.type_info->GetImportedOrError(tm.module->GetImportByName().at("a")));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Code").value(), &import_data));
  const TypeDefinition record =
      imported->module->GetTypeDefinition("Record").value();
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(record, &import_data));
  const std::string before = manager.Emit();
  ASSERT_TRUE(RE2::PartialMatch(before, R"(\n\s+Common = 8'h00)")) << before;
  ASSERT_EQ(CountOccurrences(before, "} Record;"), 1) << before;
  ASSERT_EQ(CountOccurrences(before, "} a_Record;"), 0) << before;
  XLS_ASSERT_OK(
      manager.AddTypeForTypeDefinition(record, &import_data, "a_Record"));
  ASSERT_EQ(manager.Emit(), before);

  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data));
  const std::string after = manager.Emit();
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+Code_Common = 8'h00)")) << after;
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+Other_Common = 8'h00)"))
      << after;
  EXPECT_EQ(CountOccurrences(after, "} a_Record;"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, "} Message;"), 1) << after;
}

TEST_F(DslxToVerilogTest, NestedSumProjectedEnumConflictDoesNotEmitPrelude) {
  constexpr std::string_view program = R"(
enum Seed { Empty, Item(u1) }
type Prelude = u8;
enum E: u1 { byte = 0 }
enum Carrier { Value(E) }
fn ordinary(value: (Prelude, Carrier)) -> (Prelude, Carrier) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Seed").value(), &import_data, "byte_"));
  const std::string before = manager.Emit();
  ASSERT_EQ(CountOccurrences(before, "typedef Seed byte_;"), 1) << before;
  ASSERT_EQ(CountOccurrences(before, " Prelude;"), 0) << before;

  Function* ordinary = tm.module->GetFunction("ordinary").value();
  const absl::Status expected = absl::InvalidArgumentError(
      "SystemVerilog alias `byte_` for sum family `Seed` conflicts with an "
      "existing package symbol");
  for (int attempt = 0; attempt < 2; ++attempt) {
    SCOPED_TRACE(attempt);
    EXPECT_EQ(
        manager.AddTypeForFunctionOutput(ordinary, &import_data, "Output"),
        expected);
    EXPECT_EQ(manager.Emit(), before);
  }
}

TEST_F(DslxToVerilogTest, SumAliasConflictingWithNewDependencyIsAtomic) {
  constexpr std::string_view program = R"(
struct Slot { value: u8 }
enum S { Empty, Value(Slot) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  const TypeDefinition sum = tm.module->GetTypeDefinition("S").value();
  const std::string before = manager.Emit();
  const absl::Status expected = absl::InvalidArgumentError(
      "SystemVerilog alias `Slot` for sum family `S` conflicts with an "
      "existing package symbol");
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(manager.AddTypeForTypeDefinition(sum, &import_data, "Slot"),
              expected);
    EXPECT_EQ(manager.Emit(), before);
  }
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(sum, &import_data));
  const std::string after = manager.Emit();
  EXPECT_EQ(CountOccurrences(after, "} Slot;"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, "} S;"), 1) << after;
}

TEST_F(DslxToVerilogTest, OuterOrdinaryAliasUsesFinalNestedEnumProjection) {
  constexpr std::string_view program = R"(
enum Seed { Empty, Item(u1) }
type Prelude = u8;
enum E: u1 { Foo = 0 }
enum Carrier { Value(E) }
fn ordinary(value: (Prelude, Carrier)) -> (Prelude, Carrier) { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Seed").value(), &import_data, "E_Foo"));
  const std::string before = manager.Emit();
  const absl::Status expected = absl::InvalidArgumentError(
      "SystemVerilog alias `E_Foo` for sum family `Seed` conflicts with an "
      "existing package symbol");
  Function* ordinary = tm.module->GetFunction("ordinary").value();
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(manager.AddTypeForFunctionOutput(ordinary, &import_data, "Foo"),
              expected);
    EXPECT_EQ(manager.Emit(), before);
  }
  XLS_ASSERT_OK(
      manager.AddTypeForFunctionOutput(ordinary, &import_data, "Output"));
  const std::string after = manager.Emit();
  EXPECT_EQ(CountOccurrences(after, " Prelude;"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, "} Carrier;"), 1) << after;
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+Foo = 1'h0)")) << after;

  EXPECT_EQ(manager.AddTypeForFunctionOutput(ordinary, &import_data, "Foo"),
            expected);
  EXPECT_EQ(manager.Emit(), after);
}

// Negative test: conflicting sum aliases are rejected without changing output.
TEST_F(DslxToVerilogTest, RejectedSumFunctionAliasPreservesOrdinaryPackage) {
  constexpr std::string_view program = R"(
enum A: u1 { TOKEN = 0 }
enum B: u1 { TOKEN = 1 }
pub type Taken = u1;
enum S { A(A), B(B) }
fn x(value: S) -> S { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  const std::vector<TypeDefinition> definitions =
      tm.module->GetTypeDefinitions();
  ASSERT_EQ(definitions.size(), 4);
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (int i : {0, 2}) {
    XLS_ASSERT_OK(
        manager.AddTypeForTypeDefinition(definitions[i], &import_data));
  }
  const std::string before = manager.Emit();
  ASSERT_TRUE(RE2::PartialMatch(before, R"(\n\s+TOKEN = 1'h0)")) << before;
  ASSERT_TRUE(RE2::PartialMatch(before, R"(typedef logic(?: \[0:0\])? Taken;)"))
      << before;
  Function* function = tm.module->GetFunction("x").value();
  const auto param =
      manager.AddTypeForFunctionParam(function, &import_data, "value", "Taken");
  ASSERT_FALSE(param.ok());
  EXPECT_NE(param.message().find("Taken"), std::string_view::npos) << param;
  EXPECT_EQ(manager.Emit(), before);
  const auto output =
      manager.AddTypeForFunctionOutput(function, &import_data, "Taken");
  ASSERT_FALSE(output.ok());
  EXPECT_NE(output.message().find("Taken"), std::string_view::npos) << output;
  EXPECT_EQ(manager.Emit(), before);
}

// Verifies: Unrelated typedefs do not change source fields or fixed positions.
// Catches: Root order altering the view, constructor, or getter interface.
TEST_F(DslxToVerilogTest, SumMembersIgnoreUnrelatedPackageTypedefOrder) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule other,
      ParseAndTypecheck("pub type Later = u8; pub type value = u8; "
                        "pub type index_0 = u8;",
                        "other.x", "other", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule owner,
      ParseAndTypecheck("pub enum Names { None, Fields { Later: u8 }, "
                        "Item(u8), Pair(u8, u8) }",
                        "owner.x", "owner", &import_data, nullptr));
  for (bool owner_first : {false, true}) {
    SCOPED_TRACE(owner_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    manager.PrepareForModules(
        {{other.module, other.type_info}, {owner.module, owner.type_info}});
    for (Module* module :
         (owner_first ? std::vector{owner.module, other.module}
                      : std::vector{other.module, owner.module})) {
      for (const TypeDefinition& definition : module->GetTypeDefinitions()) {
        XLS_ASSERT_OK(
            manager.AddTypeForTypeDefinition(definition, &import_data));
      }
    }
    const std::string emitted = manager.Emit();
    for (std::string_view text :
         {"logic [7:0] Later;", "logic [7:0] value;", "logic [7:0] index_0;",
          "input logic [7:0] Later", "input logic [7:0] value",
          "input logic [7:0] index_0", "Names_get_tag (input Names value)"}) {
      EXPECT_NE(emitted.find(text), std::string::npos) << emitted;
    }
    for (std::string_view text : {"Later__1", "value__1", "index_0__1"}) {
      EXPECT_EQ(emitted.find(text), std::string::npos) << emitted;
    }
  }
}

// Verifies: Wide padding is an explicitly sized repeat before the payload.
// Catches: Allocating wide zero literals or changing padding or payload widths.
TEST_F(DslxToVerilogTest, SumConstructorsEmitCompactWidePadding) {
  constexpr std::string_view program = R"(
pub enum Small { Empty, One(u1), Full(uN[65]) }
// Each scalar fits the DSLX bit-count limit; the total payload is 1048576 bits.
pub enum Wide { Empty, One(u1), Full(uN[524288][2]) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(definition, &import_data));
  }
  const std::string emitted = manager.Emit();
  for (std::string_view statement : {
           "Small_make_empty = Small'({Small_tag_Empty, {65{1'h0}}});",
           "Small_make_one = Small'({Small_tag_One, "
           "64'h0000_0000_0000_0000, value});",
           "Small_make_full = Small'({Small_tag_Full, value});",
           "Wide_make_empty = Wide'({Wide_tag_Empty, {1048576{1'h0}}});",
           "Wide_make_one = Wide'({Wide_tag_One, {1048575{1'h0}}, value});",
           "Wide_make_full = Wide'({Wide_tag_Full, value});",
       }) {
    EXPECT_NE(emitted.find(statement), std::string::npos) << emitted;
  }
}

// Verifies: Views keep fields that shadow constructor, type, or tag.
// Catches: Shadowed SV symbols or renamed inputs missing in the body.
TEST_F(DslxToVerilogTest, SumConstructorSeparatesFieldAndFunctionScopeNames) {
  constexpr std::string_view program = R"(
pub enum Message {
  None,
  Value {
    Message_make_value: u8,
    Message_make_value__1: u8,
    Message: u8,
    Message_tag_Value: u8,
  },
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinitions().front(), &import_data));
  const std::string emitted = manager.Emit();
  std::string view;
  ASSERT_TRUE(RE2::PartialMatch(
      emitted, R"(typedef struct packed \{([^}]+)\} Message_value_view_t;)",
      &view))
      << emitted;
  for (std::string_view name : {"Message_make_value", "Message_make_value__1",
                                "Message", "Message_tag_Value"}) {
    EXPECT_NE(view.find(absl::StrFormat("logic [7:0] %s;", name)),
              std::string::npos)
        << view;
  }
  constexpr std::string_view identifier = "([A-Za-z_][A-Za-z0-9_$]*)";
  const std::string declaration = absl::StrFormat(
      R"(function automatic Message Message_make_value \(input logic \[7:0\] %s, input logic \[7:0\] %s, input logic \[7:0\] %s, input logic \[7:0\] %s\);)",
      identifier, identifier, identifier, identifier);
  std::vector<std::string> formals(4);
  ASSERT_TRUE(RE2::PartialMatch(emitted, declaration, &formals[0], &formals[1],
                                &formals[2], &formals[3]))
      << emitted;
  EXPECT_NE(formals[0], "Message_make_value");
  EXPECT_EQ(formals[1], "Message_make_value__1");
  EXPECT_NE(formals[2], "Message");
  EXPECT_NE(formals[3], "Message_tag_Value");
  EXPECT_EQ(std::set<std::string>(formals.begin(), formals.end()).size(), 4);
  const std::string assignment = absl::StrFormat(
      R"(Message_make_value\s*=\s*Message'\(\{Message_tag_Value,\s*%s,\s*%s,\s*%s,\s*%s\}\);)",
      identifier, identifier, identifier, identifier);
  std::vector<std::string> assigned(4);
  ASSERT_TRUE(RE2::PartialMatch(emitted, assignment, &assigned[0], &assigned[1],
                                &assigned[2], &assigned[3]))
      << emitted;
  EXPECT_EQ(assigned, formals);
}

// Verifies: Packed aggregates omit zero-width sums but keep nonzero fields.
// Catches: Accepting standalone zero-width sums or dropping sibling fields.
TEST_F(DslxToVerilogTest, OmitsZeroWidthSemanticSumFromPackedAggregate) {
  constexpr std::string_view program = R"(
enum Marker {
  Only(),
}

pub struct Wrapper {
  marker: Marker,
  nested: (Marker, u8),
  value: u8,
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));
  const std::vector<TypeDefinition> definitions =
      tm.module->GetTypeDefinitions();
  ASSERT_EQ(definitions.size(), 2);
  const auto standalone_status =
      type_to_verilog.AddTypeForTypeDefinition(definitions[0], &import_data);
  ASSERT_FALSE(standalone_status.ok());
  EXPECT_NE(standalone_status.message().find("Zero sized interface type"),
            std::string_view::npos)
      << standalone_status;
  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForTypeDefinition(definitions[1], &import_data));

  const std::string emitted = type_to_verilog.Emit();
  EXPECT_EQ(emitted.find("} Marker;"), std::string::npos) << emitted;
  EXPECT_EQ(emitted.find(" marker;"), std::string::npos) << emitted;
  EXPECT_EQ(emitted.find(" index_0;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("logic [7:0] index_1;"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("logic [7:0] value;"), std::string::npos) << emitted;
}

TEST_F(DslxToVerilogTest, NestedTypeDefinition) {
  constexpr std::string_view program =
      R"(
struct Point {
  x: u16,
  y: u32,
}

enum Option : u5 {
  ZERO = 0,
  ONE = 1,
}

type AliasType = Point;
type AliasType1 = Point[1];
type AliasType2 = uN[100];

struct TopType {
  a: Point,
  b: Option,
  c: AliasType,
  d: AliasType1,
  e: AliasType2,
}
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(def, &import_data));
  }

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, TypeWithNestedTuple) {
  constexpr std::string_view program =
      R"(
struct NestedType {
  x: (u16, u32),
  y: u32[4],
}

fn f() -> NestedType {
  zero!<NestedType>()
}
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  Function* func = tm.module->GetFunction("f").value();

  XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionOutput(
      func, &import_data, "user_defined_name_t"));

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, MultiDimTest) {
  constexpr std::string_view program =
      R"(
struct StructType {
  x: u16,
}

fn f(a : StructType[4][7], b : u32[5][8], c : bits[300][8][9]) -> u32 {
  u32:0
}
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  Function* func = tm.module->GetFunction("f").value();

  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForFunctionParam(func, &import_data, "a"));

  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForFunctionParam(func, &import_data, "b"));

  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForFunctionParam(func, &import_data, "c"));

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, ArrayTypedefTest) {
  constexpr std::string_view program =
      R"(
struct StructType {
  x: u16,
}

type ArrayOfStructType = StructType[5];

fn f(a : ArrayOfStructType, b : ArrayOfStructType[2]) -> u32 {
  u32:0
}
)";

  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  Function* func = tm.module->GetFunction("f").value();

  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForFunctionParam(func, &import_data, "a"));
  XLS_ASSERT_OK(
      type_to_verilog.AddTypeForFunctionParam(func, &import_data, "b"));

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, ImportedTypesWithTheSameName) {
  constexpr std::string_view program =
      R"(
import a;
import b;

type AType = a::ArrayOfStructType;
type BType = b::ArrayOfStructType;

)";

  constexpr std::string_view a_import =
      R"(
struct StructType {
  x: u16,
}

pub type ArrayOfStructType = StructType[5];
)";
  constexpr std::string_view b_import =
      R"(
struct StructType {
  x: u32,
}

pub type ArrayOfStructType = StructType[10];
)";

  absl::flat_hash_map<std::filesystem::path, std::string> files;
  files[std::filesystem::path("/a.x")] = a_import;
  files[std::filesystem::path("/b.x")] = b_import;
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  // Parse and typecheck program.
  dslx::ImportData import_data = dslx::CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      dslx::ParseAndTypecheck(program, "test_module.x", "test_module",
                              &import_data, nullptr));

  // Create package, add function types, and check output
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    XLS_ASSERT_OK(type_to_verilog.AddTypeForTypeDefinition(def, &import_data));
  }

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

}  // namespace
}  // namespace xls::dslx
