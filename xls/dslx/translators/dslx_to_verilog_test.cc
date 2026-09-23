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
#include "xls/dslx/virtualizable_file_system.h"

namespace xls::dslx {
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

// Verifies: An unrelated ordinary enum retains its source spelling in either
// order. Catches: Failing to refresh, or displacing the wrong literal, on a
// shared name.
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
    if (sum_first) {
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(definitions[2], &import_data));
    }
    XLS_ASSERT_OK(
        manager.AddTypeForTypeDefinition(definitions[0], &import_data));
    XLS_ASSERT_OK(
        manager.AddTypeForTypeDefinition(definitions[1], &import_data));
    if (!sum_first) {
      XLS_ASSERT_OK(
          manager.AddTypeForTypeDefinition(definitions[2], &import_data));
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

// Verifies: An emitted ordinary typedef still blocks an explicit sum alias.
// Catches: Renaming payload enums even though their sum alias must be rejected.
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
