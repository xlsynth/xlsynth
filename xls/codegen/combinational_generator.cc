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

#include "xls/codegen/combinational_generator.h"

#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "xls/codegen/block_conversion.h"
#include "xls/codegen/block_generator.h"
#include "xls/codegen/block_metrics.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_pass.h"
#include "xls/codegen/codegen_pass_pipeline.h"
#include "xls/codegen/codegen_residual_data.pb.h"
#include "xls/codegen/codegen_result.h"
#include "xls/codegen/module_signature.h"
#include "xls/codegen/verilog_line_map.pb.h"
#include "xls/codegen/xls_metrics.pb.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/estimators/delay_model/delay_estimator.h"
#include "xls/ir/node.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"

namespace xls {
namespace verilog {

absl::StatusOr<CodegenResult> GenerateCombinationalModule(
    FunctionBase* module, const CodegenOptions& options,
    const DelayEstimator* delay_estimator) {
  XLS_ASSIGN_OR_RETURN(CodegenContext context,
                       FunctionBaseToCombinationalBlock(module, options));

  const CodegenPassOptions codegen_pass_options = {
      .codegen_options = options,
      .delay_estimator = delay_estimator,
  };

  PassResults results;
  OptimizationContext opt_context;
  XLS_RETURN_IF_ERROR(
      CreateCodegenPassPipeline(opt_context)
          ->Run(module->package(), codegen_pass_options, &results, context)
          .status());
  XLS_RET_CHECK_NE(context.top_block(), nullptr);
  XLS_RET_CHECK(context.metadata().contains(context.top_block()));
  XLS_RET_CHECK(context.top_block()->GetSignature().has_value());
  VerilogLineMap verilog_line_map;
  CodegenResidualData residual_data;
  XLS_ASSIGN_OR_RETURN(std::string verilog,
                       GenerateVerilog(context.top_block(), options,
                                       &verilog_line_map, &residual_data));

  XLS_ASSIGN_OR_RETURN(
      ModuleSignature signature,
      ModuleSignature::FromProto(*context.top_block()->GetSignature()));

  XlsMetricsProto metrics;
  XLS_ASSIGN_OR_RETURN(
      *metrics.mutable_block_metrics(),
      GenerateBlockMetrics(context.top_block(), /*delay_estimator=*/nullptr));

  // TODO: google/xls#1323 - add all block signatures to ModuleGeneratorResult,
  // not just top.
  return CodegenResult{.verilog_text = verilog,
                       .verilog_line_map = verilog_line_map,
                       .signature = signature,
                       .block_metrics = metrics,
                       .residual_data = residual_data,
                       .pass_pipeline_metrics = results.ToProto()};
}

}  // namespace verilog
}  // namespace xls
