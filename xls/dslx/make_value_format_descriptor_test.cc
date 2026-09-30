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

#include "xls/dslx/make_value_format_descriptor.h"

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/typecheck_test_utils.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {
namespace {

TEST(MakeValueFormatDescriptorTest, ProvidersVisitEnumsAndOnlyNestedSums) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Leaf: u2 { Zero = 0 }
enum Inner { A(Leaf) }
enum Outer { First(Inner), Second(Inner), Direct(Leaf) }
fn f(x: Outer) -> Outer { x }
)"));
  auto function = result.tm.module->GetFunction("f");
  ASSERT_TRUE(function.has_value());
  XLS_ASSERT_OK_AND_ASSIGN(
      const FunctionType* function_type,
      result.tm.type_info->GetItemAs<FunctionType>(*function));

  std::vector<std::string> enum_visits;
  std::vector<std::string> nested_sum_visits;
  XLS_ASSERT_OK_AND_ASSIGN(
      ValueFormatDescriptor descriptor,
      MakeDefaultValueFormatDescriptorWithNestedSumProvider(
          *function_type->params().at(0),
          [&](const EnumType& type) {
            enum_visits.push_back(type.nominal_type().identifier());
            return MakeValueFormatDescriptor(type, FormatPreference::kDefault);
          },
          [&](const SumType& type) {
            nested_sum_visits.push_back(type.nominal_type().identifier());
            return MakeValueFormatDescriptor(type, FormatPreference::kDefault);
          }));

  EXPECT_THAT(enum_visits, testing::ElementsAre("Leaf"));
  EXPECT_THAT(nested_sum_visits, testing::ElementsAre("Inner"));
  ASSERT_TRUE(descriptor.IsSum());
  EXPECT_EQ(descriptor.sum_name(), "Outer");
  const ValueFormatDescriptor& first =
      descriptor.sum_variant(0).payload_formats().front();
  const ValueFormatDescriptor& second =
      descriptor.sum_variant(1).payload_formats().front();
  ASSERT_TRUE(first.IsSum());
  ASSERT_TRUE(second.IsSum());
  EXPECT_EQ(first.sum_format_identity(), second.sum_format_identity());
  EXPECT_EQ(descriptor.sum_variant(2).payload_formats().front().enum_name(),
            "Leaf");
}

}  // namespace
}  // namespace xls::dslx
