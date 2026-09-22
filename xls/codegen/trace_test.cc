// Copyright 2020 The XLS Authors
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

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_result.h"
#include "xls/codegen/combinational_generator.h"
#include "xls/codegen/pipeline_generator.h"
#include "xls/common/status/matchers.h"
#include "xls/estimators/delay_model/delay_estimator.h"
#include "xls/estimators/delay_model/delay_estimators.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/package.h"
#include "xls/scheduling/pipeline_schedule.h"
#include "xls/scheduling/run_pipeline_schedule.h"
#include "xls/scheduling/scheduling_options.h"
#include "xls/simulation/module_testbench.h"
#include "xls/simulation/module_testbench_thread.h"
#include "xls/simulation/verilog_simulator.h"
#include "xls/simulation/verilog_test_base.h"

namespace xls {
namespace verilog {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

constexpr char kTestName[] = "trace_test";
constexpr char kTestdataPath[] = "xls/codegen/testdata";

class TraceTest : public VerilogTestBase {};

constexpr char kSimpleTraceText[] = R"(
package SimpleTrace
top fn main(tkn: token, cond: bits[1]) -> token {
  ret trace.1: token = trace(tkn, cond, format="This is a simple trace.", data_operands=[], id=1)
}
)";

TEST_P(TraceTest, CombinationalSimpleTrace) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(kSimpleTraceText));
  std::optional<FunctionBase*> top = package->GetTop();
  ASSERT_TRUE(top.has_value());
  FunctionBase* entry = top.value();
  CodegenOptions options;
  options.use_system_verilog(UseSystemVerilog());
  XLS_ASSERT_OK_AND_ASSIGN(auto result,
                           GenerateCombinationalModule(entry, options));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 result.verilog_text);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ModuleTestbench> tb,
      NewModuleTestbench(result.verilog_text, result.signature));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", /*default_value=*/ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  // The combinational module doesn't a connected clock, but the clock can still
  // be used to sequence events in time.
  seq.NextCycle().Set("cond", 0);
  tbt->ExpectTrace("This is a simple trace.");
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));

  seq.NextCycle().Set("cond", 1);
  XLS_ASSERT_OK(tb->Run());

  // Expect a second trace output
  tbt->ExpectTrace("This is a simple trace.");
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));

  // Trigger a second output by changing cond
  seq.NextCycle().Set("cond", 0);
  seq.NextCycle().Set("cond", 1);
  XLS_ASSERT_OK(tb->Run());

  // Expect a third trace output
  tbt->ExpectTrace("This is a simple trace.");
  seq.NextCycle();

  // Fail to find the third trace output because cond did not change.
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));
}

// Verifies: Conditional RTL traces emit exactly one line per enabled event.
// Catches: Extra lines, partial messages, and output while tracing is disabled.
TEST_P(TraceTest, CombinationalConditionalTraceEmitsOneLine) {
  constexpr std::string_view kConditionalTraceText = R"(
package ConditionalTrace
top fn main(tkn: token, cond: bits[1], outer: bits[1], inner: bits[1], value: bits[8]) -> token {
  ret trace.1: token = trace(tkn, cond, format="prefix{?} outer{?} {}{/}{/} suffix", data_operands=[outer, inner, value], id=1)
}
)";
  // Keep the measured output free of testbench monitors. Mark each condition
  // change so disabled output cannot substitute for a missing enabled trace.
  constexpr char kTestbench[] = R"(
module testbench;
  reg cond;
  reg outer;
  reg inner;
  reg [7:0] value;
  main dut(.cond(cond), .outer(outer), .inner(inner), .value(value));
  initial begin
    $display("TRACE_TEST_BEGIN");
    cond = 0;
    outer = 1;
    inner = 1;
    value = 8'd142;
    #1;
    $display("TRACE_ENABLED");
    cond = 1;
    #1;
    $display("TRACE_DISABLED");
    cond = 0;
    outer = 0;
    #1;
    $display("TRACE_ENABLED");
    cond = 1;
    #1;
    $display("TRACE_DISABLED");
    cond = 0;
    outer = 1;
    inner = 0;
    #1;
    $display("TRACE_ENABLED");
    cond = 1;
    #1;
    $display("TRACE_DISABLED");
    cond = 0;
    inner = 1;
    value = 8'd143;
    #1;
    $display("TRACE_TEST_END");
    $finish;
  end
endmodule
)";
  for (bool use_system_verilog : {false, true}) {
    SCOPED_TRACE(use_system_verilog ? "SystemVerilog" : "Verilog");
    XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                             Parser::ParsePackage(kConditionalTraceText));
    ASSERT_TRUE(package->GetTop().has_value());
    CodegenOptions options;
    options.use_system_verilog(use_system_verilog);
    XLS_ASSERT_OK_AND_ASSIGN(
        auto result,
        GenerateCombinationalModule(package->GetTop().value(), options));
    EXPECT_THAT(result.verilog_text, HasSubstr("$write("));
    EXPECT_THAT(result.verilog_text, HasSubstr("$display(\"\")"));

    if (use_system_verilog == UseSystemVerilog()) {
      XLS_ASSERT_OK_AND_ASSIGN(
          auto stdout_stderr,
          GetSimulator()->Run(result.verilog_text + kTestbench, GetFileType()));
      std::string_view output = stdout_stderr.first;
      constexpr std::string_view kBegin = "TRACE_TEST_BEGIN\n";
      constexpr std::string_view kEnd = "TRACE_TEST_END\n";
      const std::size_t begin = output.find(kBegin);
      ASSERT_NE(begin, std::string_view::npos) << output;
      output.remove_prefix(begin + kBegin.size());
      const std::size_t end = output.find(kEnd);
      ASSERT_NE(end, std::string_view::npos) << output;
      EXPECT_EQ(output.substr(0, end),
                "TRACE_ENABLED\n"
                "prefix outer 142 suffix\n"
                "TRACE_DISABLED\n"
                "TRACE_ENABLED\n"
                "prefix suffix\n"
                "TRACE_DISABLED\n"
                "TRACE_ENABLED\n"
                "prefix outer suffix\n"
                "TRACE_DISABLED\n");
    }
  }
}

// This is just a basic test to ensure that traces in clocked modules generate
// output. See side_effect_condition_pass_test.cc for coverage of more
// interesting scenarios.
TEST_P(TraceTest, ClockedSimpleTraceTest) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(kSimpleTraceText));
  std::optional<FunctionBase*> top = package->GetTop();
  ASSERT_TRUE(top.has_value());
  FunctionBase* entry = top.value();

  XLS_ASSERT_OK_AND_ASSIGN(const DelayEstimator* delay_estimator,
                           GetDelayEstimator("unit"));
  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(entry, *delay_estimator,
                          SchedulingOptions().pipeline_stages(1)));

  XLS_ASSERT_OK_AND_ASSIGN(
      verilog::CodegenResult result,
      ToPipelineModuleText(
          schedule, entry,
          BuildPipelineOptions().use_system_verilog(UseSystemVerilog())));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 result.verilog_text);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ModuleTestbench> tb,
      NewModuleTestbench(result.verilog_text, result.signature));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", /*default_value=*/ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.NextCycle().Set("cond", 0);
  tbt->ExpectTrace("This is a simple trace.");
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));

  seq.NextCycle().Set("cond", 1);
  // Advance a second cycle so that cond makes it through the pipeline to
  // trigger the trace.
  seq.NextCycle();
  XLS_ASSERT_OK(tb->Run());

  // Expect a second trace output
  tbt->ExpectTrace("This is a simple trace.");
  // Fail to find the second trace because we haven't advanced the clock.
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));

  // Trigger a second output by advancing the clock even though cond is 0.
  seq.NextCycle().Set("cond", 0);
  XLS_ASSERT_OK(tb->Run());

  // Expect a third trace output
  tbt->ExpectTrace("This is a simple trace.");

  // Fail to find it after advancing the clock because cond was 0 in the
  // previous cycle.
  EXPECT_THAT(tb->Run(), StatusIs(absl::StatusCode::kNotFound,
                                  HasSubstr("This is a simple trace.")));
}

TEST_P(TraceTest, ClockedSimpleTraceTestWithInvertedSimulationMacro) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(kSimpleTraceText));
  std::optional<FunctionBase*> top = package->GetTop();
  ASSERT_TRUE(top.has_value());
  FunctionBase* entry = top.value();

  XLS_ASSERT_OK_AND_ASSIGN(const DelayEstimator* delay_estimator,
                           GetDelayEstimator("unit"));
  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(entry, *delay_estimator,
                          SchedulingOptions().pipeline_stages(1)));

  XLS_ASSERT_OK_AND_ASSIGN(
      CodegenResult result,
      ToPipelineModuleText(schedule, entry,
                           BuildPipelineOptions()
                               .use_system_verilog(UseSystemVerilog())
                               .set_simulation_macro_name("!SYNTHESIS")));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 result.verilog_text);
}

INSTANTIATE_TEST_SUITE_P(TraceTestInstantiation, TraceTest,
                         testing::ValuesIn(kDefaultSimulationTargets),
                         ParameterizedTestName<TraceTest>);

}  // namespace
}  // namespace verilog
}  // namespace xls
