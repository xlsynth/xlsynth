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

#include "xls/dslx/type_info_to_proto.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls::dslx {
namespace {

void DoRun(std::string_view program, absl::Span<const std::string> want) {
  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data));

  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip, TypeInfoToProto(*tm.type_info));
  ASSERT_THAT(want, ::testing::SizeIs(tip.nodes_size()));
  std::vector<std::string> got;
  for (int64_t i = 0; i < tip.nodes_size(); ++i) {
    XLS_ASSERT_OK_AND_ASSIGN(std::string node_str,
                             ToHumanString(tip.nodes(i), *tm.module));
    EXPECT_EQ(node_str, want[i]) << "at index: " << i;
  }
}

TEST(TypeInfoToProtoTest, IdentityFunction) {
  std::string program = R"(fn id(x: u32) -> u32 { x })";
  std::vector<std::string> want = {
      /*0=*/
      "0:0-0:26: FUNCTION :: `fn id(x: u32) -> u32 {\n  x\n}` :: (uN[32]) -> "
      "uN[32]",
      /*1=*/"0:3-0:5: NAME_DEF :: `id` :: (uN[32]) -> uN[32]",
      /*2=*/"0:6-0:7: NAME_DEF :: `x` :: uN[32]",
      /*3=*/"0:6-0:12: PARAM :: `x: u32` :: uN[32]",
      /*4=*/"0:9-0:12: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*5=*/"0:17-0:20: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*6=*/"0:23-0:24: NAME_REF :: `x` :: uN[32]",
  };
  DoRun(program, want);
}

TEST(TypeInfoToProtoTest, ParametricIdentityFunction) {
  std::string program = R"(
fn pid<N: u32>(x: bits[N]) -> bits[N] { x }
fn id(x: u32) -> u32 { pid<u32:32>(x) }
)";
  std::vector<std::string> want = {
      /*0=*/
      "1:0-1:43: FUNCTION :: `fn pid<N: u32>(x: bits[N]) -> bits[N] {\n  x\n}` "
      ":: (uN[N]) -> uN[N]",
      /*1=*/"1:3-1:6: NAME_DEF :: `pid` :: (uN[N]) -> uN[N]",
      /*2=*/"1:7-1:8: NAME_DEF :: `N` :: uN[32]",
      /*3=*/"1:10-1:13: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*4=*/"1:15-1:16: NAME_DEF :: `x` :: uN[N]",
      /*5=*/"1:15-1:25: PARAM :: `x: bits[N]` :: uN[N]",
      /*6=*/"1:18-1:25: TYPE_ANNOTATION :: `bits` :: uN[N]",
      /*7=*/"1:30-1:37: TYPE_ANNOTATION :: `bits` :: uN[N]",
      /*8=*/
      "2:0-2:39: FUNCTION :: `fn id(x: u32) -> u32 {\n  pid<u32:32>(x)\n}` :: "
      "(uN[32]) -> uN[32]",
      /*9=*/"2:3-2:5: NAME_DEF :: `id` :: (uN[32]) -> uN[32]",
      /*10=*/"2:6-2:7: NAME_DEF :: `x` :: uN[32]",
      /*11=*/"2:6-2:12: PARAM :: `x: u32` :: uN[32]",
      /*12=*/"2:9-2:12: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*13=*/"2:17-2:20: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*14=*/"2:23-2:26: NAME_REF :: `pid` :: (uN[N]) -> uN[N]",
      /*15=*/"2:26-2:37: INVOCATION :: `pid<u32:32>(x)` :: uN[32]",
      /*16=*/"2:27-2:30: TYPE_ANNOTATION :: `u32` :: uN[32]",
      /*17=*/"2:31-2:33: NUMBER :: `u32:32` :: uN[32]",
      /*18=*/"2:35-2:36: NAME_REF :: `x` :: uN[32]",
  };
  DoRun(program, want);
}

}  // namespace
}  // namespace xls::dslx
