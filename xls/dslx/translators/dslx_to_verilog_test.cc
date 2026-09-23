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
    EXPECT_NE(aliases.first, aliases.second) << emitted;
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
