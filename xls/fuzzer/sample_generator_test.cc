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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/fuzzer/ast_generator.h"
#include "xls/fuzzer/sample.h"
#include "xls/fuzzer/sample.pb.h"
#include "xls/fuzzer/value_generator.h"
#include "xls/ir/bits.h"

namespace xls {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::testing::ContainsRegex;
using ::testing::HasSubstr;

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

TEST(SampleGeneratorTest, RequiredSumInputsAndResultsVaryAtRuntime) {
  dslx::AstGeneratorOptions generator_options;
  generator_options.require_sum_type = true;
  generator_options.max_width_bits_types = 8;
  generator_options.max_width_aggregate_types = 64;
  SampleOptions sample_options;
  sample_options.set_calls_per_sample(128);
  bool saw_multiple_constructors = false;
  bool saw_singleton = false;
  bool saw_standalone_struct_payload = false;
  for (uint64_t seed = 0; seed < 8; ++seed) {
    dslx::FileTable file_table;
    std::mt19937_64 rng{seed};
    XLS_ASSERT_OK_AND_ASSIGN(
        Sample sample,
        GenerateSample(generator_options, sample_options, rng, file_table));
    SCOPED_TRACE(sample.input_text());
    auto import_data = dslx::CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        dslx::TypecheckedModule tm,
        dslx::ParseAndTypecheck(sample.input_text(), "runtime_sum.x",
                                "runtime_sum", &import_data));
    XLS_ASSERT_OK_AND_ASSIGN(
        dslx::Function * main,
        tm.module->GetMemberOrError<dslx::Function>("main"));
    XLS_ASSERT_OK_AND_ASSIGN(dslx::FunctionType * function_type,
                             tm.type_info->GetItemAs<dslx::FunctionType>(main));
    XLS_ASSERT_OK_AND_ASSIGN(dslx::TypeDim return_width,
                             function_type->return_type().GetTotalBitCount());
    EXPECT_THAT(
        return_width.GetAsInt64(),
        IsOkAndHolds(testing::Le(generator_options.max_width_aggregate_types)));
    const dslx::SumDef* definition = tm.module->GetSumDefs().back();
    const dslx::SumType* sum_type = nullptr;
    int64_t sum_index = -1;
    for (int64_t i = 0; i < function_type->params().size(); ++i) {
      EXPECT_EQ(dynamic_cast<const dslx::StructType*>(
                    function_type->params()[i].get()),
                nullptr);
      auto* candidate =
          dynamic_cast<const dslx::SumType*>(function_type->params()[i].get());
      if (candidate != nullptr && &candidate->nominal_type() == definition) {
        sum_type = candidate;
        sum_index = i;
      }
    }
    ASSERT_NE(sum_type, nullptr);
    ASSERT_GE(sum_index, 0);
    const int64_t count = sum_type->variant_count();
    for (const dslx::SumTypeVariant& variant : sum_type->variants()) {
      for (int64_t i = 0; i < variant.size(); ++i) {
        saw_standalone_struct_payload |=
            dynamic_cast<const dslx::StructType*>(&variant.GetMemberType(i)) !=
            nullptr;
      }
    }
    saw_multiple_constructors |= count > 1;
    saw_singleton |= count == 1;
    XLS_ASSERT_OK_AND_ASSIGN(int64_t tag_width,
                             sum_type->tag_bit_count().GetAsInt64());
    if (count == 1) {
      EXPECT_EQ(tag_width, 0);
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        auto bytecode,
        dslx::BytecodeEmitter::Emit(&import_data, tm.type_info, *main,
                                    /*caller_bindings=*/std::nullopt));
    std::vector<std::vector<dslx::InterpValue>> unsigned_args;
    XLS_ASSERT_OK(sample.GetArgsAndChannels(unsigned_args));
    ASSERT_EQ(unsigned_args.size(), 128);
    std::set<int64_t> input_variants;
    std::set<int64_t> observed_if_let_arms;
    for (int64_t call = 0; call < unsigned_args.size(); ++call) {
      XLS_ASSERT_OK_AND_ASSIGN(
          std::vector<dslx::InterpValue> args,
          dslx::SignConvertArgs(*function_type, unsigned_args[call]));
      XLS_ASSERT_OK(
          dslx::ValidateInterpValueMatchesType(args[sum_index], *sum_type));
      const Bits& input_tag =
          args[sum_index].GetValuesOrDie()[0].GetBitsOrDie();
      for (int64_t i = 0; i < count; ++i) {
        if (input_tag == sum_type->GetDiscriminant(i).GetBitsOrDie()) {
          input_variants.insert(i);
        }
      }
      const int64_t selector_index = sum_index + 1;
      const int64_t rhs_index = sum_index + 2;
      const int64_t lhs_index = sum_index + 3;
      ASSERT_LT(lhs_index, args.size());
      // Guarantee unequal scalar operands and every selectable constructor,
      // independently of the fuzzer's bias toward reusing earlier values.
      if (call < count) {
        XLS_ASSERT_OK_AND_ASSIGN(int64_t selector_width,
                                 args[selector_index].GetBitCount());
        args[selector_index] =
            dslx::InterpValue::MakeUBits(selector_width, call);
        XLS_ASSERT_OK_AND_ASSIGN(int64_t payload_width,
                                 args[lhs_index].GetBitCount());
        const bool is_signed = args[lhs_index].IsSBits();
        args[lhs_index] =
            dslx::InterpValue::MakeBits(is_signed, UBits(1, payload_width));
        args[rhs_index] =
            dslx::InterpValue::MakeBits(is_signed, UBits(0, payload_width));
      }
      XLS_ASSERT_OK_AND_ASSIGN(uint64_t selector,
                               args[selector_index].GetBitValueUnsigned());
      const int64_t selected = std::min<int64_t>(selector, count - 1);
      XLS_ASSERT_OK_AND_ASSIGN(dslx::InterpValue result,
                               dslx::BytecodeInterpreter::Interpret(
                                   &import_data, bytecode.get(), args));
      ASSERT_TRUE(result.IsTuple());
      ASSERT_EQ(result.GetValuesOrDie().size(), 2);
      const std::vector<dslx::InterpValue>& observed =
          result.GetValuesOrDie()[1].GetValuesOrDie();
      ASSERT_EQ(observed.size(), 5);
      // Complete transported payloads must equal the original input. No sum
      // encoder or decoder is used to construct this expected result.
      EXPECT_EQ(observed[0], args[sum_index]);
      const dslx::SumVariant* variant = definition->variants()[selected];
      Bits expected_tag = UBits(selected, tag_width);
      if (variant->discriminant().has_value()) {
        auto* literal = dynamic_cast<dslx::Number*>(*variant->discriminant());
        ASSERT_NE(literal, nullptr);
        XLS_ASSERT_OK_AND_ASSIGN(
            expected_tag,
            literal->GetBits(tag_width, import_data.file_table()));
      }
      EXPECT_EQ(observed[1].GetValuesOrDie()[0].GetBitsOrDie(), expected_tag);
      const Bits& original_payload = args[lhs_index].GetBitsOrDie();
      EXPECT_EQ(observed[2].GetBitsOrDie(),
                selected == 0 ? original_payload
                              : Bits(original_payload.bit_count()));
      XLS_ASSERT_OK_AND_ASSIGN(uint64_t if_let_arm,
                               observed[4].GetBitValueUnsigned());
      EXPECT_EQ(if_let_arm, selected);
      observed_if_let_arms.insert(if_let_arm);
    }
    EXPECT_EQ(input_variants.size(), count);
    EXPECT_EQ(observed_if_let_arms.size(), count);
  }
  EXPECT_TRUE(saw_multiple_constructors);
  EXPECT_TRUE(saw_singleton);
  EXPECT_TRUE(saw_standalone_struct_payload);
}

TEST(SampleGeneratorTest, GenerateCrossModuleSumFunctionSample) {
  dslx::FileTable file_table;
  std::mt19937_64 rng{0};
  SampleOptions sample_options;
  constexpr int kCallsPerSample = 2;
  sample_options.set_calls_per_sample(kCallsPerSample);

  dslx::AstGeneratorOptions generator_options;
  generator_options.require_sum_type = true;
  generator_options.require_cross_module_sum_type = true;
  XLS_ASSERT_OK_AND_ASSIGN(
      Sample sample,
      GenerateSample(generator_options, sample_options, rng, file_table));

  EXPECT_TRUE(sample.options().input_is_dslx());
  EXPECT_TRUE(sample.options().convert_to_ir());
  EXPECT_TRUE(sample.options().optimize_ir());
  EXPECT_THAT(sample.input_text(), HasSubstr("import float32;"));
  EXPECT_THAT(sample.input_text(), HasSubstr("float32::F32 {"));
  EXPECT_THAT(sample.input_text(), HasSubstr(".fraction as "));
  EXPECT_THAT(sample.input_text(),
              HasSubstr("import xls.fuzzer.testdata.semantic_sum_provider;"));
  EXPECT_THAT(sample.input_text(),
              HasSubstr("semantic_sum_provider::Option::Some("));
  EXPECT_THAT(sample.input_text(),
              HasSubstr("semantic_sum_provider::identity("));
  EXPECT_THAT(sample.input_text(),
              HasSubstr("semantic_sum_provider::Option::None"));
  EXPECT_THAT(sample.input_text(),
              HasSubstr("semantic_sum_provider::Option) -> "
                        "semantic_sum_provider::Option"));

  auto import_data = dslx::CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(sample.input_text(), "cross_module_sum.x",
                              "cross_module_sum", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Function * main,
                           tm.module->GetMemberOrError<dslx::Function>("main"));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::FunctionType * function_type,
                           tm.type_info->GetItemAs<dslx::FunctionType>(main));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto bytecode,
      dslx::BytecodeEmitter::Emit(&import_data, tm.type_info, *main,
                                  /*caller_bindings=*/std::nullopt));
  std::vector<std::vector<dslx::InterpValue>> args_batch;
  XLS_ASSERT_OK(sample.GetArgsAndChannels(args_batch));
  ASSERT_EQ(args_batch.size(), kCallsPerSample);
  for (const std::vector<dslx::InterpValue>& args : args_batch) {
    ASSERT_FALSE(args.empty());
    const dslx::InterpValue& imported_sum = args.back();
    ASSERT_TRUE(imported_sum.IsTuple());
    ASSERT_EQ(imported_sum.GetValuesOrDie().size(), 2);

    const dslx::InterpValue& tag = imported_sum.GetValuesOrDie().at(0);
    ASSERT_TRUE(tag.IsUBits());
    EXPECT_THAT(tag.GetBitCount(), IsOkAndHolds(1));

    const dslx::InterpValue& payload = imported_sum.GetValuesOrDie().at(1);
    ASSERT_TRUE(payload.IsTuple());
    ASSERT_EQ(payload.GetValuesOrDie().size(), 1);
    EXPECT_TRUE(payload.GetValuesOrDie().at(0).IsUBits());
    EXPECT_THAT(payload.GetValuesOrDie().at(0).GetBitCount(), IsOkAndHolds(8));

    // Execute the generated payload assertions as well as the imported identity
    // call. Source-shape checks alone do not establish either behavior.
    XLS_ASSERT_OK_AND_ASSIGN(std::vector<dslx::InterpValue> signed_args,
                             dslx::SignConvertArgs(*function_type, args));
    XLS_ASSERT_OK_AND_ASSIGN(dslx::InterpValue result,
                             dslx::BytecodeInterpreter::Interpret(
                                 &import_data, bytecode.get(), signed_args));
    EXPECT_EQ(result, signed_args.back());
  }

  ASSERT_EQ(sample.testvector().function_args().args_size(), kCallsPerSample);
  for (const std::string& args : sample.testvector().function_args().args()) {
    EXPECT_THAT(
        args,
        ContainsRegex(R"(\(bits\[1\]:0x[01], \(bits\[8\]:0x[0-9a-f]+\)\)$)"));
  }
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

}  // namespace
}  // namespace xls
