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

#include "xls/fuzzer/sample_generator.h"

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/frontend/proc.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/ast_generator.h"
#include "xls/fuzzer/sample.h"
#include "xls/fuzzer/sample.pb.h"
#include "xls/fuzzer/value_generator.h"

namespace xls {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::testing::HasSubstr;

absl::StatusOr<dslx::TypecheckedModule> ParseSample(
    std::string_view text, dslx::ImportData* import_data) {
  return dslx::ParseAndTypecheck(text, "sample.x", "sample", import_data);
}

bool TypeDefinitionContainsSumDef(const dslx::TypeDefinition& type_def);

bool TypeAnnotationContainsSumDef(const dslx::TypeAnnotation* type) {
  if (auto* type_ref = dynamic_cast<const dslx::TypeRefTypeAnnotation*>(type);
      type_ref != nullptr) {
    return TypeDefinitionContainsSumDef(
        type_ref->type_ref()->type_definition());
  }
  if (auto* tuple = dynamic_cast<const dslx::TupleTypeAnnotation*>(type);
      tuple != nullptr) {
    for (dslx::TypeAnnotation* member_type : tuple->members()) {
      if (TypeAnnotationContainsSumDef(member_type)) {
        return true;
      }
    }
    return false;
  }
  if (auto* array = dynamic_cast<const dslx::ArrayTypeAnnotation*>(type);
      array != nullptr) {
    return TypeAnnotationContainsSumDef(array->element_type());
  }
  return false;
}

bool TypeDefinitionContainsSumDef(const dslx::TypeDefinition& type_def) {
  return std::visit(
      xls::Visitor{
          [](dslx::SumDef*) { return true; },
          [](dslx::StructDef*) { return false; },
          [](dslx::ProcDef*) { return false; },
          [](dslx::EnumDef*) { return false; },
          [](dslx::ColonRef*) { return false; },
          [](dslx::UseTreeEntry*) { return false; },
          [](dslx::TypeAlias* type_alias) {
            return TypeAnnotationContainsSumDef(&type_alias->type_annotation());
          },
      },
      type_def);
}

std::vector<std::string> ProcChannelNamesWithSumDef(
    const dslx::Proc& proc, dslx::ChannelDirection direction) {
  std::vector<std::string> names;
  for (dslx::ProcMember* member : proc.members()) {
    auto* channel_type =
        dynamic_cast<dslx::ChannelTypeAnnotation*>(member->type_annotation());
    if (channel_type != nullptr && channel_type->direction() == direction &&
        TypeAnnotationContainsSumDef(channel_type->payload())) {
      names.push_back(member->identifier());
    }
  }
  return names;
}

bool ProcHasChannelPayloadWithSumDef(const dslx::Proc& proc,
                                     dslx::ChannelDirection direction) {
  return !ProcChannelNamesWithSumDef(proc, direction).empty();
}

bool ContainsRecvFromChannelNames(
    const dslx::AstNode* node,
    const absl::flat_hash_set<std::string>& channel_names) {
  if (auto* invocation = dynamic_cast<const dslx::Invocation*>(node);
      invocation != nullptr) {
    auto* callee = dynamic_cast<const dslx::NameRef*>(invocation->callee());
    if (callee != nullptr && callee->identifier() == "recv" &&
        invocation->args().size() == 2) {
      auto* channel_ref =
          dynamic_cast<const dslx::NameRef*>(invocation->args()[1]);
      if (channel_ref != nullptr &&
          channel_names.contains(channel_ref->identifier())) {
        return true;
      }
    }
  }
  for (const dslx::AstNode* child : node->GetChildren(/*want_types=*/false)) {
    if (ContainsRecvFromChannelNames(child, channel_names)) {
      return true;
    }
  }
  return false;
}

bool ProcReceivesFromInputChannelWithSumDef(const dslx::Proc& proc) {
  std::vector<std::string> names =
      ProcChannelNamesWithSumDef(proc, dslx::ChannelDirection::kIn);
  absl::flat_hash_set<std::string> name_set(names.begin(), names.end());
  return !name_set.empty() &&
         ContainsRecvFromChannelNames(&proc.next(), name_set);
}

bool ProcHasStateWithSumDef(const dslx::Proc& proc) {
  if (proc.next().params().empty()) {
    return false;
  }
  return TypeAnnotationContainsSumDef(
      proc.next().params().front()->type_annotation());
}

absl::StatusOr<dslx::Proc*> GetGeneratedMainProc(
    const dslx::TypecheckedModule& tm) {
  return tm.module->GetMemberOrError<dslx::Proc>("main");
}

TEST(SampleGeneratorTest, GenerateBasicFunctionSample) {
  dslx::FileTable file_table;
  std::mt19937_64 rng;
  SampleOptions sample_options;
  constexpr int kCallsPerSample = 3;
  sample_options.set_calls_per_sample(kCallsPerSample);
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample, GenerateSample(dslx::AstGeneratorOptions{}, sample_options,
                                    rng, file_table));
  EXPECT_TRUE(sample.options().input_is_dslx());
  EXPECT_TRUE(sample.options().convert_to_ir());
  EXPECT_TRUE(sample.options().optimize_ir());
  EXPECT_FALSE(sample.options().codegen());
  EXPECT_FALSE(sample.options().simulate());

  std::vector<std::vector<dslx::InterpValue>> args_batch;
  XLS_EXPECT_OK(sample.GetArgsAndChannels(args_batch));
  EXPECT_EQ(args_batch.size(), kCallsPerSample);
  EXPECT_THAT(sample.input_text(), testing::HasSubstr("fn main"));
}

TEST(SampleGeneratorTest, GenerateCodegenSample) {
  dslx::FileTable file_table;
  std::mt19937_64 rng;
  SampleOptions sample_options;
  sample_options.set_codegen(true);
  sample_options.set_simulate(true);
  constexpr int64_t kCallsPerSample = 0;
  sample_options.set_calls_per_sample(kCallsPerSample);
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample, GenerateSample(dslx::AstGeneratorOptions{}, sample_options,
                                    rng, file_table));
  EXPECT_TRUE(sample.options().input_is_dslx());
  EXPECT_TRUE(sample.options().convert_to_ir());
  EXPECT_TRUE(sample.options().optimize_ir());
  EXPECT_TRUE(sample.options().codegen());
  EXPECT_TRUE(sample.options().simulate());
  EXPECT_FALSE(sample.options().codegen_args().empty());
  std::vector<std::vector<dslx::InterpValue>> args_batch;
  XLS_EXPECT_OK(sample.GetArgsAndChannels(args_batch));
  EXPECT_EQ(args_batch.size(), kCallsPerSample);
}

TEST(SampleGeneratorTest, GenerateChannelArgument) {
  std::mt19937_64 rng;
  std::vector<std::unique_ptr<dslx::Type>> param_types;
  constexpr int64_t kBitCount = 4;
  param_types.push_back(
      std::make_unique<dslx::ChannelType>(std::make_unique<dslx::BitsType>(
                                              /*signed=*/true,
                                              /*size=*/kBitCount),
                                          dslx::ChannelDirection::kOut));

  std::vector<const dslx::Type*> param_type_ptrs;
  param_type_ptrs.reserve(param_types.size());
  for (const auto& t : param_types) {
    param_type_ptrs.push_back(t.get());
  }
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> arguments,
                           GenerateInterpValues(rng, param_type_ptrs));
  ASSERT_EQ(arguments.size(), 1);
  ASSERT_EQ(arguments.size(), param_types.size());
  const dslx::InterpValue& value = arguments[0];
  ASSERT_TRUE(value.IsSBits());
  EXPECT_THAT(value.GetBitCount(), IsOkAndHolds(kBitCount));
}

TEST(SampleGeneratorTest, GenerateBasicProcSample) {
  dslx::FileTable file_table;
  std::mt19937_64 rng;
  SampleOptions sample_options;
  constexpr int64_t kProcTicks = 3;
  sample_options.set_sample_type(fuzzer::SampleType::SAMPLE_TYPE_PROC);
  sample_options.set_calls_per_sample(0);
  sample_options.set_proc_ticks(kProcTicks);
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample,
      GenerateSample(dslx::AstGeneratorOptions{.generate_proc = true},
                     sample_options, rng, file_table));
  EXPECT_TRUE(sample.options().input_is_dslx());
  EXPECT_TRUE(sample.options().convert_to_ir());
  EXPECT_TRUE(sample.options().optimize_ir());
  EXPECT_FALSE(sample.options().codegen());
  EXPECT_FALSE(sample.options().simulate());

  std::vector<std::vector<dslx::InterpValue>> args_batch;
  std::vector<std::string> ir_channel_names;
  XLS_EXPECT_OK(sample.GetArgsAndChannels(args_batch, &ir_channel_names));
  EXPECT_EQ(args_batch.size(), kProcTicks);

  EXPECT_THAT(sample.input_text(), HasSubstr("proc main"));
}

TEST(SampleGeneratorTest, GenerateProcSampleWithRequiredSumType) {
  dslx::FileTable file_table;
  std::mt19937_64 rng;
  SampleOptions sample_options;
  constexpr int64_t kProcTicks = 3;
  sample_options.set_sample_type(fuzzer::SampleType::SAMPLE_TYPE_PROC);
  sample_options.set_calls_per_sample(0);
  sample_options.set_proc_ticks(kProcTicks);
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample,
      GenerateSample(dslx::AstGeneratorOptions{.generate_proc = true,
                                               .require_sum_type = true},
                     sample_options, rng, file_table));
  EXPECT_EQ(sample.options().sample_type(),
            fuzzer::SampleType::SAMPLE_TYPE_PROC);
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypecheckedModule tm,
                           ParseSample(sample.input_text(), &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Proc * proc, GetGeneratedMainProc(tm));
  EXPECT_TRUE(ProcReceivesFromInputChannelWithSumDef(*proc));
  EXPECT_TRUE(
      ProcHasStateWithSumDef(*proc) ||
      ProcHasChannelPayloadWithSumDef(*proc, dslx::ChannelDirection::kOut));

  std::vector<std::vector<dslx::InterpValue>> args_batch;
  std::vector<std::string> ir_channel_names;
  XLS_EXPECT_OK(sample.GetArgsAndChannels(args_batch, &ir_channel_names));
  ASSERT_EQ(args_batch.size(), kProcTicks);
  ASSERT_FALSE(args_batch.front().empty());
  EXPECT_FALSE(ir_channel_names.empty());
}

TEST(SampleGeneratorTest, GenerateStatelessProcSampleWithRequiredSumType) {
  dslx::FileTable file_table;
  std::mt19937_64 rng;
  SampleOptions sample_options;
  constexpr int64_t kProcTicks = 3;
  sample_options.set_sample_type(fuzzer::SampleType::SAMPLE_TYPE_PROC);
  sample_options.set_calls_per_sample(0);
  sample_options.set_proc_ticks(kProcTicks);
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample,
      GenerateSample(dslx::AstGeneratorOptions{.generate_proc = true,
                                               .emit_stateless_proc = true,
                                               .require_sum_type = true},
                     sample_options, rng, file_table));
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypecheckedModule tm,
                           ParseSample(sample.input_text(), &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Proc * proc, GetGeneratedMainProc(tm));
  EXPECT_TRUE(proc->IsStateless());
  EXPECT_THAT(sample.input_text(), HasSubstr("send("));
  EXPECT_TRUE(ProcReceivesFromInputChannelWithSumDef(*proc));
  EXPECT_TRUE(
      ProcHasChannelPayloadWithSumDef(*proc, dslx::ChannelDirection::kOut));

  std::vector<std::vector<dslx::InterpValue>> args_batch;
  std::vector<std::string> ir_channel_names;
  XLS_EXPECT_OK(sample.GetArgsAndChannels(args_batch, &ir_channel_names));
  ASSERT_EQ(args_batch.size(), kProcTicks);
  ASSERT_FALSE(args_batch.front().empty());
  EXPECT_FALSE(ir_channel_names.empty());
}

}  // namespace
}  // namespace xls
