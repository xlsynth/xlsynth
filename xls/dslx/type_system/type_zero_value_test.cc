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

#include "xls/dslx/type_system/type_zero_value.h"

#include <memory>
#include <optional>
#include <vector>

#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {
namespace {
using ::absl_testing::IsOk;

SumType MakeOuterSumWithInhabitedNestedSumPayload(Module& module) {
  const Span kFakeSpan = Span::Fake();

  auto* inner_name = module.Make<NameDef>(kFakeSpan, "Inner", nullptr);
  auto* inner_unit_name = module.Make<NameDef>(kFakeSpan, "InnerUnit", nullptr);
  auto* inner_unit = module.Make<SumVariant>(
      kFakeSpan, inner_unit_name, SumVariant::PayloadShape::kUnit,
      std::vector<TypeAnnotation*>{}, std::vector<StructMemberNode*>{});
  auto* inner_def = module.Make<SumDef>(
      kFakeSpan, inner_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{inner_unit}, /*is_public=*/false);
  inner_name->set_definer(inner_def);
  std::vector<SumTypeVariant> inner_variants;
  inner_variants.push_back(SumTypeVariant::MakeUnit(*inner_unit));
  SumType inner_type(*inner_def, std::move(inner_variants));

  auto* outer_name = module.Make<NameDef>(kFakeSpan, "Outer", nullptr);
  auto* wrapped_name = module.Make<NameDef>(kFakeSpan, "Wrapped", nullptr);
  auto* wrapped = module.Make<SumVariant>(
      kFakeSpan, wrapped_name, SumVariant::PayloadShape::kTuple,
      std::vector<TypeAnnotation*>{module.Make<TypeRefTypeAnnotation>(
          kFakeSpan, module.Make<TypeRef>(kFakeSpan, inner_def),
          std::vector<ExprOrType>{})},
      std::vector<StructMemberNode*>{});
  auto* outer_def = module.Make<SumDef>(
      kFakeSpan, outer_name, std::vector<ParametricBinding*>{},
      std::vector<SumVariant*>{wrapped}, /*is_public=*/false);
  outer_name->set_definer(outer_def);
  std::vector<std::unique_ptr<Type>> wrapped_members;
  wrapped_members.push_back(inner_type.CloneToUnique());
  std::vector<SumTypeVariant> outer_variants;
  outer_variants.push_back(
      SumTypeVariant::MakeTuple(*wrapped, std::move(wrapped_members)));
  return SumType(*outer_def, std::move(outer_variants));
}

TEST(TypeZeroValueTest, AcceptsNestedSumPayload) {
  FileTable file_table;
  Module module("test", /*fs_path=*/std::nullopt, file_table);
  SumType outer_type = MakeOuterSumWithInhabitedNestedSumPayload(module);
  ImportData import_data = CreateImportDataForTest();

  EXPECT_THAT(MakeZeroValue(outer_type, import_data, Span::Fake()), IsOk());
}

}  // namespace
}  // namespace xls::dslx
