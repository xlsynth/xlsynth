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

namespace xls::dslx {
namespace {

bool IsBool(const BitsLikeProperties& bits_like) {
    return bits_like.size.GetAsInt64().value() == 1 && !bits_like.is_signed.GetAsBool().value();
}

}  // namespace

bool IsKnownExhaustiveMatchBool(const Match& match, const BitsLikeProperties& bits_like, const TypeInfo& type_info) {
    auto tested = BitsValueRange::MakeEmpty(bits_like);
    for (const MatchArm* arm : match.arms()) {
        for (const NameDefTree* pattern : arm->patterns()) {
            CHECK(pattern->is_leaf());
            if (pattern->IsIrrefutable()) {
                return true;
            }
            absl::visit(Visitor{
                [&](auto* node) {
                    std::optional<InterpValue> value = type_info.GetConstExprOption(node);
                    CHECK(value.has_value());
                    tested = BitsValueRange::Merge(tested, BitsValueRange::MakeSingleRange(bits_like, *value, *value));
                },
                [&](Range* range) {
                    std::optional<InterpValue> start = type_info.GetConstExprOption(range->start());
                    CHECK(start.has_value());
                    std::optional<InterpValue> end = type_info.GetConstExprOption(range->end());
                    CHECK(end.has_value());
                    tested = BitsValueRange::Merge(tested, BitsValueRange::MakeSingleRange(bits_like, *start, *end));
                },
                [](RestOfTuple* rest_of_tuple) {
                    LOG(FATAL) << "RestOfTuple not valid for boolean value match";
                },
            }, pattern->leaf());
        }
    }
    return tested.IsExhaustive();
}

bool IsKnownExhaustiveMatch(const Match& match, const BitsLikeProperties& bits_like, const TypeInfo& type_info) {
    if (IsBool(bits_like)) {
        return IsKnownExhaustiveMatchBool(match, bits_like, type_info);
    }
    LOG(FATAL) << "Not implemented";
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
    Expr* matched = match->matched();
    const TypeInfo& type_info = *tm.type_info;
    XLS_ASSERT_OK_AND_ASSIGN(const Type* matched_type, type_info.GetItemOrError(matched));
    std::optional<BitsLikeProperties> bits_like = GetBitsLike(*matched_type);
    ASSERT_TRUE(bits_like.has_value());
    EXPECT_TRUE(IsKnownExhaustiveMatch(*match, bits_like.value(), type_info));
}

}  // namespace xls::dslx