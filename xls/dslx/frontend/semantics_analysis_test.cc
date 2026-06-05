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

#include "xls/dslx/frontend/semantics_analysis.h"

#include <memory>
#include <string_view>
#include <variant>

#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/warning_collector.h"

namespace xls::dslx {
namespace {

bool ContainsIfLet(const AstNode* node) {
  if (const auto* conditional = dynamic_cast<const Conditional*>(node);
      conditional != nullptr && conditional->IsIfLet()) {
    return true;
  }
  for (const AstNode* child : node->GetChildren(/*want_types=*/true)) {
    if (ContainsIfLet(child)) {
      return true;
    }
  }
  return false;
}

TEST(SemanticsAnalysisTest, NormalizesIfLetToMatchBeforeTypecheck) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u8),
}

fn unwrap_or(x: Option, y: Option) -> u8 {
  if let Option::Some(v) = x {
    v
  } else if let Option::Some(w) = y {
    w
  } else {
    u8:0
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Module> module,
                           ParseModule(kProgram, "fake_path.x", "the_module",
                                       import_data.file_table()));
  WarningCollector warnings(import_data.enabled_warnings());
  SemanticsAnalysis analysis(/*suppress_warnings=*/false);
  Module* original_module = module.get();
  XLS_ASSERT_OK_AND_ASSIGN(
      module,
      analysis.RunPreTypeCheckPass(std::move(module), warnings, import_data));
  EXPECT_NE(module.get(), original_module);
  EXPECT_FALSE(ContainsIfLet(module.get()));

  XLS_ASSERT_OK_AND_ASSIGN(Function * unwrap_or,
                           module->GetMemberOrError<Function>("unwrap_or"));
  const Statement* body_expr = unwrap_or->body()->statements().back();
  ASSERT_TRUE(std::holds_alternative<Expr*>(body_expr->wrapped()));
  const auto* outer_match =
      dynamic_cast<const Match*>(std::get<Expr*>(body_expr->wrapped()));
  ASSERT_NE(outer_match, nullptr);
  ASSERT_EQ(outer_match->arms().size(), 2);
  EXPECT_TRUE(std::holds_alternative<SumVariantPayloadPattern*>(
      outer_match->arms()[0]->patterns()[0]->leaf()));
  EXPECT_TRUE(outer_match->arms()[1]->patterns()[0]->IsWildcardLeaf());

  const auto* inner_match =
      dynamic_cast<const Match*>(outer_match->arms()[1]->expr());
  ASSERT_NE(inner_match, nullptr);
  ASSERT_EQ(inner_match->arms().size(), 2);
  EXPECT_TRUE(std::holds_alternative<SumVariantPayloadPattern*>(
      inner_match->arms()[0]->patterns()[0]->leaf()));
  EXPECT_TRUE(inner_match->arms()[1]->patterns()[0]->IsWildcardLeaf());
}

TEST(SemanticsAnalysisTest, PreservesModuleWhenThereIsNoIfLet) {
  constexpr std::string_view kProgram = R"(
fn identity(x: u32) -> u32 {
  x
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Module> module,
                           ParseModule(kProgram, "fake_path.x", "the_module",
                                       import_data.file_table()));
  Module* original_module = module.get();
  WarningCollector warnings(import_data.enabled_warnings());
  SemanticsAnalysis analysis(/*suppress_warnings=*/false);
  XLS_ASSERT_OK_AND_ASSIGN(
      module,
      analysis.RunPreTypeCheckPass(std::move(module), warnings, import_data));

  EXPECT_EQ(module.get(), original_module);
}

}  // namespace
}  // namespace xls::dslx
