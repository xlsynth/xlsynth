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

#include "gtest/gtest.h"

#include "xls/dslx/exhaustiveness/exhaustiveness.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/type.h"
#include "xls/common/visitor.h"
#include "absl/log/log.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/create_import_data.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/exhaustiveness/match_exhaustiveness_checker.h"

namespace xls::dslx {
    namespace {

std::vector<const NameDefTree*> GetPatterns(const Match& match) {
    std::vector<const NameDefTree*> patterns;
    for (const MatchArm* arm : match.arms()) {
        for (const NameDefTree* pattern : arm->patterns()) {
            patterns.push_back(pattern);
        }
    }
    return patterns;
}

TEST(ExhaustivenessMatchTest, MatchBoolTrueFalse) {
    constexpr std::string_view kMatch = R"(fn main(x: bool) -> u32 {
    match x {
        false => u32:42,
        true => u32:64,
    }
})";

    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, ParseAndTypecheck(kMatch, "test.x", "test", &import_data));
    std::optional<Function*> func = tm.module->GetFunction("main");
    ASSERT_TRUE(func.has_value());
    StatementBlock* body = func.value()->body();
    ASSERT_EQ(body->statements().size(), 1);
    const Statement& statement = *body->statements().front();
    Match* match = dynamic_cast<Match*>(std::get<Expr*>(statement.wrapped()));
    ASSERT_TRUE(match != nullptr);

    XLS_ASSERT_OK_AND_ASSIGN(MatchExhaustivenessChecker checker, MatchExhaustivenessChecker::Make(*match, *tm.type_info));

    std::vector<const NameDefTree*> patterns = GetPatterns(*match);
    for (int64_t i = 0; i < patterns.size(); ++i) {
        bool now_exhaustive = checker.AddPattern(*patterns[i]);
        // We expect it to become exhaustive with the last match arm.
        bool expect_now_exhaustive = i+1 == patterns.size();
        EXPECT_EQ(now_exhaustive, expect_now_exhaustive);
    }
}

TEST(ExhaustivenessMatchTest, MatchBoolJustTrue) {
    constexpr std::string_view kMatch = R"(fn main(x: bool) -> u32 {
    match x {
        true => u32:42,
    }
})";
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, ParseAndTypecheck(kMatch, "test.x", "test", &import_data));
    std::optional<Function*> func = tm.module->GetFunction("main");
    ASSERT_TRUE(func.has_value());
    StatementBlock* body = func.value()->body();
    ASSERT_EQ(body->statements().size(), 1);
    const Statement& statement = *body->statements().front();
    Match* match = dynamic_cast<Match*>(std::get<Expr*>(statement.wrapped()));
    ASSERT_TRUE(match != nullptr);

    XLS_ASSERT_OK_AND_ASSIGN(MatchExhaustivenessChecker checker, MatchExhaustivenessChecker::Make(*match, *tm.type_info));

    std::vector<const NameDefTree*> patterns = GetPatterns(*match);
    for (int64_t i = 0; i < patterns.size(); ++i) {
        bool now_exhaustive = checker.AddPattern(*patterns[i]);
        // This one never becomes exhaustive.
        EXPECT_FALSE(now_exhaustive);
    }
}

    }  // namespace
}  // namespace xls::dslx
