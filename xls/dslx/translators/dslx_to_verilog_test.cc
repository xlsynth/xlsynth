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
#include <cstdint>
#include <filesystem>
#include <map>
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
#include "xls/codegen/vast/vast.h"
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
#include "xls/ir/source_location.h"

namespace xls::dslx {

class DslxTypeToVerilogManagerTestPeer {
 public:
  static std::string ProjectedMemberNameScopes() {
    DslxTypeToVerilogManager manager("test_pkg");
    verilog::DataType* scalar = manager.MakeBits(1);
    verilog::DataType* token = manager.AddNamedType("Token", scalar);
    verilog::DataType* positional = manager.AddNamedType("index_0", scalar);
    auto array = [&](verilog::DataType* element) {
      return manager.file_->Make<verilog::PackedArrayType>(
          SourceInfo(), element, std::vector<int64_t>{2}, false);
    };
    auto record = [&](const std::vector<verilog::Def*>& fields) {
      return manager.file_->Make<verilog::Struct>(SourceInfo(), fields);
    };

    std::vector<verilog::Def*> named = {
        manager.MakeMember("Token", scalar), manager.MakeMember("value", token),
        manager.MakeMember("values", array(token))};
    manager.ProjectSumAggregateMemberNames(named);
    manager.AddNamedType("NamedBefore", record(named));
    std::vector<verilog::Def*> named_reverse = {
        manager.MakeMember("value", token),
        manager.MakeMember("values", array(token)),
        manager.MakeMember("Token", scalar)};
    manager.ProjectSumAggregateMemberNames(named_reverse);
    manager.AddNamedType("NamedAfter", record(named_reverse));

    std::vector<verilog::Def*> fixed = {
        manager.MakeMember("index_0", scalar),
        manager.MakeMember("index_1", positional),
        manager.MakeMember("index_2", array(positional))};
    manager.ProtectFixedSumMemberNames(fixed);
    manager.AddNamedType("FixedBefore", record(fixed));
    std::vector<verilog::Def*> fixed_reverse = {
        manager.MakeMember("index_0", array(positional)),
        manager.MakeMember("index_1", scalar)};
    manager.ProtectFixedSumMemberNames(fixed_reverse);
    manager.AddNamedType("FixedAfter", record(fixed_reverse));

    std::vector<verilog::Def*> nested = {
        manager.MakeMember("index_0", scalar),
        manager.MakeMember("index_1", positional)};
    std::vector<verilog::Def*> ordinary = {
        manager.MakeMember("tuples", array(record(nested)))};
    manager.ProjectOrdinarySumStructMemberNames(ordinary);
    manager.AddNamedType("Ordinary", record(ordinary));
    return manager.file_->Emit();
  }

  static absl::StatusOr<std::string> ProjectedPayloadTypes(
      const SumType& sum, ImportData* import_data) {
    DslxTypeToVerilogManager manager("test_pkg");
    DslxTypeToVerilogManager::SumFamily family;
    family.type = sum.CloneToUnique();
    family.name = "Projection";
    auto symbols = manager.CompanionNameRequests(sum, family.name);
    if (!symbols.ok()) {
      return symbols.status();
    } else {
      family.symbols = *std::move(symbols);
    }
    for (const SumTypeVariant& variant : sum.variants()) {
      auto type = manager.SumMemberToVastType(variant.GetMemberType(0), family,
                                              import_data);
      if (!type.ok()) {
        return type.status();
      } else {
        manager.AddNamedType(variant.variant().identifier(), *type);
      }
    }
    return manager.file_->Emit();
  }
};

namespace {

// Collects the selected requests and exercises the public batch API only when
// a test asks for the resulting package.
class BatchExportCollector {
 public:
  explicit BatchExportCollector(std::string_view package_name)
      : package_name_(package_name) {}

  void PrepareForModules(
      std::vector<std::pair<Module*, TypeInfo*>> roots,
      const std::map<const AstNode*, std::string>& owners = {},
      SumQualifierPolicy policy = SumQualifierPolicy::kWholeModuleGraph) {
    options_.roots = std::move(roots);
    options_.public_nominal_owners = owners;
    options_.sum_qualifier_policy = policy;
    package_.reset();
  }

  void AddTypeForTypeDefinition(
      const TypeDefinition& definition, ImportData* import_data,
      std::optional<std::string_view> name = std::nullopt) {
    Add({definition, CopyName(name)}, import_data);
  }

  void AddTypeForFunctionParam(
      Function* function, ImportData* import_data, std::string_view param_name,
      std::optional<std::string_view> name = std::nullopt) {
    Add({VerilogFunctionParamExport{function, std::string(param_name)},
         CopyName(name)},
        import_data);
  }

  void AddTypeForFunctionOutput(
      Function* function, ImportData* import_data,
      std::optional<std::string_view> name = std::nullopt) {
    Add({VerilogFunctionOutputExport{function}, CopyName(name)}, import_data);
  }

  const std::string& Emit() const {
    if (!package_.has_value()) {
      auto result = ExportDslxTypesToVerilog(package_name_, import_data_,
                                             requests_, options_);
      EXPECT_TRUE(result.ok()) << result.status();
      package_ = result.ok() ? *std::move(result) : std::string();
    }
    return *package_;
  }

 private:
  static std::optional<std::string> CopyName(
      std::optional<std::string_view> name) {
    return name.has_value() ? std::optional<std::string>(*name) : std::nullopt;
  }

  void Add(VerilogExportRequest request, ImportData* import_data) {
    EXPECT_TRUE(import_data_ == nullptr || import_data_ == import_data);
    import_data_ = import_data;
    requests_.push_back(std::move(request));
    package_.reset();
  }

  std::string package_name_;
  ImportData* import_data_ = nullptr;
  std::vector<VerilogExportRequest> requests_;
  VerilogExportOptions options_;
  mutable std::optional<std::string> package_;
};

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

TEST_F(DslxToVerilogTest, BatchReservesSelectedAliasesBeforeGeneratedNames) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("enum First { Empty, Item(u8) } "
                        "enum Second { Empty, Item(u16) }",
                        "test.x", "test", &import_data, nullptr));
  TypeDefinition first = tm.module->GetTypeDefinition("First").value();
  TypeDefinition second = tm.module->GetTypeDefinition("Second").value();
  for (bool reverse : {false, true}) {
    std::vector<VerilogExportRequest> requests = {{first, "Second_make_item"},
                                                  {second, std::nullopt}};
    if (reverse) {
      std::reverse(requests.begin(), requests.end());
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string package,
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
    EXPECT_EQ(CountOccurrences(package, "typedef First Second_make_item;"), 1);
    EXPECT_NE(package.find("Second_make_item__1 ("), std::string::npos);
  }
  const std::vector<VerilogExportRequest> conflicting = {{first, "Fixed"},
                                                         {second, "Fixed"}};
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, conflicting).ok());
}

TEST_F(DslxToVerilogTest, BatchRejectsFunctionAliasTakenByNewDependency) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
fn direct(x: Message<u32:8>) -> Message<u32:8> { x }
fn tuple(x: (Message<u32:8>, u8)) -> (Message<u32:8>, u8) { x }
)",
                        "test.x", "test", &import_data, nullptr));
  Function* direct = tm.module->GetFunction("direct").value();
  Function* tuple = tm.module->GetFunction("tuple").value();
  const std::vector<VerilogExportRequest> probe = {
      {VerilogFunctionOutputExport{direct}, std::nullopt}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, probe));
  std::string helper;
  ASSERT_TRUE(RE2::PartialMatch(
      package, R"(function automatic [^\n]+ ([^ ]+_make_item) \()", &helper));
  const std::vector<VerilogExportRequest> conflicting = {
      {VerilogFunctionParamExport{tuple, "x"}, helper}};
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, conflicting).ok());
}

TEST_F(DslxToVerilogTest, BatchGenericSameNameAliasKeepsSpecializationName) {
  absl::flat_hash_map<std::filesystem::path, std::string> files;
  files["/shared.x"] = R"(#![feature(generics)]
pub enum Event<N: u32> { Empty, Item(uN[N]) }
pub fn direct(x: Event<u32:8>) -> Event<u32:8> { x }
)";
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule client,
      ParseAndTypecheck(R"(#![feature(generics)]
import shared;
pub type Event = shared::Event<u32:8>;
)",
                        "client.x", "client", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * shared,
                           import_data.Get(ImportTokens({"shared"})));
  Function* direct = shared->module().GetFunction("direct").value();
  const std::vector<VerilogExportRequest> probe = {
      {VerilogFunctionOutputExport{direct}, "Direct"}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string direct_package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, probe));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(direct_package, R"(typedef ([^ ]+) Direct;)",
                                &canonical));
  const std::vector<VerilogExportRequest> aliases = {
      {client.module->GetTypeDefinition("Event").value(), std::nullopt}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, aliases));
  EXPECT_EQ(CountOccurrences(package, "typedef " + canonical + " Event;"), 1);
  EXPECT_EQ(CountOccurrences(package, "} " + canonical + ";"), 1);
}

TEST_F(DslxToVerilogTest, BatchProtectsSameNameAliasFromQualifiedFamily) {
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/a.x", "pub enum Event { Empty, Item(u8) }"},
      {"/b.x", "pub enum Event { Empty, Item(u16) }"},
      {"/c.x", "pub enum a_Event { Empty, Item(u32) }"}};
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule client,
      ParseAndTypecheck("import a; import b; import c; "
                        "pub type a_Event = c::a_Event;",
                        "client.x", "client", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * a,
                           import_data.Get(ImportTokens({"a"})));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * b,
                           import_data.Get(ImportTokens({"b"})));
  for (bool reverse : {false, true}) {
    std::vector<VerilogExportRequest> requests = {
        {a->module().GetTypeDefinition("Event").value(), std::nullopt},
        {b->module().GetTypeDefinition("Event").value(), std::nullopt},
        {client.module->GetTypeDefinition("a_Event").value(), std::nullopt}};
    if (reverse) {
      std::reverse(requests.begin(), requests.end());
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string package,
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
    EXPECT_EQ(CountOccurrences(package, "} a_Event;"), 1);
    EXPECT_EQ(CountOccurrences(package, "} a_Event__1;"), 1);
    EXPECT_EQ(CountOccurrences(package, "} b_Event;"), 1);
    EXPECT_NE(package.find("a_Event_make_item ("), std::string::npos);
    EXPECT_NE(package.find("a_Event__1_make_item ("), std::string::npos);
  }
}

TEST_F(DslxToVerilogTest, BatchRejectsTokenBearingValueArguments) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#![feature(generics)]
struct V { t: token }
enum Marker<A: V> { Empty, Only(u1) }
const FIRST = V { t: token() };
pub type One = Marker<FIRST>;
enum TypeMarker<T: type> { Empty, Only(u1) }
pub type TokenType = TypeMarker<token>;
)",
                        "test.x", "test", &import_data, nullptr));
  const std::vector<VerilogExportRequest> supported = {
      {tm.module->GetTypeDefinition("TokenType").value(), std::nullopt}};
  EXPECT_TRUE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, supported).ok());
  const std::vector<VerilogExportRequest> rejected = {
      {tm.module->GetTypeDefinition("One").value(), std::nullopt}};
  auto result = ExportDslxTypesToVerilog("test_pkg", &import_data, rejected);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("value argument contains a token"),
            std::string_view::npos);
}

TEST_F(DslxToVerilogTest,
       BatchRejectsAliasesConflictingWithOrdinaryDependencies) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
struct Slot { value: u8 }
enum Seed { Empty, Value(Slot) }
enum Inner { Empty, Item(u8) }
enum Code: u1 { TOKEN = 0 }
fn ordinary(x: (Inner, Code)) -> (Inner, Code) { x }
)",
                        "test.x", "test", &import_data, nullptr));
  TypeDefinition seed = tm.module->GetTypeDefinition("Seed").value();
  Function* ordinary = tm.module->GetFunction("ordinary").value();
  const std::vector<VerilogExportRequest> payload_conflict = {{seed, "Slot"}};
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, payload_conflict)
          .ok());
  const std::vector<VerilogExportRequest> enum_conflict = {
      {seed, "TOKEN"}, {VerilogFunctionOutputExport{ordinary}, "TupleOut"}};
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, enum_conflict).ok());
}

TEST_F(DslxToVerilogTest, SelectedAliasDisplacesSignedArrayCompanion) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("enum Message { Empty, Samples(s8[2]) } "
                        "enum Other { Empty, Item(u8) }",
                        "test.x", "test", &import_data, nullptr));
  for (bool reverse : {false, true}) {
    std::vector<VerilogExportRequest> requests = {
        {tm.module->GetTypeDefinition("Other").value(), "Message_s8_value_t"},
        {tm.module->GetTypeDefinition("Message").value(), std::nullopt}};
    if (reverse) {
      std::reverse(requests.begin(), requests.end());
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string package,
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
    EXPECT_EQ(CountOccurrences(package, "typedef Other Message_s8_value_t;"),
              1);
    EXPECT_NE(package.find("Message_s8_value_t__1"), std::string::npos);
  }
}

TEST_F(DslxToVerilogTest, BatchChecksAliasesAgainstNewGenericDependencies) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
type Batch = Message<u32:16>[2];
struct Existing { field: Message<u32:8> }
fn existing(value: Existing) -> Existing { value }
fn array(value: Batch) -> Batch { value }
fn wide(value: Message<u32:16>) -> Message<u32:16> { value }
)",
                        "test.x", "test", &import_data, nullptr));
  Function* existing = tm.module->GetFunction("existing").value();
  Function* array = tm.module->GetFunction("array").value();
  Function* wide = tm.module->GetFunction("wide").value();
  const std::vector<VerilogExportRequest> probe = {
      {VerilogFunctionOutputExport{wide}, "Concrete"}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string probe_package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, probe));
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe_package, R"(typedef ([^ ]+) Concrete;)",
                                &canonical));
  const std::vector<VerilogExportRequest> allowed = {
      {VerilogFunctionOutputExport{existing}, "First"},
      {VerilogFunctionParamExport{existing, "value"}, "Second"},
      {VerilogFunctionOutputExport{array}, "Allowed"},
      {VerilogFunctionOutputExport{wide}, "Pinned"}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, allowed));
  for (std::string_view alias : {"First", "Second", "Allowed", "Pinned"}) {
    EXPECT_EQ(CountOccurrences(package, absl::StrCat(" ", alias, ";")), 1);
  }
  EXPECT_EQ(CountOccurrences(package, "} " + canonical + ";"), 1);
  const std::vector<VerilogExportRequest> array_conflict = {
      {VerilogFunctionOutputExport{array}, canonical}};
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, array_conflict).ok());
  for (const std::string& alias : {std::string("Pinned"), canonical}) {
    auto rejected = allowed;
    rejected.push_back({VerilogFunctionOutputExport{existing}, alias});
    EXPECT_FALSE(
        ExportDslxTypesToVerilog("test_pkg", &import_data, rejected).ok());
  }
}

TEST_F(DslxToVerilogTest,
       DynamicSumNamesPreserveUnemittedOrdinaryReservations) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#![feature(generics)]
type Message__value_3a_5_3a_u32_3a_8 = u8;
enum Message<N: u32> { Empty, Item(uN[N]) }
fn concrete(value: Message<u32:8>) -> Message<u32:8> { value }
)",
                        "test.x", "test", &import_data, nullptr));
  const std::string ordinary_name = "Message__value_3a_5_3a_u32_3a_8";
  const std::string family = ordinary_name + "__2";
  const std::string tag = family + "_tag_t";
  TypeDefinition ordinary = tm.module->GetTypeDefinition(ordinary_name).value();
  const std::vector<VerilogExportRequest> requests = {
      {ordinary, std::nullopt},
      {ordinary, std::nullopt},
      {ordinary, tag},
      {VerilogFunctionOutputExport{tm.module->GetFunction("concrete").value()},
       "Output"}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
  EXPECT_EQ(CountOccurrences(package, "} " + family + ";"), 1);
  EXPECT_EQ(CountOccurrences(package, "} " + tag + "__1;"), 1);
  EXPECT_EQ(CountOccurrences(package, " " + tag + "__1 tag;"), 1);
  EXPECT_EQ(CountOccurrences(package, "typedef " + family + " Output;"), 1);
}

TEST_F(DslxToVerilogTest, BatchRejectsSanitizedEnumAndFunctionAliasCollisions) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
type Taken = u1;
enum Code: u1 { byte = 0 }
enum Seed { Empty, Item(u1) }
enum Carrier { Value(Code) }
fn identity(value: Seed) -> Seed { value }
fn ordinary(value: (u8, Carrier)) -> (u8, Carrier) { value }
)",
                        "test.x", "test", &import_data, nullptr));
  TypeDefinition seed = tm.module->GetTypeDefinition("Seed").value();
  TypeDefinition taken = tm.module->GetTypeDefinition("Taken").value();
  Function* identity = tm.module->GetFunction("identity").value();
  Function* ordinary = tm.module->GetFunction("ordinary").value();
  const std::vector<VerilogExportRequest> enum_conflict = {
      {seed, "byte_"}, {VerilogFunctionOutputExport{ordinary}, "Output"}};
  auto enum_result =
      ExportDslxTypesToVerilog("test_pkg", &import_data, enum_conflict);
  ASSERT_FALSE(enum_result.ok());
  EXPECT_NE(enum_result.status().message().find("byte_"),
            std::string_view::npos);
  for (const VerilogExportRequest& request :
       {VerilogExportRequest{VerilogFunctionParamExport{identity, "value"},
                             "Taken"},
        VerilogExportRequest{VerilogFunctionOutputExport{identity}, "Taken"}}) {
    const std::vector<VerilogExportRequest> requests = {{taken, std::nullopt},
                                                        request};
    auto result = ExportDslxTypesToVerilog("test_pkg", &import_data, requests);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.status().message().find("Taken"), std::string_view::npos);
  }
}

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
  BatchExportCollector type_to_verilog("test_pkg");

  Function* func = tm.module->GetFunction("add_point_elements").value();

  for (Param* p : func->params()) {
    type_to_verilog.AddTypeForFunctionParam(func, &import_data,
                                            p->name_def()->identifier());
  }

  type_to_verilog.AddTypeForFunctionOutput(func, &import_data,
                                           "user_defined_output_type_t");

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
  BatchExportCollector type_to_verilog("test_pkg");

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(def, &import_data);
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
    BatchExportCollector type_to_verilog("test_pkg");
    if (direct_first) {
      type_to_verilog.AddTypeForTypeDefinition(definitions[1], &import_data);
    } else {
      type_to_verilog.AddTypeForFunctionOutput(function, &import_data,
                                               "output_t");
    }
    type_to_verilog.AddTypeForFunctionParam(function, &import_data, "x",
                                            "input_t");
    if (direct_first) {
      type_to_verilog.AddTypeForFunctionOutput(function, &import_data,
                                               "output_t");
    } else {
      type_to_verilog.AddTypeForTypeDefinition(definitions[1], &import_data);
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
  BatchExportCollector type_to_verilog("test_pkg");
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(definition, &import_data);
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
    BatchExportCollector manager("test_pkg");
    manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().front(),
                                     &import_data);
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
    BatchExportCollector manager("test_pkg");
    if (export_ordinary_first) {
      manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition("index_0").value(), &import_data);
      manager.AddTypeForTypeDefinition(holder_type, &import_data);
      manager.AddTypeForTypeDefinition(standalone_type, &import_data);
      const std::string ordinary = declaration(manager.Emit(), "Holder");
      EXPECT_EQ(CountOccurrences(ordinary, "test_pkg::index_0"), 0) << ordinary;
      EXPECT_EQ(CountOccurrences(ordinary, "index_0 index_1;"), 2) << ordinary;
    }
    manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinition("E").value(),
                                     &import_data);
    if (!export_ordinary_first) {
      manager.AddTypeForTypeDefinition(holder_type, &import_data);
      manager.AddTypeForTypeDefinition(standalone_type, &import_data);
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

    manager.AddTypeForTypeDefinition(
        tm.module->GetTypeDefinition("Direct").value(), &import_data);
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
  BatchExportCollector type_to_verilog("test_pkg");
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data);
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
      BatchExportCollector manager("test_pkg");
      for (std::string_view name :
           sum_first ? std::vector<std::string_view>{"Message", "Second", "E",
                                                     "EAlias"}
                     : std::vector<std::string_view>{"E", "EAlias", "Message",
                                                     "Second"}) {
        manager.AddTypeForTypeDefinition(
            tm.module->GetTypeDefinition(name).value(), &import_data);
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
    BatchExportCollector manager("test_pkg");
    if (sum_first) {
      manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().back(),
                                       &import_data);
    }
    manager.AddTypeForFunctionParam(identity, &import_data, "value", "A");
    manager.AddTypeForFunctionParam(identity, &import_data, "value", "B");
    if (!sum_first) {
      manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().back(),
                                       &import_data);
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
    BatchExportCollector manager("test_pkg");
    for (std::string_view name :
         sum_first
             ? std::vector<std::string_view>{"Message", "Unsigned", "Signed"}
             : std::vector<std::string_view>{"Unsigned", "Signed", "Message"}) {
      manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition(name).value(), &import_data);
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
    BatchExportCollector manager("test_pkg");
    std::vector<TypeDefinition> definitions = tm.module->GetTypeDefinitions();
    if (!sums_first) {
      std::reverse(definitions.begin(), definitions.end());
    }
    for (const TypeDefinition& definition : definitions) {
      manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector type_to_verilog("test_pkg");
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(definition, &import_data);
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
    BatchExportCollector type_to_verilog("test_pkg");
    type_to_verilog.AddTypeForTypeDefinition(definitions[reverse ? 1 : 0],
                                             &import_data);
    type_to_verilog.AddTypeForTypeDefinition(definitions[reverse ? 0 : 1],
                                             &import_data);
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

// Sum qualifier policy changes whether an empty, colliding module contributes
// a qualifier collision. Ordinary payload names still use the whole graph.
TEST_F(DslxToVerilogTest, SumQualifierPolicyIsIndependentOfEmptyOwnerMap) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule first,
      ParseAndTypecheck("pub struct Record { value: u8 } "
                        "pub enum Packet { Empty, Item(Record) }",
                        "first.x", "a.b", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule second,
      ParseAndTypecheck("pub struct Record { value: u16 } "
                        "pub enum Packet { Empty, Item(Record) }",
                        "second.x", "c", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule empty,
      ParseAndTypecheck("", "empty.x", "a_b", &import_data, nullptr));
  const std::vector<std::pair<Module*, TypeInfo*>> modules = {
      {first.module, first.type_info},
      {second.module, second.type_info},
      {empty.module, empty.type_info}};
  BatchExportCollector default_policy("test_pkg");
  BatchExportCollector empty_owners("test_pkg");
  BatchExportCollector same_identifier("test_pkg");
  default_policy.PrepareForModules(modules);
  empty_owners.PrepareForModules(modules, {});
  same_identifier.PrepareForModules(modules, {},
                                    SumQualifierPolicy::kSameIdentifier);

  for (auto [manager, sum_name] : {std::pair{&default_policy, "a_2e_b_Packet"},
                                   std::pair{&empty_owners, "a_2e_b_Packet"},
                                   std::pair{&same_identifier, "a_b_Packet"}}) {
    SCOPED_TRACE(sum_name);
    for (Module* module : {first.module, second.module}) {
      manager->AddTypeForTypeDefinition(
          module->GetTypeDefinition("Packet").value(), &import_data);
    }
    const std::string emitted = manager->Emit();
    EXPECT_EQ(CountOccurrences(emitted, std::string("} ") + sum_name + ";"), 1)
        << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "} c_Packet;"), 1) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "} a_2e_b_Record;"), 1) << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "} c_Record;"), 1) << emitted;
  }
  EXPECT_EQ(default_policy.Emit(), empty_owners.Emit());
}

// A public owner can have no compiler qualifier bucket, or collide with a
// bucket containing a single different compiler owner.
TEST_F(DslxToVerilogTest, SumQualifierPolicyCombinesPublicAndCompilerOwners) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule first,
      ParseAndTypecheck("pub enum Packet { Empty, Item(u8) }", "first.x",
                        "internal", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule second,
      ParseAndTypecheck("pub enum Packet { Empty, Item(u16) }", "second.x", "c",
                        &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule empty,
      ParseAndTypecheck("", "empty.x", "a_b", &import_data, nullptr));
  const std::vector<std::pair<Module*, TypeInfo*>> modules = {
      {first.module, first.type_info},
      {second.module, second.type_info},
      {empty.module, empty.type_info}};
  const TypeDefinition first_packet =
      first.module->GetTypeDefinition("Packet").value();
  const std::map<const AstNode*, std::string> public_owners = {
      {TypeDefinitionToAstNode(first_packet), "a.b"}};
  BatchExportCollector whole_graph("test_pkg");
  BatchExportCollector same_identifier("test_pkg");
  BatchExportCollector no_compiler_bucket("test_pkg");
  whole_graph.PrepareForModules(modules, public_owners,
                                SumQualifierPolicy::kWholeModuleGraph);
  same_identifier.PrepareForModules(modules, public_owners,
                                    SumQualifierPolicy::kSameIdentifier);
  no_compiler_bucket.PrepareForModules(
      {{first.module, first.type_info}, {second.module, second.type_info}},
      public_owners);

  for (auto [manager, sum_name] :
       {std::pair{&whole_graph, "a_2e_b_Packet"},
        std::pair{&same_identifier, "a_b_Packet"},
        std::pair{&no_compiler_bucket, "a_b_Packet"}}) {
    SCOPED_TRACE(sum_name);
    manager->AddTypeForTypeDefinition(first_packet, &import_data);
    manager->AddTypeForTypeDefinition(
        second.module->GetTypeDefinition("Packet").value(), &import_data);
    const std::string emitted = manager->Emit();
    EXPECT_EQ(CountOccurrences(emitted, std::string("} ") + sum_name + ";"), 1)
        << emitted;
    EXPECT_EQ(CountOccurrences(emitted, "} c_Packet;"), 1) << emitted;
    EXPECT_EQ(emitted.find("internal_Packet"), std::string::npos) << emitted;
  }
}

TEST_F(DslxToVerilogTest, ExplicitImportedSumSourceNameIsUsedOrRejected) {
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/a.x", R"(pub enum Duplicate { Empty, Item(u8) }
pub fn identity(value: Duplicate) -> Duplicate { value })"},
      {"/b.x", R"(pub enum Duplicate { Empty, Item(u16) }
pub fn identity(value: Duplicate) -> Duplicate { value })"}};
  auto vfs =
      std::make_unique<FakeFilesystem>(files, std::filesystem::path("/"));
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule client,
      ParseAndTypecheck("import a; import b; pub type Left = a::Duplicate; "
                        "pub type Right = b::Duplicate;",
                        "client.x", "client", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * a,
                           import_data.Get(ImportTokens({"a"})));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * b,
                           import_data.Get(ImportTokens({"b"})));
  VerilogExportOptions options;
  options.roots = {{client.module, client.type_info}};
  for (int entry = 0; entry < 3; ++entry) {
    SCOPED_TRACE(entry);
    auto request = [&](Module* module, std::optional<std::string> alias) {
      if (entry == 0) {
        return VerilogExportRequest{
            module->GetTypeDefinition("Duplicate").value(), std::move(alias)};
      }
      Function* identity = module->GetFunction("identity").value();
      if (entry == 1) {
        return VerilogExportRequest{
            VerilogFunctionParamExport{identity, "value"}, std::move(alias)};
      }
      return VerilogExportRequest{VerilogFunctionOutputExport{identity},
                                  std::move(alias)};
    };
    std::vector<VerilogExportRequest> requests = {
        request(&a->module(), std::nullopt),
        request(&b->module(), std::nullopt), request(&a->module(), "Duplicate"),
        request(&a->module(), "Duplicate"), request(&a->module(), "")};
    if (entry == 0) {
      requests.push_back(
          {client.module->GetTypeDefinition("Left").value(), std::nullopt});
      requests.push_back(
          {client.module->GetTypeDefinition("Right").value(), std::nullopt});
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string package,
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests, options));
    EXPECT_EQ(CountOccurrences(package, "} a_Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(package, "} b_Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(package, "typedef a_Duplicate Duplicate;"), 1);
    EXPECT_EQ(CountOccurrences(package, "typedef a_Duplicate _;"), 1);
    if (entry == 0) {
      EXPECT_EQ(CountOccurrences(package, "typedef a_Duplicate Left;"), 1);
      EXPECT_EQ(CountOccurrences(package, "typedef b_Duplicate Right;"), 1);
    }
    requests.push_back(request(&b->module(), "Duplicate"));
    auto conflict =
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests, options);
    ASSERT_FALSE(conflict.ok());
    EXPECT_NE(conflict.status().message().find("Duplicate"),
              std::string_view::npos);
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
    BatchExportCollector manager("test_pkg");
    auto add = [&](Function* function, std::string_view alias) {
      manager.AddTypeForFunctionParam(function, &import_data, "value", alias);
    };
    if (narrow_first) {
      add(narrow, "Narrow");
      add(wide, "Wide");
    } else {
      add(wide, "Wide");
      add(narrow, "Narrow");
    }
    manager.AddTypeForFunctionOutput(narrow, &import_data, "Again");
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
  BatchExportCollector manager("test_pkg");
  for (std::string_view alias : {"One", "Two", "TupleOne", "TupleTwo"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  for (std::string_view alias : {"Receive", "Send"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  std::vector<std::string> families;
  for (std::string_view alias :
       {"First", "Repeated", "Other", "Nested", "Binding", "Defaulted",
        "OtherValue", "OtherType"}) {
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition(alias));
    manager.AddTypeForTypeDefinition(definition, &import_data);
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
    BatchExportCollector manager("test_pkg");
    for (std::string_view alias : test_case.aliases) {
      XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                               tm.module->GetTypeDefinition(alias));
      manager.AddTypeForTypeDefinition(definition, &import_data);
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

TEST_F(DslxToVerilogTest, OrdinaryDefinitionYieldsToFixedSumAlias) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("type Plain = u8; enum Message { Empty, Item(u8) }",
                        "test_module.x", "test_module", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition plain,
                           tm.module->GetTypeDefinition("Plain"));
  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition message,
                           tm.module->GetTypeDefinition("Message"));
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    std::vector<VerilogExportRequest> requests = {
        {plain, "Slot"}, {message, "Slot"}};
    if (reverse) {
      std::reverse(requests.begin(), requests.end());
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string emitted,
        ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
    EXPECT_THAT(emitted, testing::HasSubstr("typedef logic [7:0] Slot__1;"));
    EXPECT_THAT(emitted, testing::HasSubstr("typedef Message Slot;"));
  }
}

TEST_F(DslxToVerilogTest, ImplicitUnitFunctionOutputReturnsError) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("fn done() { () }", "test_module.x", "test_module",
                        &import_data, nullptr));
  Function* done = tm.module->GetFunction("done").value();
  const std::vector<VerilogExportRequest> requests = {
      {VerilogFunctionOutputExport{done}, std::nullopt}};
  auto result = ExportDslxTypesToVerilog("test_pkg", &import_data, requests);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kUnimplemented);
  EXPECT_EQ(result.status().message(),
            "Zero sized interface type () not supported.");
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
    BatchExportCollector manager("test_pkg");
    XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition definition,
                             tm.module->GetTypeDefinition("Out"));
    manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().front(),
                                   &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().back(),
                                   &import_data);
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
    BatchExportCollector manager("test_pkg");
    const std::vector<TypeDefinition> definitions =
        tm.module->GetTypeDefinitions();
    ASSERT_EQ(definitions.size(), 3);
    if (independent_sum_first) {
      manager.AddTypeForTypeDefinition(definitions[2], &import_data);
    }
    manager.AddTypeForTypeDefinition(definitions[1], &import_data);
    ASSERT_TRUE(tm.module->GetFunction("identity").has_value());
    Function* identity = tm.module->GetFunction("identity").value();
    manager.AddTypeForFunctionParam(identity, &import_data, "value", "Item");
    manager.AddTypeForFunctionParam(identity, &import_data, "value", "E_Item");
    if (!independent_sum_first) {
      manager.AddTypeForTypeDefinition(definitions[2], &import_data);
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
    BatchExportCollector manager("test_pkg");
    const std::vector<TypeDefinition> definitions =
        tm.module->GetTypeDefinitions();
    ASSERT_EQ(definitions.size(), 3);
    manager.AddTypeForTypeDefinition(definitions[sum_first ? 2 : 1],
                                     &import_data);
    manager.AddTypeForTypeDefinition(definitions[sum_first ? 1 : 2],
                                     &import_data);
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
    BatchExportCollector manager("test_pkg");
    for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
      manager.AddTypeForTypeDefinition(definition, &import_data);
    }
    Function* first = tm.module->GetFunction("first").value();
    Function* second = tm.module->GetFunction("second").value();
    manager.AddTypeForFunctionParam(first, &import_data, "value", "Shared");
    manager.AddTypeForFunctionParam(first, &import_data, "value", "Shared");
    manager.AddTypeForFunctionParam(second, &import_data, "value", "Shared");
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
    BatchExportCollector manager("test_pkg");
    for (std::string_view name :
         sum_first
             ? std::vector<std::string_view>{"Message", "Record", "Code"}
             : std::vector<std::string_view>{"Record", "Code", "Message"}) {
      manager.AddTypeForTypeDefinition(
          tm.module->GetTypeDefinition(name).value(), &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().front(),
                                   &import_data);
  std::optional<Function*> function = tm.module->GetFunction("identity");
  ASSERT_TRUE(function.has_value());
  manager.AddTypeForFunctionOutput(*function, &import_data);
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
    BatchExportCollector manager("test_pkg");
    for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
      manager.AddTypeForTypeDefinition(definition, &import_data);
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
    BatchExportCollector manager("test_pkg");
    manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().front(),
                                     &import_data);
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
  BatchExportCollector probe("test_pkg");
  probe.AddTypeForFunctionOutput(concrete, &import_data, "Concrete");
  std::string canonical;
  ASSERT_TRUE(RE2::PartialMatch(probe.Emit(), R"(typedef ([^ ]+) Concrete;)",
                                &canonical));
  ASSERT_NE(canonical, "Message");

  BatchExportCollector manager("test_pkg");
  manager.PrepareForModules({{tm.module, tm.type_info}});
  manager.AddTypeForFunctionOutput(ordinary, &import_data, "Message");
  manager.AddTypeForFunctionOutput(concrete, &import_data, "Concrete");
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] Message;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "typedef " + canonical + " Concrete;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "} " + canonical + ";"), 1) << emitted;
  probe.AddTypeForFunctionParam(ordinary, &import_data, "value", "Message");
  EXPECT_EQ(CountOccurrences(probe.Emit(), "typedef logic [7:0] Message;"), 1)
      << probe.Emit();
}

TEST_F(DslxToVerilogTest, RealSumSymbolsTakePriorityOverGenericNominalNames) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule generic,
      ParseAndTypecheck(R"(#![feature(generics)]
enum Message<N: u32> { Empty, Item(uN[N]) }
fn ordinary(value: u8) -> u8 { value }
)",
                        "a.x", "a", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule fixed,
      ParseAndTypecheck("enum Message { Empty, Item(u8) }", "b.x", "b",
                        &import_data, nullptr));
  Function* ordinary = generic.module->GetFunction("ordinary").value();
  TypeDefinition message = fixed.module->GetTypeDefinition("Message").value();
  VerilogExportOptions options;
  options.roots = {{generic.module, generic.type_info},
                   {fixed.module, fixed.type_info}};
  const std::vector<VerilogExportRequest> allowed = {
      {VerilogFunctionOutputExport{ordinary}, "a_Message"},
      {message, std::nullopt}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, allowed, options));
  EXPECT_EQ(CountOccurrences(package, "} b_Message;"), 1);
  for (std::string_view symbol : {"b_Message", "b_Message_tag_t"}) {
    const std::vector<VerilogExportRequest> rejected = {
        {VerilogFunctionOutputExport{ordinary}, std::string(symbol)},
        {message, std::nullopt}};
    EXPECT_FALSE(
        ExportDslxTypesToVerilog("test_pkg", &import_data, rejected, options)
            .ok());
  }
}

TEST_F(DslxToVerilogTest, OrdinaryFunctionOutputReusesCrossModuleTypedef) {
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
  BatchExportCollector manager("test_pkg");
  manager.PrepareForModules(
      {{first.module, first.type_info}, {second.module, second.type_info}});
  Function* ordinary = first.module->GetFunction("ordinary").value();
  manager.AddTypeForFunctionOutput(ordinary, &cross_module_imports);
  const std::string ordinary_only = manager.Emit();
  manager.AddTypeForFunctionOutput(ordinary, &cross_module_imports);
  EXPECT_EQ(manager.Emit(), ordinary_only);
  manager.AddTypeForTypeDefinition(
      first.module->GetTypeDefinition("Envelope").value(),
      &cross_module_imports);
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] Envelope_tag_t;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, " Envelope_tag_t;"), 1) << emitted;
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
  BatchExportCollector manager("test_pkg");
  manager.PrepareForModules(
      {{first.module, first.type_info}, {second.module, second.type_info}});
  manager.AddTypeForTypeDefinition(
      first.module->GetTypeDefinition(ordinary).value(), &import_data);
  Function* plain = second.module->GetFunction("plain").value();
  manager.AddTypeForFunctionOutput(plain, &import_data, fixed);
  manager.AddTypeForTypeDefinition(
      second.module->GetTypeDefinition(ordinary).value(), &import_data);
  ASSERT_EQ(CountOccurrences(manager.Emit(), " " + fixed + ";"), 2)
      << manager.Emit();

  manager.AddTypeForTypeDefinition(
      second.module->GetTypeDefinition("Carrier").value(), &import_data);
  ASSERT_EQ(CountOccurrences(manager.Emit(), " " + fixed + ";"), 1)
      << manager.Emit();
  manager.AddTypeForFunctionOutput(
      second.module->GetFunction("generic").value(), &import_data);
  manager.AddTypeForFunctionOutput(plain, &import_data, fixed);
  const std::string emitted = manager.Emit();
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic [7:0] " + fixed + ";"), 2)
      << emitted;
  EXPECT_EQ(CountOccurrences(emitted, " " + fixed + ";"), 2) << emitted;
  EXPECT_EQ(CountOccurrences(emitted, "} " + ordinary + "__2;"), 1) << emitted;
}

TEST_F(DslxToVerilogTest, IndependentRecordSumsIgnoreUnrelatedPackageNames) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
enum Old: u8 { Anchor = 0 }
enum Prelude { None, Some(Old) }
struct Anchor { value: u8 }
enum Overlap { None, Some(Anchor) }
struct AliasCollision { value: u8 }
enum AliasOverlap { None, Some(AliasCollision) }
struct Record0 { value: u8 }
enum Item0 { None, Some(Record0) }
struct Record1 { value: u8 }
enum Item1 { None, Some(Record1) }
)",
                        "test.x", "test", &import_data, nullptr));
  auto definition = [&](std::string_view name) {
    return tm.module->GetTypeDefinition(name).value();
  };
  std::vector<VerilogExportRequest> requests = {
      {definition("Prelude"), "AliasCollision"},
      {definition("Item0"), std::nullopt},
      {definition("Item1"), std::nullopt},
      {definition("Overlap"), std::nullopt}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
  EXPECT_EQ(CountOccurrences(package, "typedef Prelude AliasCollision;"), 1);
  EXPECT_TRUE(RE2::PartialMatch(package, R"(\n\s+Old_Anchor = 8'h00)"));
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(CountOccurrences(package, absl::StrFormat("} Record%d;", i)), 1);
    EXPECT_EQ(CountOccurrences(package, absl::StrFormat("} Item%d;", i)), 1);
  }
  requests.push_back({definition("AliasOverlap"), std::nullopt});
  EXPECT_FALSE(
      ExportDslxTypesToVerilog("test_pkg", &import_data, requests).ok());
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
  BatchExportCollector manager("test_pkg");
  const TypeDefinition record = tm.module->GetTypeDefinition("byte").value();
  manager.AddTypeForTypeDefinition(record, &import_data);
  const std::string ordinary_record = manager.Emit();
  ASSERT_EQ(CountOccurrences(ordinary_record, "} byte;"), 1) << ordinary_record;
  ASSERT_EQ(CountOccurrences(ordinary_record, "} byte_;"), 0)
      << ordinary_record;
  manager.AddTypeForTypeDefinition(record, &import_data, "byte_");
  ASSERT_EQ(manager.Emit(), ordinary_record);
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinition("Code").value(),
                                   &import_data);
  const std::string before = manager.Emit();
  ASSERT_TRUE(RE2::PartialMatch(before, R"(\n\s+wire = 8'h00)")) << before;

  manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinition("Code").value(),
                                   &import_data);
  const TypeDefinition record =
      imported->module->GetTypeDefinition("Record").value();
  manager.AddTypeForTypeDefinition(record, &import_data);
  const std::string before = manager.Emit();
  ASSERT_TRUE(RE2::PartialMatch(before, R"(\n\s+Common = 8'h00)")) << before;
  ASSERT_EQ(CountOccurrences(before, "} Record;"), 1) << before;
  ASSERT_EQ(CountOccurrences(before, "} a_Record;"), 0) << before;
  manager.AddTypeForTypeDefinition(record, &import_data, "a_Record");
  ASSERT_EQ(manager.Emit(), before);

  manager.AddTypeForTypeDefinition(
      tm.module->GetTypeDefinition("Message").value(), &import_data);
  const std::string after = manager.Emit();
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+Code_Common = 8'h00)")) << after;
  EXPECT_TRUE(RE2::PartialMatch(after, R"(\n\s+Other_Common = 8'h00)"))
      << after;
  EXPECT_EQ(CountOccurrences(after, "} a_Record;"), 1) << after;
  EXPECT_EQ(CountOccurrences(after, "} Message;"), 1) << after;
}

TEST_F(DslxToVerilogTest, OuterOrdinaryAliasUsesFinalNestedEnumProjection) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
enum Seed { Empty, Item(u1) }
type Prelude = u8;
enum E: u1 { Foo = 0 }
enum Carrier { Value(E) }
fn ordinary(value: (Prelude, Carrier)) -> (Prelude, Carrier) { value }
)",
                        "test.x", "test", &import_data, nullptr));
  TypeDefinition seed = tm.module->GetTypeDefinition("Seed").value();
  Function* ordinary = tm.module->GetFunction("ordinary").value();
  const std::vector<VerilogExportRequest> allowed = {
      {seed, "E_Foo"}, {VerilogFunctionOutputExport{ordinary}, "Output"}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, allowed));
  EXPECT_EQ(CountOccurrences(package, " Prelude;"), 1);
  EXPECT_EQ(CountOccurrences(package, "} Carrier;"), 1);
  EXPECT_TRUE(RE2::PartialMatch(package, R"(\n\s+Foo = 1'h0)"));
  const std::vector<VerilogExportRequest> rejected = {
      {seed, "E_Foo"}, {VerilogFunctionOutputExport{ordinary}, "Foo"}};
  auto result = ExportDslxTypesToVerilog("test_pkg", &import_data, rejected);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("E_Foo"), std::string_view::npos);
}

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
    BatchExportCollector manager("test_pkg");
    manager.PrepareForModules(
        {{other.module, other.type_info}, {owner.module, owner.type_info}});
    for (Module* module :
         (owner_first ? std::vector{owner.module, other.module}
                      : std::vector{other.module, owner.module})) {
      for (const TypeDefinition& definition : module->GetTypeDefinitions()) {
        manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  for (const TypeDefinition& definition : tm.module->GetTypeDefinitions()) {
    manager.AddTypeForTypeDefinition(definition, &import_data);
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
  BatchExportCollector manager("test_pkg");
  manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinitions().front(),
                                   &import_data);
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
  BatchExportCollector type_to_verilog("test_pkg");
  const std::vector<TypeDefinition> definitions =
      tm.module->GetTypeDefinitions();
  ASSERT_EQ(definitions.size(), 2);
  const std::vector<VerilogExportRequest> standalone = {
      {definitions[0], std::nullopt}};
  const auto standalone_result =
      ExportDslxTypesToVerilog("test_pkg", &import_data, standalone);
  ASSERT_FALSE(standalone_result.ok());
  EXPECT_NE(
      standalone_result.status().message().find("Zero sized interface type"),
      std::string_view::npos)
      << standalone_result.status();
  type_to_verilog.AddTypeForTypeDefinition(definitions[1], &import_data);

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
  BatchExportCollector type_to_verilog("test_pkg");

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(def, &import_data);
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
  BatchExportCollector type_to_verilog("test_pkg");

  Function* func = tm.module->GetFunction("f").value();

  type_to_verilog.AddTypeForFunctionOutput(func, &import_data,
                                           "user_defined_name_t");

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
  BatchExportCollector type_to_verilog("test_pkg");

  Function* func = tm.module->GetFunction("f").value();

  type_to_verilog.AddTypeForFunctionParam(func, &import_data, "a");

  type_to_verilog.AddTypeForFunctionParam(func, &import_data, "b");

  type_to_verilog.AddTypeForFunctionParam(func, &import_data, "c");

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
  BatchExportCollector type_to_verilog("test_pkg");

  Function* func = tm.module->GetFunction("f").value();

  type_to_verilog.AddTypeForFunctionParam(func, &import_data, "a");
  type_to_verilog.AddTypeForFunctionParam(func, &import_data, "b");

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
  BatchExportCollector type_to_verilog("test_pkg");

  for (const TypeDefinition& def : tm.module->GetTypeDefinitions()) {
    type_to_verilog.AddTypeForTypeDefinition(def, &import_data);
  }

  ExpectEqualToGoldenFile(GoldenFilePath("vtxt"), type_to_verilog.Emit());
}

TEST_F(DslxToVerilogTest, PayloadMemberNamesRespectDeclarationOrder) {
  const std::string emitted =
      DslxTypeToVerilogManagerTestPeer::ProjectedMemberNameScopes();
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    logic Token__1;
    Token value;
    Token [1:0] values;
  } NamedBefore;)"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    Token value;
    Token [1:0] values;
    logic Token;
  } NamedAfter;)"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    logic index_0;
    test_pkg::index_0 index_1;
    test_pkg::index_0 [1:0] index_2;
  } FixedBefore;)"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    index_0 [1:0] index_0;
    logic index_1;
  } FixedAfter;)"),
            std::string::npos)
      << emitted;
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    struct packed {
      logic index_0;
      test_pkg::index_0 index_1;
    } [1:0] tuples;
  } Ordinary;)"),
            std::string::npos)
      << emitted;
}

TEST_F(DslxToVerilogTest, PayloadProjectionPreservesSignedSelectionsAndTuples) {
  constexpr std::string_view program = R"(
enum Payload {
  Scalar(s1),
  Packed(s8[2][3]),
  Repeat(s8[4]),
  Tuple((s8, uN[0], u3[2], s8[2])),
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string emitted,
      DslxTypeToVerilogManagerTestPeer::ProjectedPayloadTypes(
          ConcreteSum(tm, "Payload"), &import_data));
  EXPECT_EQ(CountOccurrences(emitted, "typedef logic signed Scalar;"), 1)
      << emitted;
  EXPECT_EQ(CountOccurrences(
                emitted, "typedef logic signed [7:0] Projection_s8_value_t;"),
            1)
      << emitted;
  EXPECT_EQ(CountOccurrences(
                emitted, "typedef Projection_s8_value_t [2:0][1:0] Packed;"),
            1)
      << emitted;
  EXPECT_EQ(
      CountOccurrences(emitted, "typedef Projection_s8_value_t [3:0] Repeat;"),
      1)
      << emitted;
  EXPECT_NE(emitted.find(R"(typedef struct packed {
    logic signed [7:0] index_0;
    logic [1:0][2:0] index_2;
    Projection_s8_value_t [1:0] index_3;
  } Tuple;)"),
            std::string::npos)
      << emitted;
}

TEST_F(DslxToVerilogTest, ConcreteGenericInsideOrdinaryWrapper) {
  constexpr std::string_view program = R"(#![feature(generics)]
struct Generic<N: u32> { value: uN[N] }
struct Wrapper { inner: Generic<u32:8> }
pub enum Direct { Value(Generic<u32:8>) }
pub enum Wrapped { Value(Wrapper) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "test.x", "test", &import_data, nullptr));
  for (const std::string name : {"Direct", "Wrapped"}) {
    SCOPED_TRACE(name);
    BatchExportCollector manager("test_pkg");
    manager.AddTypeForTypeDefinition(tm.module->GetTypeDefinition(name).value(),
                                     &import_data);
    const std::string emitted = manager.Emit();
    // A wrapper preserves the same concrete field and complete payload width.
    EXPECT_TRUE(RE2::PartialMatch(emitted, R"(logic \[7:0\] value;)"))
        << emitted;
    EXPECT_TRUE(RE2::PartialMatch(emitted, R"(logic \[7:0\] bits;)"))
        << emitted;
  }
}

TEST_F(DslxToVerilogTest, SignedScalarsNeedNoAdditionalNames) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("struct Record { value: s8 } "
                        "enum Scalar { Value(s8) } "
                        "enum Wrapped { Value(Record) }",
                        "test.x", "test", &import_data, nullptr));
  const std::vector<VerilogExportRequest> requests = {
      {tm.module->GetTypeDefinition("Scalar").value(), std::nullopt},
      {tm.module->GetTypeDefinition("Wrapped").value(), std::nullopt}};
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string package,
      ExportDslxTypesToVerilog("test_pkg", &import_data, requests));
  EXPECT_NE(package.find("logic signed [7:0] value;"), std::string::npos);
  EXPECT_EQ(package.find("Scalar_s8_value_t"), std::string::npos);
  EXPECT_NE(package.find("Wrapped_Record_value_t"), std::string::npos);
}

}  // namespace
}  // namespace xls::dslx
