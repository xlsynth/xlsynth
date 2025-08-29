// Copyright 2025 The XLS Authors
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

#include "xls/dslx/replace_invocations.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

struct PT {
  std::unique_ptr<ImportData> import_data;
  TypecheckedModule tm;
};

absl::StatusOr<PT> ParseTypecheck(std::string text) {
  std::filesystem::path stdlib = std::string(::xls::kDefaultDslxStdlibPath);
  auto import_data = std::make_unique<ImportData>(CreateImportData(
      stdlib, /*additional_search_paths=*/std::vector<std::filesystem::path>{},
      kAllWarningsSet, std::make_unique<RealFilesystem>()));
  XLS_ASSIGN_OR_RETURN(
      TypecheckedModule tm,
      ParseAndTypecheck(text, /*path=*/"test.x", /*module_name=*/"test",
                        import_data.get()));
  return PT{.import_data = std::move(import_data), .tm = std::move(tm)};
}

TEST(ReplaceInvocationsTest, NonParametricSimpleReplacement) {
  const std::string kText = R"(// test
fn a(x: u32) -> u32 { x + u32:1 }
fn b(x: u32) -> u32 { x + u32:2 }
fn caller(x: u32) -> u32 { b(x) + b(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  ASSERT_NE(m->GetFunction("caller"), std::nullopt);
  ASSERT_NE(m->GetFunction("a"), std::nullopt);
  ASSERT_NE(m->GetFunction("b"), std::nullopt);
  Function* caller = m->GetFunction("caller").value();
  Function* a = m->GetFunction("a").value();
  Function* b = m->GetFunction("b").value();

  InvocationRewriteRule rule;
  rule.from_callee = b;
  rule.to_callee = a;

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  ASSERT_NE(new_module->GetFunction("caller"), std::nullopt);
  Function* caller_new = new_module->GetFunction("caller").value();

  // Expect both b(x) calls to be replaced by a(x).
  int b_uses = 0;
  int a_uses = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    std::string callee_s = inv->callee()->ToString();
    if (callee_s == "a") a_uses++;
    if (callee_s == "b") b_uses++;
  }
  EXPECT_EQ(a_uses, 2);
  EXPECT_EQ(b_uses, 0);
}

TEST(ReplaceInvocationsTest, ParametricFilterMatchesOnlyOne) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> (u8, u16) {
  let y8 = id<u32:8>(u8:1);
  let y16 = id<u32:16>(u16:2);
  (y8, y16)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  ASSERT_NE(m->GetFunction("caller"), std::nullopt);
  ASSERT_NE(m->GetFunction("id"), std::nullopt);
  ASSERT_NE(m->GetFunction("id2"), std::nullopt);
  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  rule.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 8)}});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  ASSERT_NE(new_module->GetFunction("caller"), std::nullopt);
  Function* caller_new = new_module->GetFunction("caller").value();

  int id_uses = 0;
  int id2_uses = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    std::string callee_s = inv->callee()->ToString();
    if (callee_s == "id") id_uses++;
    if (callee_s == "id2") id2_uses++;
  }
  // Current behavior replaces only the N=8 invocation.
  EXPECT_EQ(id2_uses, 1);
  EXPECT_EQ(id_uses, 1);
}

TEST(ReplaceInvocationsTest,
     ParametricReplacementNoToEnvRetainsExplicitParams) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> u8 { id<u32:8>(u8:1) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  // Required filter for the existing invocation (N=8)
  rule.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 8)}});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  Function* caller_new = new_module->GetFunction("caller").value();
  int num_checked = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() != "id2") continue;
    // Since to_callee_env is not provided, retain original explicit
    // parametrics.
    EXPECT_FALSE(inv->explicit_parametrics().empty());
    num_checked++;
  }
  EXPECT_EQ(num_checked, 1);
}

TEST(ReplaceInvocationsTest,
     ParametricReplacementEmptyToEnvDropsExplicitParams) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> u8 { id<u32:8>(u8:1) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  // No explicit env provided (empty set), emit no explicit parametrics and rely
  // on deduction.
  rule.to_callee_env = ParametricEnv();

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  Function* caller_new = new_module->GetFunction("caller").value();
  int num_checked = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() != "id2") continue;
    EXPECT_TRUE(inv->explicit_parametrics().empty());
    num_checked++;
  }
  EXPECT_EQ(num_checked, 1);
}

TEST(ReplaceInvocationsTest, ParametricReplacementWithDeductionWorks) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller(x: u32) -> u32 { id(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  // Required filter: deduced N=32 for u32 param
  rule.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 32)}});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  int num_id2 = 0;
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() == "id2") {
      EXPECT_TRUE(inv->explicit_parametrics().empty());
      num_id2++;
    }
  }
  EXPECT_EQ(num_id2, 1);
}

TEST(ReplaceInvocationsTest, EmptyMatchEnvMatchesOnlyNonParamCallee) {
  const std::string kText = R"(// test
fn a(x: u32) -> u32 { x + u32:1 }
fn b(x: u32) -> u32 { x + u32:2 }
fn caller(x: u32) -> u32 { b(x) + b(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* a = m->GetFunction("a").value();
  Function* b = m->GetFunction("b").value();

  InvocationRewriteRule rule;
  rule.from_callee = b;
  rule.to_callee = a;
  rule.match_callee_env = ParametricEnv();  // explicit empty env

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int a_uses = 0;
  int b_uses = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    std::string callee_s = inv->callee()->ToString();
    if (callee_s == "a") a_uses++;
    if (callee_s == "b") b_uses++;
  }
  EXPECT_EQ(a_uses, 2);
  EXPECT_EQ(b_uses, 0);
}

TEST(ReplaceInvocationsTest, EmptyMatchEnvDoesNotMatchParametricCallee) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> (u8, u16) { (id<u32:8>(u8:1), id<u32:16>(u16:2)) }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  rule.match_callee_env =
      ParametricEnv();  // explicit empty -> invalid on parametric

  auto status_or =
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw");
  EXPECT_THAT(status_or,
              ::absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ReplaceInvocationsTest, ParametricFilterMatchesOnlyOne_Deduced) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> (u8, u16) {
  let a: u8 = u8:1;
  let b: u16 = u16:2;
  (id(a), id(b))
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule2;
  rule2.from_callee = id;
  rule2.to_callee = id2;
  // Match only the deduced N=8 call.
  rule2.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 8)}});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule2, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int id_uses = 0;
  int id2_uses = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    std::string callee_s = inv->callee()->ToString();
    if (callee_s == "id") id_uses++;
    if (callee_s == "id2") id2_uses++;
  }
  EXPECT_EQ(id2_uses, 1);
  EXPECT_EQ(id_uses, 1);
}

TEST(ReplaceInvocationsTest, ParametricEnumFilterAndExplicitReplacement) {
  const std::string kText = R"(// test
enum E : u2 {
  A = 0,
  B = 1,
}
fn f<N: E>(x: u32) -> u32 { x }
fn g<N: E>(x: u32) -> u32 { x }
fn caller(x: u32) -> (u32, u32) {
  let a = f<E::A>(x);
  let b = f<E::B>(x);
  (a, b)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* f = m->GetFunction("f").value();
  Function* g = m->GetFunction("g").value();

  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition td, m->GetTypeDefinition("E"));
  EnumDef* e_def = std::get<EnumDef*>(td);

  InterpValue enum_a =
      InterpValue::MakeEnum(xls::UBits(/*value=*/0, /*bit_count=*/2),
                            /*is_signed=*/false, e_def);

  InvocationRewriteRule rule;
  rule.from_callee = f;
  rule.to_callee = g;
  // Replace only the N=E::A specialization and preserve explicit parametrics
  // for the replacement via to_callee_env.
  rule.match_callee_env = ParametricEnv(
      absl::flat_hash_map<std::string, InterpValue>{{"N", enum_a}});
  rule.to_callee_env = rule.match_callee_env;

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int count_f = 0;
  int count_g = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    std::string callee_s = inv->callee()->ToString();
    if (callee_s == "f") count_f++;
    if (callee_s == "g") {
      count_g++;
      EXPECT_EQ(inv->explicit_parametrics().size(), 1);
    }
  }
  EXPECT_EQ(count_g, 1);
  EXPECT_EQ(count_f, 1);
}

TEST(ReplaceInvocationsTest, ReplaceAllParametricWhenNoMatchEnvExplicit) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> (u8, u16) { (id<u32:8>(u8:1), id<u32:16>(u16:2)) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  // match_callee_env unset -> match all instantiations
  // to_callee_env unset -> retain explicit parametrics

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int id2_uses = 0;
  int with_explicit = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() == "id2") {
      id2_uses++;
      if (!inv->explicit_parametrics().empty()) with_explicit++;
    }
  }
  EXPECT_EQ(id2_uses, 2);
  EXPECT_EQ(with_explicit, 2);
}

TEST(ReplaceInvocationsTest, ReplaceAllParametricWhenNoMatchEnvDeduced) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller() -> (u8, u16) {
  let a: u8 = u8:1;
  let b: u16 = u16:2;
  (id(a), id(b))
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule;
  rule.from_callee = id;
  rule.to_callee = id2;
  // match_callee_env unset -> match all; to_callee_env unset -> retain

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int id2_uses = 0;
  int num_with_params = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() == "id2") {
      id2_uses++;
      if (!inv->explicit_parametrics().empty()) num_with_params++;
    }
  }
  EXPECT_EQ(id2_uses, 2);
  EXPECT_EQ(num_with_params, 0);
}

TEST(ReplaceInvocationsTest, BitsExplicitParamUnsigned) {
  const std::string kText = R"(// test
fn f<N: u32>(x: u32) -> u32 { x }
fn g<N: u32>(x: u32) -> u32 { x }
fn caller(x: u32) -> u32 { f<u32:1>(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* f = m->GetFunction("f").value();
  Function* g = m->GetFunction("g").value();

  InvocationRewriteRule rule;
  rule.from_callee = f;
  rule.to_callee = g;
  rule.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 1)}});
  rule.to_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"N", InterpValue::MakeUBits(32, 8)}});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;
  Function* caller_new = new_module->GetFunction("caller").value();
  int num_checked = 0;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto nodes, CollectUnder(caller_new->body(), /*want_types=*/false));
  for (AstNode* n : nodes) {
    auto* inv = dynamic_cast<Invocation*>(n);
    if (inv == nullptr) continue;
    if (inv->callee()->ToString() != "g") continue;
    EXPECT_EQ(inv->explicit_parametrics().size(), 1);
    EXPECT_NE(inv->ToString().find("u32:8"), std::string::npos);
    num_checked++;
  }
  EXPECT_EQ(num_checked, 1);
}

TEST(ReplaceInvocationsTest, MatchEnvBadOrderErrors) {
  const std::string kText = R"(// test
fn id<B: u32, A: u32>(x: u32) -> u32 { x }
fn id2<B: u32, A: u32>(x: u32) -> u32 { x }
fn caller() -> u32 { id<u32:1, u32:2>(u32:0) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();

  InvocationRewriteRule rule2;
  rule2.from_callee = id;
  rule2.to_callee = id2;
  // Provide match env in A,B order while callee bindings are B,A -> invalid
  rule2.match_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"A", InterpValue::MakeUBits(32, 2)},
          {"B", InterpValue::MakeUBits(32, 1)}});

  auto status_or =
      ReplaceInvocationsInModule(pt.tm, caller, rule2, *pt.import_data,
                                 "test.rw");
  EXPECT_THAT(status_or,
              ::absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ReplaceInvocationsTest, ToEnvMissingRequiredBindingErrors) {
  const std::string kText = R"(// test
fn f<M: u32, K: u32>(x: u32) -> u32 { x }
fn g<M: u32, K: u32>(x: u32) -> u32 { x }
fn caller() -> u32 { f<u32:1, u32:1>(u32:0) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* f = m->GetFunction("f").value();
  Function* g = m->GetFunction("g").value();

  InvocationRewriteRule rule;
  rule.from_callee = f;
  rule.to_callee = g;
  // Only provide optional K, omit required M -> error
  rule.to_callee_env =
      ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
          {"K", InterpValue::MakeUBits(32, 1)}});

  auto status_or =
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw");
  EXPECT_THAT(status_or,
              ::absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ReplaceInvocationsTest, EnumToEnvNoMemberErrors) {
  const std::string kText = R"(// test
enum E : u2 {
  A = 0,
  B = 1,
}
fn f<N: E>(x: u32) -> u32 { x }
fn g<N: E>(x: u32) -> u32 { x }
fn caller(x: u32) -> u32 { f<E::A>(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller = m->GetFunction("caller").value();
  Function* f = m->GetFunction("f").value();
  Function* g = m->GetFunction("g").value();

  XLS_ASSERT_OK_AND_ASSIGN(TypeDefinition td, m->GetTypeDefinition("E"));
  EnumDef* e_def = std::get<EnumDef*>(td);
  // Make enum value with underlying 2, which has no member mapping
  InterpValue enum_bad =
      InterpValue::MakeEnum(xls::UBits(/*value=*/2, /*bit_count=*/2),
                            /*is_signed=*/false, e_def);

  InvocationRewriteRule rule;
  rule.from_callee = f;
  rule.to_callee = g;
  rule.to_callee_env = ParametricEnv(
      absl::flat_hash_map<std::string, InterpValue>{{"N", enum_bad}});

  auto status_or =
      ReplaceInvocationsInModule(pt.tm, caller, rule, *pt.import_data,
                                 "test.rw");
  EXPECT_THAT(status_or,
              ::absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ReplaceInvocationsTest, BulkMultipleCallersMultipleRules) {
  const std::string kText = R"(// test
fn f(x: u32) -> u32 { x + u32:1 }
fn g(x: u32) -> u32 { x + u32:2 }
fn h(x: u32) -> u32 { x + u32:3 }
fn caller1(x: u32) -> u32 { f(x) + g(x) }
fn caller2(x: u32) -> u32 { f(x) + h(x) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller1 = m->GetFunction("caller1").value();
  Function* caller2 = m->GetFunction("caller2").value();
  Function* f = m->GetFunction("f").value();
  Function* g = m->GetFunction("g").value();
  Function* h = m->GetFunction("h").value();

  std::vector<const Function*> callers{caller1, caller2};
  std::vector<InvocationRewriteRule> rules;
  rules.push_back(InvocationRewriteRule{.from_callee = f, .to_callee = g});
  rules.push_back(InvocationRewriteRule{.from_callee = g, .to_callee = h});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, absl::MakeSpan(callers),
                                 absl::MakeSpan(rules), *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  auto check_counts = [&](std::string caller_name, int expect_f, int expect_g,
                          int expect_h) {
    Function* c = new_module->GetFunction(caller_name).value();
    int f_uses = 0, g_uses = 0, h_uses = 0;
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes,
                             CollectUnder(c->body(), /*want_types=*/false));
    for (AstNode* n : nodes) {
      auto* inv = dynamic_cast<Invocation*>(n);
      if (inv == nullptr) continue;
      std::string s = inv->callee()->ToString();
      if (s == "f") f_uses++;
      if (s == "g") g_uses++;
      if (s == "h") h_uses++;
    }
    EXPECT_EQ(f_uses, expect_f);
    EXPECT_EQ(g_uses, expect_g);
    EXPECT_EQ(h_uses, expect_h);
  };

  // f->g, and g->h (first-match-wins per callsite):
  // caller1: f(x)+g(x) -> g(x)+h(x)
  // caller2: f(x)+h(x) -> g(x)+h(x)
  check_counts("caller1", /*f=*/0, /*g=*/1, /*h=*/1);
  check_counts("caller2", /*f=*/0, /*g=*/1, /*h=*/1);
}

TEST(ReplaceInvocationsTest, BulkParametricMatchAcrossCallers) {
  const std::string kText = R"(// test
fn id<N: u32>(x: uN[N]) -> uN[N] { x }
fn id2<N: u32>(x: uN[N]) -> uN[N] { x }
fn id3<N: u32>(x: uN[N]) -> uN[N] { x }
fn caller1() -> (u8, u16) { (id<u32:8>(u8:1), id<u32:16>(u16:2)) }
fn caller2() -> (u16, u8) { (id<u32:16>(u16:3), id<u32:8>(u8:4)) }
)";
  XLS_ASSERT_OK_AND_ASSIGN(PT pt, ParseTypecheck(kText));
  Module* m = pt.tm.module;
  TypeInfo* ti = pt.tm.type_info;

  Function* caller1 = m->GetFunction("caller1").value();
  Function* caller2 = m->GetFunction("caller2").value();
  Function* id = m->GetFunction("id").value();
  Function* id2 = m->GetFunction("id2").value();
  Function* id3 = m->GetFunction("id3").value();

  std::vector<const Function*> callers{caller1, caller2};
  std::vector<InvocationRewriteRule> rules;
  rules.push_back(InvocationRewriteRule{
      .from_callee = id,
      .to_callee = id2,
      .match_callee_env =
          ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
              {"N", InterpValue::MakeUBits(32, 8)}})});
  rules.push_back(InvocationRewriteRule{
      .from_callee = id,
      .to_callee = id3,
      .match_callee_env =
          ParametricEnv(absl::flat_hash_map<std::string, InterpValue>{
              {"N", InterpValue::MakeUBits(32, 16)}})});

  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule new_tm,
      ReplaceInvocationsInModule(pt.tm, absl::MakeSpan(callers),
                                 absl::MakeSpan(rules), *pt.import_data,
                                 "test.rw"));
  Module* new_module = new_tm.module;

  auto count_in_caller =
      [&](std::string caller_name) -> std::tuple<int, int, int> {
    Function* c = new_module->GetFunction(caller_name).value();
    int id_uses = 0, id2_uses = 0, id3_uses = 0;
    auto nodes_or = CollectUnder(c->body(), /*want_types=*/false);
    EXPECT_TRUE(nodes_or.ok());
    auto nodes = nodes_or.value();
    for (AstNode* n : nodes) {
      auto* inv = dynamic_cast<Invocation*>(n);
      if (inv == nullptr) continue;
      std::string s = inv->callee()->ToString();
      if (s == "id") id_uses++;
      if (s == "id2") id2_uses++;
      if (s == "id3") id3_uses++;
    }
    return std::tuple<int, int, int>{id_uses, id2_uses, id3_uses};
  };

  auto [u1, u2, u3] = count_in_caller("caller1");
  EXPECT_EQ(u1, 0);
  EXPECT_EQ(u2, 1);
  EXPECT_EQ(u3, 1);

  auto [v1, v2, v3] = count_in_caller("caller2");
  EXPECT_EQ(v1, 0);
  EXPECT_EQ(v2, 1);
  EXPECT_EQ(v3, 1);
}

}  // namespace
}  // namespace xls::dslx
