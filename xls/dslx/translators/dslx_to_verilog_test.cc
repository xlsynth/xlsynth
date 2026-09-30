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

#include <filesystem>
#include <memory>
#include <optional>
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
  XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager type_to_verilog,
                           DslxTypeToVerilogManager::Create("test_pkg"));

  // Function annotations use a different token-check/export entry point from
  // public type definitions. Exercise both consumers of the shared sum type.
  XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionParam(function, &import_data,
                                                        "x", "input_t"));
  XLS_ASSERT_OK(type_to_verilog.AddTypeForFunctionOutput(function, &import_data,
                                                         "output_t"));
  const std::string emitted = type_to_verilog.Emit();
  EXPECT_NE(emitted.find("input_t"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("output_t"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("logic [8:0] payload;"), std::string::npos) << emitted;
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

TEST_F(DslxToVerilogTest, SumPayloadEnumDefersToUnrelatedOrdinaryEnum) {
  constexpr std::string_view program = R"(
pub enum Payload: u1 { TOKEN = 0 }
pub struct Record { byte: u8 }
pub enum Message { None, Value((Payload, Record)) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule module,
      ParseAndTypecheck(program, "test_module.x", "test_module", &import_data,
                        nullptr));
  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    auto add = [&](std::string_view name) {
      return manager.AddTypeForTypeDefinition(
          module.module->GetTypeDefinition(name).value(), &import_data);
    };
    if (sum_first) {
      XLS_ASSERT_OK(add("Message"));
      const std::string sum = manager.Emit();
      EXPECT_EQ(sum.find("} Payload;"), std::string::npos) << sum;
      EXPECT_EQ(sum.find("} Record;"), std::string::npos) << sum;
    }
    XLS_ASSERT_OK(add("Payload"));
    XLS_ASSERT_OK(add("Record"));
    if (!sum_first) {
      XLS_ASSERT_OK(add("Message"));
    }
    const std::string emitted = manager.Emit();
    EXPECT_NE(emitted.find("TOKEN = 1'h0"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("logic [7:0] byte;"), std::string::npos) << emitted;
    EXPECT_NE(emitted.find("logic [8:0] payload;"), std::string::npos)
        << emitted;
    EXPECT_EQ(emitted.find("byte_;"), std::string::npos) << emitted;
    EXPECT_EQ(emitted.find("_view_t"), std::string::npos) << emitted;
  }
}

TEST_F(DslxToVerilogTest, SumGeneratedPackageNamesRetainTheirOwners) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule first,
      ParseAndTypecheck("pub enum Duplicate { Empty, Item(u8) } "
                        "pub fn identity(x: Duplicate) -> Duplicate { x }",
                        "a.x", "a", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule second,
      ParseAndTypecheck("pub enum Duplicate { Empty, Item(u16) } "
                        "pub type a_Duplicate_tag_t = u8;",
                        "b.x", "b", &import_data, nullptr));

  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager manager,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    const std::string empty = manager.Emit();
    if (reverse) {
      manager.PrepareForModules(
          {{second.module, second.type_info}, {first.module, first.type_info}});
    } else {
      manager.PrepareForModules(
          {{first.module, first.type_info}, {second.module, second.type_info}});
    }
    EXPECT_EQ(manager.Emit(), empty);
    Module* earlier = reverse ? second.module : first.module;
    Module* later = reverse ? first.module : second.module;
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        earlier->GetTypeDefinition("Duplicate").value(), &import_data));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        later->GetTypeDefinition("Duplicate").value(), &import_data));
    XLS_ASSERT_OK(manager.AddTypeForTypeDefinition(
        second.module->GetTypeDefinition("a_Duplicate_tag_t").value(),
        &import_data));
    const std::string before = manager.Emit();
    EXPECT_EQ(CountOccurrences(before, "} a_Duplicate;"), 1) << before;
    EXPECT_EQ(CountOccurrences(before, "} b_Duplicate;"), 1) << before;
    EXPECT_EQ(
        CountOccurrences(before, "typedef logic [7:0] a_Duplicate_tag_t;"), 1)
        << before;
    EXPECT_EQ(before.find("a_Duplicate_tag_t__1"), std::string::npos) << before;
    EXPECT_EQ(before.find("union packed"), std::string::npos) << before;

    Function* identity = first.module->GetFunction("identity").value();
    auto reserved = manager.AddTypeForFunctionOutput(identity, &import_data,
                                                     "a_Duplicate_tag_t__1");
    EXPECT_FALSE(reserved.ok()) << reserved;
    EXPECT_EQ(manager.Emit(), before);
    XLS_ASSERT_OK(
        manager.AddTypeForFunctionOutput(identity, &import_data, "Explicit"));
    EXPECT_EQ(CountOccurrences(manager.Emit(), "typedef a_Duplicate Explicit;"),
              1);
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

  for (bool sum_first : {false, true}) {
    SCOPED_TRACE(sum_first);
    XLS_ASSERT_OK_AND_ASSIGN(DslxTypeToVerilogManager ordered,
                             DslxTypeToVerilogManager::Create("test_pkg"));
    auto add_sum = [&]() {
      return ordered.AddTypeForFunctionOutput(function, &import_data, "TOKEN");
    };
    auto add_enum = [&]() {
      return ordered.AddTypeForTypeDefinition(definitions[0], &import_data);
    };
    if (sum_first) {
      XLS_ASSERT_OK(add_sum());
    } else {
      XLS_ASSERT_OK(add_enum());
    }
    const std::string accepted = ordered.Emit();
    const auto rejected = sum_first ? add_enum() : add_sum();
    ASSERT_FALSE(rejected.ok());
    EXPECT_NE(rejected.message().find("TOKEN"), std::string_view::npos)
        << rejected;
    EXPECT_EQ(ordered.Emit(), accepted);
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
