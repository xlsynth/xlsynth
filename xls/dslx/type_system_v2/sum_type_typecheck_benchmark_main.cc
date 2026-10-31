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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/warning_kind.h"

ABSL_FLAG(int64_t, iterations, 1,
          "Positive number of complete parse/typecheck operations to time.");
ABSL_FLAG(std::string, dslx_stdlib_path,
          std::string(xls::kDefaultDslxStdlibPath),
          "Path to the DSLX standard library directory.");
ABSL_FLAG(bool, inspect, false,
          "Report retained types for one compilation instead of timing it.");
ABSL_FLAG(std::string, inspect_function, "main",
          "In --inspect mode, inspect this function's first parameter type.");

namespace xls::dslx {
namespace {

constexpr std::string_view kUsage = R"(
Times complete parse/typecheck operations with fresh compilation owners.

Usage: sum_type_typecheck_benchmark_main --iterations=N input.x
       sum_type_typecheck_benchmark_main --inspect --inspect_function=f input.x

Timing emits one JSON object with elapsed_ns and iterations. The top-level input
file read and JSON output are excluded. Imported-file reads and compilation-owner
construction and destruction are included. There are no implicit warm-ups. Each
process is one sample, regardless of its iteration count. Inspection is a
separate, untimed invocation.
)";

struct RetainedTypes {
  absl::flat_hash_set<const Type*> wrappers;
  absl::flat_hash_set<const std::vector<SumTypeVariant>*> sum_descriptions;
};

void CollectRetainedTypes(const Type& type, RetainedTypes& retained) {
  if (retained.wrappers.insert(&type).second) {
    if (type.IsSum()) {
      const SumType& sum = type.AsSum();
      // The variants vector identifies a completed sum description, whether
      // owned by one wrapper or shared by several. Visit its children once,
      // while still counting every distinct outer Type wrapper above.
      if (retained.sum_descriptions.insert(&sum.variants()).second) {
        for (const auto& argument : sum.parametric_arguments()) {
          std::visit(
              [&](const auto& value_or_type) {
                if constexpr (!std::is_same_v<
                                  std::decay_t<decltype(value_or_type)>,
                                  InterpValue>) {
                  CollectRetainedTypes(*value_or_type, retained);
                }
              },
              argument);
        }
        for (const SumTypeVariant& variant : sum.variants()) {
          for (int64_t i = 0; i < variant.size(); ++i) {
            CollectRetainedTypes(variant.GetMemberType(i), retained);
          }
        }
      }
    } else if (type.IsArray()) {
      CollectRetainedTypes(type.AsArray().element_type(), retained);
    } else if (type.IsTuple()) {
      for (const auto& member : type.AsTuple().members()) {
        CollectRetainedTypes(*member, retained);
      }
    } else if (type.IsStruct()) {
      for (const auto& member : type.AsStruct().members()) {
        CollectRetainedTypes(*member, retained);
      }
    } else if (type.IsProc()) {
      for (const auto& member : type.AsProc().members()) {
        CollectRetainedTypes(*member, retained);
      }
    } else if (type.IsFunction()) {
      for (const auto& param : type.AsFunction().params()) {
        CollectRetainedTypes(*param, retained);
      }
      CollectRetainedTypes(type.AsFunction().return_type(), retained);
    } else if (type.IsChannel()) {
      CollectRetainedTypes(type.AsChannel().payload_type(), retained);
    } else if (type.IsMeta()) {
      CollectRetainedTypes(*type.AsMeta().wrapped(), retained);
    }
  }
}

absl::Status PrintInspection(const TypecheckedModule& module,
                             std::string_view function_name) {
  XLS_ASSIGN_OR_RETURN(
      const Function* function,
      module.module->GetMemberOrError<Function>(function_name));
  XLS_ASSIGN_OR_RETURN(const FunctionType* function_type,
                       module.type_info->GetItemAs<FunctionType>(function));
  if (function_type->params().empty()) {
    return absl::InvalidArgumentError(
        "The inspected function must have at least one parameter.");
  } else {
    const Type& root = *function_type->params().front();
    XLS_ASSIGN_OR_RETURN(TypeDim bit_count, root.GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t bits, bit_count.GetAsInt64());
    RetainedTypes retained;
    CollectRetainedTypes(root, retained);
    // These counts cover only the graph reachable from the first parameter,
    // not the whole compilation, cumulative allocations, or memory bytes.
    std::cout << "{\"root_param_bit_count\":" << bits
              << ",\"reachable_type_wrappers\":" << retained.wrappers.size()
              << ",\"unique_sum_descriptions\":"
              << retained.sum_descriptions.size() << "}\n";
    return absl::OkStatus();
  }
}

absl::Status RealMain(std::string_view input_path) {
  const int64_t iterations = absl::GetFlag(FLAGS_iterations);
  if (iterations <= 0) {
    return absl::InvalidArgumentError("--iterations must be positive.");
  } else {
    XLS_ASSIGN_OR_RETURN(std::string source,
                         GetFileContents(std::filesystem::path(input_path)));
    XLS_ASSIGN_OR_RETURN(std::string module_name, PathToName(input_path));
    const std::filesystem::path stdlib_path =
        absl::GetFlag(FLAGS_dslx_stdlib_path);
    if (absl::GetFlag(FLAGS_inspect)) {
      ImportData import_data = CreateImportData(
          stdlib_path, /*additional_search_paths=*/{}, kDefaultWarningsSet,
          std::make_unique<RealFilesystem>());
      XLS_ASSIGN_OR_RETURN(
          TypecheckedModule module,
          ParseAndTypecheck(source, input_path, module_name, &import_data));
      return PrintInspection(module, absl::GetFlag(FLAGS_inspect_function));
    } else {
      const auto start = std::chrono::steady_clock::now();
      for (int64_t i = 0; i < iterations; ++i) {
        ImportData import_data = CreateImportData(
            stdlib_path, /*additional_search_paths=*/{}, kDefaultWarningsSet,
            std::make_unique<RealFilesystem>());
        XLS_RETURN_IF_ERROR(
            ParseAndTypecheck(source, input_path, module_name, &import_data)
                .status());
        // ImportData owns the module, type information, and compilation-local
        // caches. Destruction at this scope boundary is part of the operation.
      }
      const auto elapsed = std::chrono::steady_clock::now() - start;
      std::cout << "{\"elapsed_ns\":"
                << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                       .count()
                << ",\"iterations\":" << iterations << "}\n";
      return absl::OkStatus();
    }
  }
}

}  // namespace
}  // namespace xls::dslx

int main(int argc, char* argv[]) {
  const std::vector<std::string_view> args =
      xls::InitXls(xls::dslx::kUsage, argc, argv);
  if (args.size() != 1) {
    return xls::ExitStatus(
        absl::InvalidArgumentError("Expected exactly one input DSLX file."));
  } else {
    return xls::ExitStatus(xls::dslx::RealMain(args.front()));
  }
}
