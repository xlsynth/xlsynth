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

#include "xls/public/c_api.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/macros.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "llvm/include/llvm/ADT/StringRef.h"
#include "llvm/include/llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/include/llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/include/llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/include/llvm/Support/Error.h"
#include "llvm/include/llvm/Support/MemoryBuffer.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/file/temp_directory.h"
#include "xls/common/logging/log_lines.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/jit/aot_entrypoint.pb.h"
#include "xls/jit/jit_buffer.h"
#include "xls/public/c_api_dslx.h"
#include "xls/public/c_api_dslx_internal.h"
#include "xls/public/c_api_format_preference.h"
#include "xls/public/c_api_ir_analysis.h"
#include "xls/public/c_api_ir_builder.h"

namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Smoke test for `xls_convert_dslx_to_ir` C API.
TEST(XlsCApiTest, ConvertDslxToIrSimple) {
  const std::string kProgram = "fn id(x: u32) -> u32 { x }";
  const char* additional_search_paths[] = {};
  char* error_out = nullptr;
  char* ir_out = nullptr;
  std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  bool ok =
      xls_convert_dslx_to_ir(kProgram.c_str(), "my_module.x", "my_module",
                             /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(),
                             additional_search_paths, 0, &error_out, &ir_out);

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error_out);
    xls_c_str_free(ir_out);
  });

  // We should get IR and no error.
  ASSERT_TRUE(ok);
  ASSERT_EQ(error_out, nullptr);
  ASSERT_NE(ir_out, nullptr);

  EXPECT_THAT(ir_out, HasSubstr("fn __my_module__id"));
}

TEST(XlsCApiTest, DslxBuildFunctionCallGraph) {
  constexpr std::string_view kProgram = R"DSLX(
fn callee(x: u32) -> u32 {
  x
}

fn caller(x: u32) -> u32 {
  callee(x) + callee(x)
}

fn apply_map(xs: u32[2]) -> u32[2] {
  map(xs, callee)
}
)DSLX";

  const char* additional_search_paths[] = {};
  std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      dslx_stdlib_path.c_str(), additional_search_paths, 0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [import_data] { xls_dslx_import_data_free(import_data); });

  char* error_out = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(std::string(kProgram).c_str(),
                                           "call_graph.x", "call_graph",
                                           import_data, &error_out, &tm));
  absl::Cleanup free_tm([tm] { xls_dslx_typechecked_module_free(tm); });
  xls_c_str_free(error_out);
  error_out = nullptr;

  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  ASSERT_NE(type_info, nullptr);
  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_NE(module, nullptr);
  xls_dslx_call_graph* graph = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_build_function_call_graph_for_module(
      type_info, module, &error_out, &graph));
  absl::Cleanup free_graph([graph] { xls_dslx_call_graph_free(graph); });
  ASSERT_EQ(error_out, nullptr);
  ASSERT_NE(graph, nullptr);

  ASSERT_EQ(xls_dslx_call_graph_get_function_count(graph), 3);

  auto get_fn_name = [](xls_dslx_function* fn) {
    char* identifier = xls_dslx_function_get_identifier(fn);
    std::string result(identifier);
    xls_c_str_free(identifier);
    return result;
  };

  xls_dslx_function* fn0 = xls_dslx_call_graph_get_function(graph, 0);
  ASSERT_NE(fn0, nullptr);
  EXPECT_EQ(get_fn_name(fn0), "callee");

  xls_dslx_function* fn1 = xls_dslx_call_graph_get_function(graph, 1);
  ASSERT_NE(fn1, nullptr);
  EXPECT_EQ(get_fn_name(fn1), "caller");

  xls_dslx_function* fn2 = xls_dslx_call_graph_get_function(graph, 2);
  ASSERT_NE(fn2, nullptr);
  EXPECT_EQ(get_fn_name(fn2), "apply_map");

  EXPECT_EQ(xls_dslx_call_graph_get_callee_count(graph, fn0), 0);

  ASSERT_EQ(xls_dslx_call_graph_get_callee_count(graph, fn1), 1);
  xls_dslx_function* caller_callee =
      xls_dslx_call_graph_get_callee_function(graph, fn1, 0);
  ASSERT_NE(caller_callee, nullptr);
  EXPECT_EQ(get_fn_name(caller_callee), "callee");

  ASSERT_EQ(xls_dslx_call_graph_get_callee_count(graph, fn2), 1);
  xls_dslx_function* mapped_fn =
      xls_dslx_call_graph_get_callee_function(graph, fn2, 0);
  ASSERT_NE(mapped_fn, nullptr);
  EXPECT_EQ(get_fn_name(mapped_fn), "callee");

  xls_c_str_free(error_out);
}

TEST(XlsCApiTest, DslxBuildFunctionCallGraphDeprecatedErrors) {
  constexpr std::string_view kProgram = R"DSLX(
fn callee(x: u32) -> u32 {
  x
}
fn caller(x: u32) -> u32 {
  callee(x)
}
)DSLX";

  const char* additional_search_paths[] = {};
  std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      dslx_stdlib_path.c_str(), additional_search_paths, 0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [import_data] { xls_dslx_import_data_free(import_data); });

  char* error_out = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(std::string(kProgram).c_str(),
                                           "call_graph.x", "call_graph",
                                           import_data, &error_out, &tm));
  absl::Cleanup free_tm([tm] { xls_dslx_typechecked_module_free(tm); });
  xls_c_str_free(error_out);
  error_out = nullptr;

  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  ASSERT_NE(type_info, nullptr);
  xls_dslx_call_graph* graph = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  ASSERT_FALSE(xls_dslx_type_info_build_function_call_graph(
      type_info, &error_out, &graph));
#pragma clang diagnostic pop
  ASSERT_EQ(graph, nullptr);
  ASSERT_NE(error_out, nullptr);
  EXPECT_THAT(error_out, HasSubstr("is deprecated"));
  xls_c_str_free(error_out);
}

TEST(XlsCApiTest, FunctionInsertSpecialization) {
  const std::string kProgram = R"(fn id<N: u32>(x: bits[N]) -> bits[N] { x }
fn call() -> bits[32] { id(bits[32]:0x0) }
)";
  const char* additional_search_paths[] = {};
  std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      dslx_stdlib_path.c_str(), additional_search_paths, 0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* parse_error = nullptr;
  struct xls_dslx_typechecked_module* tm = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.c_str(), "specialize.x",
                                         "specialize_module", import_data,
                                         &parse_error, &tm);
  absl::Cleanup free_parse_error([&] { xls_c_str_free(parse_error); });
  ASSERT_TRUE(ok) << (parse_error ? parse_error : "");
  ASSERT_NE(tm, nullptr);
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });

  struct xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_NE(module, nullptr);

  ASSERT_EQ(xls_dslx_module_get_member_count(module), 2);
  struct xls_dslx_module_member* member0 =
      xls_dslx_module_get_member(module, 0);
  ASSERT_EQ(xls_dslx_module_member_get_kind(member0),
            xls_dslx_module_member_kind_function);
  struct xls_dslx_function* source_function =
      xls_dslx_module_member_get_function(member0);
  ASSERT_TRUE(xls_dslx_function_is_parametric(source_function));

  struct xls_dslx_interp_value* value =
      xls_dslx_interp_value_make_ubits(/*bit_count=*/32, /*value=*/32);
  absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });

  xls_dslx_parametric_env_item items[] = {{"N", value}};
  char* env_error = nullptr;
  struct xls_dslx_parametric_env* env = nullptr;
  ASSERT_TRUE(xls_dslx_parametric_env_create(items, /*items_count=*/1,
                                             &env_error, &env));
  absl::Cleanup free_env([&] { xls_dslx_parametric_env_free(env); });
  absl::Cleanup free_env_error([&] { xls_c_str_free(env_error); });

  xls_dslx_function_specialization_request requests[] = {
      {.function_name = "id", .specialized_name = "id_N32", .env = env},
  };

  char* missing_context_error = nullptr;
  xls_dslx_typechecked_module* missing_context_result = nullptr;
  absl::Cleanup free_missing_context_error(
      [&] { xls_c_str_free(missing_context_error); });
  EXPECT_FALSE(xls_dslx_typechecked_module_insert_function_specializations(
      tm, requests, /*request_count=*/1, /*import_data=*/nullptr,
      "specialize_module.missing_context", &missing_context_error,
      &missing_context_result));
  EXPECT_EQ(missing_context_result, nullptr);
  ASSERT_NE(missing_context_error, nullptr);
  EXPECT_THAT(missing_context_error, HasSubstr("ImportData must be provided"));

  char* specialize_error = nullptr;
  struct xls_dslx_typechecked_module* specialized_tm = nullptr;
  ASSERT_TRUE(xls_dslx_typechecked_module_insert_function_specializations(
      tm, requests, /*request_count=*/1, import_data,
      "specialize_module.specializations", &specialize_error, &specialized_tm))
      << "specialization error: "
      << (specialize_error == nullptr ? "<none>" : specialize_error);
  absl::Cleanup free_specialize_error(
      [&] { xls_c_str_free(specialize_error); });
  ASSERT_NE(specialized_tm, nullptr);
  absl::Cleanup free_specialized_tm(
      [&] { xls_dslx_typechecked_module_free(specialized_tm); });

  struct xls_dslx_module* specialized_module =
      xls_dslx_typechecked_module_get_module(specialized_tm);
  ASSERT_NE(specialized_module, nullptr);

  EXPECT_EQ(xls_dslx_module_get_member_count(module), 2);
  ASSERT_EQ(xls_dslx_module_get_member_count(specialized_module), 3);
  struct xls_dslx_module_member* member1 =
      xls_dslx_module_get_member(specialized_module, 1);
  ASSERT_EQ(xls_dslx_module_member_get_kind(member1),
            xls_dslx_module_member_kind_function);
  struct xls_dslx_function* specialized_function =
      xls_dslx_module_member_get_function(member1);
  ASSERT_NE(specialized_function, nullptr);

  struct xls_dslx_module_member* member2 =
      xls_dslx_module_get_member(specialized_module, 2);
  ASSERT_EQ(xls_dslx_module_member_get_kind(member2),
            xls_dslx_module_member_kind_function);
  struct xls_dslx_function* call_function =
      xls_dslx_module_member_get_function(member2);
  ASSERT_NE(call_function, nullptr);

  char* name_source = xls_dslx_function_get_identifier(source_function);
  char* name_specialized =
      xls_dslx_function_get_identifier(specialized_function);
  char* name_call = xls_dslx_function_get_identifier(call_function);
  absl::Cleanup free_name_source([&] { xls_c_str_free(name_source); });
  absl::Cleanup free_name_specialized(
      [&] { xls_c_str_free(name_specialized); });
  absl::Cleanup free_name_call([&] { xls_c_str_free(name_call); });

  EXPECT_STREQ(name_source, "id");
  EXPECT_STREQ(name_specialized, "id_N32");
  EXPECT_STREQ(name_call, "call");
}

TEST(XlsCApiTest, FunctionInsertSpecializationWithConstantSumPayloadPatterns) {
  // The constants keep nominal types out of the specialized signature while
  // both payload-pattern forms still require synthetic spans in its body.
  const char kProgram[] = R"(enum Payload {
    Tuple(u8, u8),
    Struct { left: u8, right: u8 },
}

const TUPLE: Payload = Payload::Tuple(u8:1, u8:2);
const RECORD: Payload = Payload::Struct { left: u8:3, right: u8:4 };

pub fn add_payload<N: u32>(bias: bits[N]) -> bits[N] {
    let tuple_sum = match TUPLE {
        Payload::Tuple(left, right) => (left as bits[N]) + (right as bits[N]),
        _ => bits[N]:0,
    };
    let struct_sum = match RECORD {
        Payload::Struct { left, right } => (left as bits[N]) + (right as bits[N]),
        _ => bits[N]:0,
    };
    tuple_sum + struct_sum + bias
}
)";
  const std::string dslx_stdlib_path(xls::kDefaultDslxStdlibPath);
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      dslx_stdlib_path.c_str(),
      /*additional_search_paths=*/nullptr, /*additional_search_paths_count=*/0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* tm = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "sum_specialize.x", "sum_specialize", import_data, &error, &tm))
      << (error == nullptr ? "" : error);
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(xls_dslx_module_get_member_count(module), 4);
  xls_dslx_function* source_function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 3));
  ASSERT_NE(source_function, nullptr);
  ASSERT_TRUE(xls_dslx_function_is_parametric(source_function));
  char* original_text = xls_dslx_module_to_string(module);
  absl::Cleanup free_original_text([&] { xls_c_str_free(original_text); });

  xls_dslx_interp_value* width =
      xls_dslx_interp_value_make_ubits(/*bit_count=*/32, /*value=*/16);
  absl::Cleanup free_width([&] { xls_dslx_interp_value_free(width); });
  xls_dslx_parametric_env_item items[] = {{"N", width}};
  xls_dslx_parametric_env* env = nullptr;
  ASSERT_TRUE(
      xls_dslx_parametric_env_create(items, /*items_count=*/1, &error, &env))
      << (error == nullptr ? "" : error);
  absl::Cleanup free_env([&] { xls_dslx_parametric_env_free(env); });
  xls_dslx_function_specialization_request requests[] = {
      {.function_name = "add_payload",
       .specialized_name = "add_payload_N16",
       .env = env},
  };

  xls_dslx_typechecked_module* specialized_tm = nullptr;
  ASSERT_TRUE(xls_dslx_typechecked_module_insert_function_specializations(
      tm, requests, /*request_count=*/1, import_data,
      "sum_specialize.specializations", &error, &specialized_tm))
      << (error == nullptr ? "" : error);
  absl::Cleanup free_specialized_tm(
      [&] { xls_dslx_typechecked_module_free(specialized_tm); });
  ASSERT_NE(specialized_tm, nullptr);

  xls_dslx_module* specialized_module =
      xls_dslx_typechecked_module_get_module(specialized_tm);
  ASSERT_NE(specialized_module, nullptr);
  EXPECT_NE(specialized_module, module);
  ASSERT_EQ(xls_dslx_module_get_member_count(specialized_module), 5);
  xls_dslx_function* specialized_function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(specialized_module, 4));
  ASSERT_NE(specialized_function, nullptr);
  EXPECT_FALSE(xls_dslx_function_is_parametric(specialized_function));
  char* specialized_name =
      xls_dslx_function_get_identifier(specialized_function);
  absl::Cleanup free_specialized_name(
      [&] { xls_c_str_free(specialized_name); });
  EXPECT_STREQ(specialized_name, "add_payload_N16");

  // Query the actual re-typechecked AST before round-tripping its source to IR.
  xls_dslx_type_info* type_info =
      xls_dslx_typechecked_module_get_type_info(specialized_tm);
  ASSERT_NE(type_info, nullptr);
  ASSERT_EQ(xls_dslx_function_get_param_count(specialized_function), 1);
  for (xls_dslx_type_annotation* annotation :
       {xls_dslx_param_get_type_annotation(
            xls_dslx_function_get_param(specialized_function, 0)),
        xls_dslx_function_get_return_type(specialized_function)}) {
    ASSERT_NE(annotation, nullptr);
    const xls_dslx_type* type =
        xls_dslx_type_info_get_type_type_annotation(type_info, annotation);
    ASSERT_NE(type, nullptr);
    int64_t bit_count = 0;
    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(type, &error, &bit_count))
        << (error == nullptr ? "" : error);
    EXPECT_EQ(bit_count, 16);
  }

  char* original_text_after = xls_dslx_module_to_string(module);
  absl::Cleanup free_original_text_after(
      [&] { xls_c_str_free(original_text_after); });
  EXPECT_STREQ(original_text_after, original_text);
  EXPECT_EQ(xls_dslx_module_get_member_count(module), 4);
  EXPECT_TRUE(xls_dslx_function_is_parametric(source_function));

  char* specialized_text = xls_dslx_module_to_string(specialized_module);
  absl::Cleanup free_specialized_text(
      [&] { xls_c_str_free(specialized_text); });
  char* ir = nullptr;
  absl::Cleanup free_ir([&] { xls_c_str_free(ir); });
  ASSERT_TRUE(xls_convert_dslx_to_ir(
      specialized_text, "sum_specialized.x", "sum_specialized",
      dslx_stdlib_path.c_str(), /*additional_search_paths=*/nullptr,
      /*additional_search_paths_count=*/0, &error, &ir))
      << (error == nullptr ? "" : error);
  xls_package* package = nullptr;
  absl::Cleanup free_package([&] { xls_package_free(package); });
  ASSERT_TRUE(xls_parse_ir_package(ir, "sum_specialized.ir", &error, &package))
      << (error == nullptr ? "" : error);
  char* ir_name = nullptr;
  absl::Cleanup free_ir_name([&] { xls_c_str_free(ir_name); });
  ASSERT_TRUE(xls_mangle_dslx_name("sum_specialized", specialized_name, &error,
                                   &ir_name))
      << (error == nullptr ? "" : error);
  xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, ir_name, &error, &function))
      << (error == nullptr ? "" : error);
  xls_value* bias = nullptr;
  absl::Cleanup free_bias([&] { xls_value_free(bias); });
  ASSERT_TRUE(
      xls_value_make_ubits(/*bit_count=*/16, /*value=*/5, &error, &bias))
      << (error == nullptr ? "" : error);
  const xls_value* args[] = {bias};
  xls_value* result = nullptr;
  absl::Cleanup free_result([&] { xls_value_free(result); });
  ASSERT_TRUE(
      xls_interpret_function(function, /*argc=*/1, args, &error, &result))
      << (error == nullptr ? "" : error);
  char* result_text = nullptr;
  absl::Cleanup free_result_text([&] { xls_c_str_free(result_text); });
  ASSERT_TRUE(xls_value_to_string(result, &result_text));
  EXPECT_STREQ(result_text, "bits[16]:15");  // 1 + 2 + 3 + 4 + bias.
}

// -- Bits comparisons

TEST(XlsCApiTest, BitsUnsignedComparisonsMixedWidths) {
  char* error_out = nullptr;
  xls_bits* a8 = nullptr;
  xls_bits* b16 = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(8, /*value=*/0x10, &error_out, &a8));
  absl::Cleanup free_a8([a8] { xls_bits_free(a8); });
  ASSERT_TRUE(xls_bits_make_ubits(16, /*value=*/0x20, &error_out, &b16));
  absl::Cleanup free_b16([b16] { xls_bits_free(b16); });

  EXPECT_TRUE(xls_bits_ult(a8, b16));
  EXPECT_TRUE(xls_bits_ule(a8, b16));
  EXPECT_FALSE(xls_bits_ugt(a8, b16));
  EXPECT_FALSE(xls_bits_uge(a8, b16));

  // Reflexive equal case across mixed widths.
  xls_bits* a16_same = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(16, /*value=*/0x0010, &error_out, &a16_same));
  absl::Cleanup free_a16_same([a16_same] { xls_bits_free(a16_same); });
  EXPECT_FALSE(xls_bits_ult(a8, a16_same));
  EXPECT_TRUE(xls_bits_ule(a8, a16_same));
  EXPECT_FALSE(xls_bits_ugt(a8, a16_same));
  EXPECT_TRUE(xls_bits_uge(a8, a16_same));
}

TEST(XlsCApiTest, BitsSignedComparisons) {
  char* error_out = nullptr;
  xls_bits* neg8 = nullptr;
  xls_bits* pos8 = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/-1, &error_out, &neg8));
  absl::Cleanup free_neg8([neg8] { xls_bits_free(neg8); });
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/1, &error_out, &pos8));
  absl::Cleanup free_pos8([pos8] { xls_bits_free(pos8); });

  EXPECT_TRUE(xls_bits_slt(neg8, pos8));
  EXPECT_TRUE(xls_bits_sle(neg8, pos8));
  EXPECT_FALSE(xls_bits_sgt(neg8, pos8));
  EXPECT_FALSE(xls_bits_sge(neg8, pos8));

  // Equal values.
  xls_bits* neg8_copy = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/-1, &error_out, &neg8_copy));
  absl::Cleanup free_neg8_copy([neg8_copy] { xls_bits_free(neg8_copy); });
  EXPECT_FALSE(xls_bits_slt(neg8, neg8_copy));
  EXPECT_TRUE(xls_bits_sle(neg8, neg8_copy));
  EXPECT_FALSE(xls_bits_sgt(neg8, neg8_copy));
  EXPECT_TRUE(xls_bits_sge(neg8, neg8_copy));
}

TEST(XlsCApiTest, BitsSignedComparisonsMixedWidths) {
  char* error_out = nullptr;
  // -1 (8-bit) vs +1 (16-bit)
  xls_bits* neg8 = nullptr;
  xls_bits* pos16 = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/-1, &error_out, &neg8));
  absl::Cleanup free_neg8([neg8] { xls_bits_free(neg8); });
  ASSERT_TRUE(xls_bits_make_sbits(16, /*value=*/1, &error_out, &pos16));
  absl::Cleanup free_pos16([pos16] { xls_bits_free(pos16); });

  EXPECT_TRUE(xls_bits_slt(neg8, pos16));
  EXPECT_TRUE(xls_bits_sle(neg8, pos16));
  EXPECT_FALSE(xls_bits_sgt(neg8, pos16));
  EXPECT_FALSE(xls_bits_sge(neg8, pos16));

  // +1 (8-bit) vs -1 (16-bit)
  xls_bits* pos8 = nullptr;
  xls_bits* neg16 = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/1, &error_out, &pos8));
  absl::Cleanup free_pos8([pos8] { xls_bits_free(pos8); });
  ASSERT_TRUE(xls_bits_make_sbits(16, /*value=*/-1, &error_out, &neg16));
  absl::Cleanup free_neg16([neg16] { xls_bits_free(neg16); });

  EXPECT_FALSE(xls_bits_slt(pos8, neg16));
  EXPECT_FALSE(xls_bits_sle(pos8, neg16));
  EXPECT_TRUE(xls_bits_sgt(pos8, neg16));
  EXPECT_TRUE(xls_bits_sge(pos8, neg16));
}

TEST(XlsCApiTest, BitsEqualityAndInequalityComparisons) {
  char* error_out = nullptr;
  xls_bits* a = nullptr;
  xls_bits* b = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(5, 0b10101, &error_out, &a));
  absl::Cleanup free_a([a] { xls_bits_free(a); });
  ASSERT_TRUE(xls_bits_make_ubits(5, 0b10101, &error_out, &b));
  absl::Cleanup free_b([b] { xls_bits_free(b); });

  EXPECT_TRUE(xls_bits_eq(a, b));
  EXPECT_FALSE(xls_bits_ne(a, b));

  xls_bits* c = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(6, 0b010101, &error_out, &c));
  absl::Cleanup free_c([c] { xls_bits_free(c); });

  // Mixed-width equality should be false; inequality true.
  EXPECT_FALSE(xls_bits_eq(a, c));
  EXPECT_TRUE(xls_bits_ne(a, c));
}

TEST(XlsCApiTest, BitsUnsignedDivMod) {
  char* error_out = nullptr;
  xls_bits* lhs = nullptr;
  xls_bits* rhs = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(8, /*value=*/20, &error_out, &lhs));
  absl::Cleanup free_lhs([lhs] { xls_bits_free(lhs); });
  ASSERT_TRUE(xls_bits_make_ubits(8, /*value=*/6, &error_out, &rhs));
  absl::Cleanup free_rhs([rhs] { xls_bits_free(rhs); });

  xls_bits* div = xls_bits_udiv(lhs, rhs);
  absl::Cleanup free_div([div] { xls_bits_free(div); });
  xls_bits* mod = xls_bits_umod(lhs, rhs);
  absl::Cleanup free_mod([mod] { xls_bits_free(mod); });

  uint64_t div_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(div, &error_out, &div_value));
  EXPECT_EQ(div_value, 3);

  uint64_t mod_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(mod, &error_out, &mod_value));
  EXPECT_EQ(mod_value, 2);
}

TEST(XlsCApiTest, BitsSignedDivMod) {
  char* error_out = nullptr;
  xls_bits* lhs = nullptr;
  xls_bits* rhs = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/-18, &error_out, &lhs));
  absl::Cleanup free_lhs([lhs] { xls_bits_free(lhs); });
  ASSERT_TRUE(xls_bits_make_sbits(8, /*value=*/6, &error_out, &rhs));
  absl::Cleanup free_rhs([rhs] { xls_bits_free(rhs); });

  xls_bits* div = xls_bits_sdiv(lhs, rhs);
  absl::Cleanup free_div([div] { xls_bits_free(div); });
  xls_bits* mod = xls_bits_smod(lhs, rhs);
  absl::Cleanup free_mod([mod] { xls_bits_free(mod); });

  int64_t div_value = 0;
  ASSERT_TRUE(xls_bits_to_int64(div, &error_out, &div_value));
  EXPECT_EQ(div_value, -3);

  int64_t mod_value = 0;
  ASSERT_TRUE(xls_bits_to_int64(mod, &error_out, &mod_value));
  EXPECT_EQ(mod_value, 0);
}

// TODO(williamjhuang) - Many warnings that may be generated under TIv1 are not
// generated under TIv2, so we are forcing TIv1 in this case.
TEST(XlsCApiTest, ConvertDslxToIrWithWarningsSet) {
  const std::string kProgram = R"(#![feature(type_inference_v1)]
fn id() { let x = u32:1; })";
  const char* additional_search_paths[] = {};
  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);

  {
    char* error_out = nullptr;
    char* ir_out = nullptr;

    absl::Cleanup free_cstrs([&] {
      xls_c_str_free(error_out);
      xls_c_str_free(ir_out);
    });

    LOG(INFO)
        << "converting with warnings in default state, should see warning...";
    char** warnings = nullptr;
    size_t warnings_count = 0;
    absl::Cleanup free_warnings(
        [&] { xls_c_strs_free(warnings, warnings_count); });

    bool ok = xls_convert_dslx_to_ir_with_warnings(
        kProgram.c_str(), "my_module.x", "my_module",
        /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths,
        0,
        /*enable_warnings=*/nullptr, 0, /*disable_warnings=*/nullptr, 0,
        /*warnings_as_errors=*/true,
        /*force_implicit_token_calling_convention=*/false, &warnings,
        &warnings_count, &error_out, &ir_out);

    // Check we got the warning data even though the return code is non-ok.
    ASSERT_EQ(warnings_count, 1);
    ASSERT_NE(warnings, nullptr);
    ASSERT_NE(warnings[0], nullptr);
    EXPECT_THAT(warnings[0], HasSubstr("is not used in function"));

    // Since we set warnings-as-errors to true, we should have gotten "not ok"
    // back.
    ASSERT_FALSE(ok);
    ASSERT_EQ(ir_out, nullptr);
    EXPECT_THAT(error_out, HasSubstr("Conversion of DSLX to IR failed due to "
                                     "warnings during parsing/typechecking."));
  }

  // Now try with the warning disabled.
  {
    char* error_out = nullptr;
    char* ir_out = nullptr;

    absl::Cleanup free_cstrs([&] {
      xls_c_str_free(error_out);
      xls_c_str_free(ir_out);
    });
    const char* enable_warnings[] = {};
    const char* disable_warnings[] = {"unused_definition"};
    LOG(INFO) << "converting with warning disabled, should not see warning...";
    bool ok = xls_convert_dslx_to_ir_with_warnings(
        kProgram.c_str(), "my_module.x", "my_module",
        /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths,
        0, enable_warnings, 0, disable_warnings, 1, /*warnings_as_errors=*/true,
        /*force_implicit_token_calling_convention=*/false,
        /*warnings_out=*/nullptr, /*warnings_out_count=*/nullptr, &error_out,
        &ir_out);
    ASSERT_TRUE(ok);
    ASSERT_EQ(error_out, nullptr);
    ASSERT_NE(ir_out, nullptr);
  }
}

TEST(XlsCApiTest, ConvertWithNoWarnings) {
  const std::string kProgram = "fn id(x: u32) -> u32 { x }";
  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  const char* additional_search_paths[] = {};
  char* error_out = nullptr;
  char* ir_out = nullptr;

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error_out);
    xls_c_str_free(ir_out);
  });

  char** warnings = nullptr;
  size_t warnings_count = 0;
  absl::Cleanup free_warnings(
      [&] { xls_c_strs_free(warnings, warnings_count); });

  bool ok = xls_convert_dslx_to_ir_with_warnings(
      kProgram.c_str(), "my_module.x", "my_module",
      /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths, 0,
      /*enable_warnings=*/nullptr, 0, /*disable_warnings=*/nullptr, 0,
      /*warnings_as_errors=*/true,
      /*force_implicit_token_calling_convention=*/false, &warnings,
      &warnings_count, &error_out, &ir_out);
  ASSERT_TRUE(ok);
  ASSERT_EQ(error_out, nullptr);
  ASSERT_NE(ir_out, nullptr);
  EXPECT_THAT(ir_out, HasSubstr("fn __my_module__id"));
  // Validate that in the no-warnings case we get a zero count and also a
  // nullptr value populating our warnings ptr.
  ASSERT_EQ(warnings_count, 0);
  ASSERT_EQ(warnings, nullptr);
}

TEST(XlsCApiTest, ConvertWithForcedImplicitToken) {
  const std::string kProgram = "fn id(x: u32) -> u32 { x }";
  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  const char* additional_search_paths[] = {};
  char* error_out = nullptr;
  char* ir_out = nullptr;

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error_out);
    xls_c_str_free(ir_out);
  });

  bool ok = xls_convert_dslx_to_ir_with_warnings(
      kProgram.c_str(), "my_module.x", "my_module",
      /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths, 0,
      /*enable_warnings=*/nullptr, 0, /*disable_warnings=*/nullptr, 0,
      /*warnings_as_errors=*/false,
      /*force_implicit_token_calling_convention=*/true,
      /*warnings_out=*/nullptr, /*warnings_out_count=*/nullptr, &error_out,
      &ir_out);
  ASSERT_TRUE(ok);
  ASSERT_EQ(error_out, nullptr);
  ASSERT_NE(ir_out, nullptr);
  EXPECT_THAT(ir_out, HasSubstr("fn __itok__my_module__id"));
}

TEST(XlsCApiTest, ConvertDslxToIrError) {
  const std::string kInvalidProgram = "@!";
  const char* additional_search_paths[] = {};
  char* error_out = nullptr;
  char* ir_out = nullptr;

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error_out);
    xls_c_str_free(ir_out);
  });

  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  bool ok = xls_convert_dslx_to_ir(
      kInvalidProgram.c_str(), "my_module.x", "my_module",
      /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths, 0,
      &error_out, &ir_out);
  ASSERT_FALSE(ok);

  // We should get an error and not get IR.
  ASSERT_NE(error_out, nullptr);
  ASSERT_EQ(ir_out, nullptr);

  EXPECT_THAT(error_out, HasSubstr("Unrecognized character: '@'"));
}

// Smoke test for `xls_convert_dslx_path_to_ir` C API.
TEST(XlsCApiTest, ConvertDslxPathToIr) {
  const std::string kProgram = "fn id(x: u32) -> u32 { x }";

  XLS_ASSERT_OK_AND_ASSIGN(xls::TempDirectory tempdir,
                           xls::TempDirectory::Create());
  const std::filesystem::path module_path = tempdir.path() / "my_module.x";
  XLS_ASSERT_OK(xls::SetFileContents(module_path, kProgram));

  const char* additional_search_paths[] = {};
  char* error_out = nullptr;
  char* ir_out = nullptr;
  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  bool ok = xls_convert_dslx_path_to_ir(
      module_path.c_str(),
      /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(), additional_search_paths, 0,
      &error_out, &ir_out);

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error_out);
    xls_c_str_free(ir_out);
  });

  // We should get IR and no error.
  ASSERT_TRUE(ok);
  ASSERT_EQ(error_out, nullptr);
  ASSERT_NE(ir_out, nullptr);

  EXPECT_THAT(ir_out, HasSubstr("fn __my_module__id"));

  // Now we take the IR and schedule/codegen it.
  struct xls_package* package = nullptr;
  ASSERT_TRUE(
      xls_parse_ir_package(ir_out, "my_module.ir", &error_out, &package));
  absl::Cleanup free_package([package] { xls_package_free(package); });

  EXPECT_EQ(xls_package_get_top(package), nullptr);
  ASSERT_TRUE(
      xls_package_set_top_by_name(package, "__my_module__id", &error_out));
  EXPECT_NE(xls_package_get_top(package), nullptr);

  const char* kSchedulingOptionsFlagsProto = R"(
pipeline_stages: 1
delay_model: "unit"
)";
  const char* kCodegenFlagsProto = R"(
register_merge_strategy: STRATEGY_DONT_MERGE
generator: GENERATOR_KIND_PIPELINE
)";

  struct xls_schedule_and_codegen_result* result = nullptr;
  ASSERT_TRUE(xls_schedule_and_codegen_package(
      package, /*scheduling_options_flags_proto=*/kSchedulingOptionsFlagsProto,
      /*codegen_flags_proto=*/kCodegenFlagsProto, /*with_delay_model=*/false,
      &error_out, &result))
      << "xls_schedule_and_codegen_package error: " << error_out;
  absl::Cleanup free_result(
      [result] { xls_schedule_and_codegen_result_free(result); });

  char* verilog_out = xls_schedule_and_codegen_result_get_verilog_text(result);
  ASSERT_NE(verilog_out, nullptr);
  absl::Cleanup free_verilog([verilog_out] { xls_c_str_free(verilog_out); });

  LOG(INFO) << "== Verilog";
  XLS_LOG_LINES(INFO, verilog_out);

  EXPECT_THAT(verilog_out, HasSubstr("module __my_module__id"));
}

TEST(XlsCApiTest, ParseTypedValueAndFreeIt) {
  char* error = nullptr;
  struct xls_value* value = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[32]:0x42", &error, &value));

  char* string_out = nullptr;
  ASSERT_TRUE(xls_value_to_string(value, &string_out));
  EXPECT_EQ(std::string{string_out}, "bits[32]:66");
  xls_c_str_free(string_out);
  string_out = nullptr;

  // Also to-string it via a format preference.
  xls_format_preference fmt_pref;
  ASSERT_TRUE(xls_format_preference_from_string("hex", &error, &fmt_pref));
  ASSERT_TRUE(xls_value_to_string_format_preference(value, fmt_pref, &error,
                                                    &string_out));
  EXPECT_EQ(std::string{string_out}, "bits[32]:0x42");
  xls_c_str_free(string_out);

  xls_value_free(value);
}

// Takes a bits-based value and flattens it to a bits buffer and checks it's the
// same as the bits inside the value.
TEST(XlsCApiTest, FlattenBitsValueToBits) {
  char* error_out = nullptr;
  xls_value* value = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[32]:0x42", &error_out, &value));
  absl::Cleanup free_value([value] { xls_value_free(value); });

  xls_value_kind kind = xls_value_kind_invalid;
  ASSERT_TRUE(xls_value_get_kind(value, &error_out, &kind));
  EXPECT_EQ(kind, xls_value_kind_bits);
  ASSERT_EQ(error_out, nullptr);

  // Get the bits from within the value. Note that it's owned by the caller.
  xls_bits* value_bits = nullptr;
  ASSERT_TRUE(xls_value_get_bits(value, &error_out, &value_bits));
  absl::Cleanup free_value_bits([value_bits] { xls_bits_free(value_bits); });

  // Flatten the value to a bits buffer.
  xls_bits* flattened = xls_value_flatten_to_bits(value);
  absl::Cleanup free_flattened([flattened] { xls_bits_free(flattened); });

  // The flattened bits should be the same as the original bits.
  char* value_bits_str = xls_bits_to_debug_string(value_bits);
  absl::Cleanup free_value_bits_str(
      [=] { xls_c_str_free(value_bits_str); });  // Ensure free
  char* flattened_str = xls_bits_to_debug_string(flattened);
  absl::Cleanup free_flattened_str(
      [=] { xls_c_str_free(flattened_str); });  // Ensure free
  EXPECT_TRUE(xls_bits_eq(value_bits, flattened))
      << "value_bits: " << value_bits_str << "\nflattened:  " << flattened_str;
}

TEST(XlsCApiTest, FlattenTupleValueToBits) {
  char* error_out = nullptr;
  xls_value* u3_7 = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[3]:0x7", &error_out, &u3_7));
  absl::Cleanup free_u3_7([u3_7] { xls_value_free(u3_7); });

  xls_value* u2_0 = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[2]:0x0", &error_out, &u2_0));
  absl::Cleanup free_u2_0([u2_0] { xls_value_free(u2_0); });

  // Make them into a tuple; i.e. (bits[3]:0x7, bits[2]:0x0).
  xls_value* elements[] = {u3_7, u2_0};
  xls_value* tuple = xls_value_make_tuple(/*element_count=*/2, elements);
  absl::Cleanup free_tuple([tuple] { xls_value_free(tuple); });

  xls_value_kind kind = xls_value_kind_invalid;
  ASSERT_TRUE(xls_value_get_kind(tuple, &error_out, &kind));
  EXPECT_EQ(kind, xls_value_kind_tuple);

  // Get the elements and check they are equal to the originals.
  xls_value* u3_7_extracted = nullptr;
  ASSERT_TRUE(xls_value_get_element(tuple, 0, &error_out, &u3_7_extracted));
  absl::Cleanup free_u3_7_extracted(
      [u3_7_extracted] { xls_value_free(u3_7_extracted); });
  EXPECT_TRUE(xls_value_eq(u3_7, u3_7_extracted));
  // White-box check that the pointers are not the same as the extracted value
  // is an independent value owned by the caller.
  EXPECT_NE(u3_7, u3_7_extracted);

  xls_value* u2_0_extracted = nullptr;
  ASSERT_TRUE(xls_value_get_element(tuple, 1, &error_out, &u2_0_extracted));
  absl::Cleanup free_u2_0_extracted(
      [u2_0_extracted] { xls_value_free(u2_0_extracted); });
  EXPECT_TRUE(xls_value_eq(u2_0, u2_0_extracted));
  // White-box check that the pointers are not the same as the extracted value
  // is an independent value owned by the caller.
  EXPECT_NE(u2_0, u2_0_extracted);

  // Flatten the tuple to a bits value.
  xls_bits* flattened = xls_value_flatten_to_bits(tuple);
  absl::Cleanup free_flattened([flattened] { xls_bits_free(flattened); });

  // Make the desired value.
  xls_bits* want_bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(5, 0b11100, &error_out, &want_bits));
  absl::Cleanup free_want_bits([want_bits] { xls_bits_free(want_bits); });

  // Check they're equivalent.
  char* flattened_str = xls_bits_to_debug_string(flattened);
  absl::Cleanup free_flattened_str(
      [=] { xls_c_str_free(flattened_str); });  // Ensure free
  char* want_bits_str = xls_bits_to_debug_string(want_bits);
  absl::Cleanup free_want_bits_str(
      [=] { xls_c_str_free(want_bits_str); });  // Ensure free
  EXPECT_TRUE(xls_bits_eq(flattened, want_bits))
      << "flattened: " << flattened_str << "\nwant_bits: " << want_bits_str;
}

TEST(XlsCApiTest, MakeArrayValue) {
  char* error_out = nullptr;

  xls_value* u3_7 = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[3]:0x7", &error_out, &u3_7));
  absl::Cleanup free_u3_7([u3_7] { xls_value_free(u3_7); });

  xls_value* u2_0 = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[2]:0x0", &error_out, &u2_0));
  absl::Cleanup free_u2_0([u2_0] { xls_value_free(u2_0); });

  // Make a valid array of two elements.
  {
    xls_value* elements[] = {u3_7, u3_7};
    xls_value* array = nullptr;
    ASSERT_TRUE(xls_value_make_array(
        /*element_count=*/2, elements, &error_out, &array));
    absl::Cleanup free_array([array] { xls_value_free(array); });

    xls_value_kind kind = xls_value_kind_invalid;
    ASSERT_TRUE(xls_value_get_kind(array, &error_out, &kind));
    EXPECT_EQ(kind, xls_value_kind_array);

    char* value_str = nullptr;
    ASSERT_TRUE(xls_value_to_string(array, &value_str));
    absl::Cleanup free_value_str([value_str] { xls_c_str_free(value_str); });
    EXPECT_EQ(std::string_view{value_str}, "[bits[3]:7, bits[3]:7]");
  }

  // Make an invalid array of two elements.
  {
    xls_value* elements[] = {u3_7, u2_0};
    xls_value* array = nullptr;
    ASSERT_FALSE(xls_value_make_array(
        /*element_count=*/2, elements, &error_out, &array));
    absl::Cleanup free_error([error_out] { xls_c_str_free(error_out); });
    EXPECT_THAT(std::string_view{error_out}, HasSubstr("SameTypeAs"));
  }
}

TEST(XlsCApiTest, MakeBitsFromUint8DataWithMsbPadding) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(6, 0b11'0000, &error_out, &bits));
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });

  EXPECT_EQ(xls_bits_get_bit_count(bits), 6);
  EXPECT_EQ(xls_bits_get_bit(bits, 0), 0);
  EXPECT_EQ(xls_bits_get_bit(bits, 1), 0);
  EXPECT_EQ(xls_bits_get_bit(bits, 2), 0);
  EXPECT_EQ(xls_bits_get_bit(bits, 3), 0);
  EXPECT_EQ(xls_bits_get_bit(bits, 4), 1);
  EXPECT_EQ(xls_bits_get_bit(bits, 5), 1);
}

TEST(XlsCApiTest, BitsToBytes) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(32, 0x0A0B0C0D, &error_out, &bits));
  uint8_t* bytes = nullptr;
  size_t byte_count = 0;

  absl::Cleanup free_memory([&error_out, &bits, &bytes] {
    xls_c_str_free(error_out);
    xls_bits_free(bits);
    xls_bytes_free(bytes);
  });

  ASSERT_TRUE(xls_bits_to_bytes(bits, &error_out, &bytes, &byte_count));
  EXPECT_EQ(byte_count, 4);
  EXPECT_EQ(bytes[0], 0x0D);
  EXPECT_EQ(bytes[1], 0x0C);
  EXPECT_EQ(bytes[2], 0x0B);
  EXPECT_EQ(bytes[3], 0x0A);
}

TEST(XlsCApiTest, BitsToBytesWithPadding) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(
      34, 0b11'0000'0000'0000'0000'0000'0000'0000'0000, &error_out, &bits));
  uint8_t* bytes = nullptr;
  size_t byte_count = 0;

  absl::Cleanup free_memory([&error_out, &bits, &bytes] {
    xls_c_str_free(error_out);
    xls_bits_free(bits);
    xls_bytes_free(bytes);
  });

  ASSERT_TRUE(xls_bits_to_bytes(bits, &error_out, &bytes, &byte_count));
  EXPECT_EQ(byte_count, 5);
  EXPECT_EQ(bytes[0], 0x00);
  EXPECT_EQ(bytes[1], 0x00);
  EXPECT_EQ(bytes[2], 0x00);
  EXPECT_EQ(bytes[3], 0x00);
  EXPECT_EQ(bytes[4], 0b11);
}

TEST(XlsCApiTest, BitsToUint64Fit) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(64, 0x0A0B0C0D, &error_out, &bits));
  absl::Cleanup free_memory([&error_out, &bits] {
    xls_c_str_free(error_out);
    xls_bits_free(bits);
  });

  uint64_t value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(bits, &error_out, &value));
  EXPECT_EQ(value, 0x0A0B0C0D);
}

TEST(XlsCApiTest, BitsToInt64Fit) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(64, 0x0A0B0C0D, &error_out, &bits));
  absl::Cleanup free_memory([&error_out, &bits] {
    xls_c_str_free(error_out);
    xls_bits_free(bits);
  });

  int64_t value = 0;
  ASSERT_TRUE(xls_bits_to_int64(bits, &error_out, &value));
  EXPECT_EQ(value, 0x0A0B0C0D);
}

TEST(XlsCApiTest, MakeSbitsDoesNotFit) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_FALSE(xls_bits_make_sbits(1, -2, &error_out, &bits));
  absl::Cleanup free_error([error_out] { xls_c_str_free(error_out); });
  EXPECT_THAT(std::string_view{error_out},
              HasSubstr("Value 0xfffffffffffffffe requires 2 bits to fit in an "
                        "signed datatype"));

  ASSERT_TRUE(xls_bits_make_sbits(2, -2, &error_out, &bits));
  char* bits_str = xls_bits_to_debug_string(bits);
  absl::Cleanup free_bits_str(
      [=] { xls_c_str_free(bits_str); });  // Ensure free
  EXPECT_EQ(std::string(bits_str), "0b10");
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });
}

TEST(XlsCApiTest, FlattenArrayValueToBits) {
  char* error_out = nullptr;
  xls_value* array = nullptr;
  ASSERT_TRUE(
      xls_parse_typed_value("[bits[3]:0x7, bits[3]:0x0]", &error_out, &array));
  absl::Cleanup free_array([array] { xls_value_free(array); });

  xls_bits* flattened = xls_value_flatten_to_bits(array);
  absl::Cleanup free_flattened([flattened] { xls_bits_free(flattened); });

  xls_bits* want_bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(6, 0b111000, &error_out, &want_bits));
  absl::Cleanup free_want_bits([want_bits] { xls_bits_free(want_bits); });

  char* flattened_str = xls_bits_to_debug_string(flattened);
  absl::Cleanup free_flattened_str(
      [=] { xls_c_str_free(flattened_str); });  // Ensure free
  char* want_bits_str = xls_bits_to_debug_string(want_bits);
  absl::Cleanup free_want_bits_str(
      [=] { xls_c_str_free(want_bits_str); });  // Ensure free
  EXPECT_TRUE(xls_bits_eq(flattened, want_bits))
      << "flattened: " << flattened_str << "\nwant_bits: " << want_bits_str;
}

TEST(XlsCApiTest, MakeBitsFromBytesNoPadding) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  xls_bits* bits_byte0 = nullptr;
  xls_bits* bits_byte1 = nullptr;
  uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04};

  ASSERT_TRUE(xls_bits_make_bits_from_bytes(16, bytes, 3, &error_out, &bits));

  ASSERT_NE(bits, nullptr);
  ASSERT_EQ(error_out, nullptr);
  EXPECT_EQ(xls_bits_get_bit_count(bits), 16);

  bits_byte0 = xls_bits_width_slice(bits, 0, 8);
  bits_byte1 = xls_bits_width_slice(bits, 8, 8);

  ASSERT_NE(bits_byte0, nullptr);
  ASSERT_NE(bits_byte1, nullptr);

  EXPECT_EQ(xls_bits_get_bit_count(bits_byte0), 8);

  uint64_t byte0_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(bits_byte0, &error_out, &byte0_value));
  ASSERT_EQ(error_out, nullptr);
  uint64_t byte1_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(bits_byte1, &error_out, &byte1_value));
  ASSERT_EQ(error_out, nullptr);

  ASSERT_EQ(byte0_value, 0x01);
  ASSERT_EQ(byte1_value, 0x02);

  absl::Cleanup free_bits([bits, bits_byte0, bits_byte1, error_out] {
    xls_bits_free(bits);
    xls_bits_free(bits_byte0);
    xls_bits_free(bits_byte1);
    xls_c_str_free(error_out);
  });
}

TEST(XlsCApiTest, MakeBitsFromBytesWithPadding) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  xls_bits* bits_byte0 = nullptr;
  xls_bits* bits_byte1 = nullptr;
  uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04};

  ASSERT_TRUE(xls_bits_make_bits_from_bytes(14, bytes, 2, &error_out, &bits));

  ASSERT_NE(bits, nullptr);
  ASSERT_EQ(error_out, nullptr);
  EXPECT_EQ(xls_bits_get_bit_count(bits), 14);

  bits_byte0 = xls_bits_width_slice(bits, 0, 8);
  bits_byte1 = xls_bits_width_slice(bits, 8, 6);

  ASSERT_NE(bits_byte0, nullptr);
  ASSERT_NE(bits_byte1, nullptr);

  EXPECT_EQ(xls_bits_get_bit_count(bits_byte0), 8);
  EXPECT_EQ(xls_bits_get_bit_count(bits_byte1), 6);

  uint64_t byte0_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(bits_byte0, &error_out, &byte0_value));
  ASSERT_EQ(error_out, nullptr);
  uint64_t byte1_value = 0;
  ASSERT_TRUE(xls_bits_to_uint64(bits_byte1, &error_out, &byte1_value));
  ASSERT_EQ(error_out, nullptr);

  ASSERT_EQ(byte0_value, 0x01);
  ASSERT_EQ(byte1_value, 0x02);

  absl::Cleanup free_bits([bits, bits_byte0, bits_byte1, error_out] {
    xls_bits_free(bits);
    xls_bits_free(bits_byte0);
    xls_bits_free(bits_byte1);
    xls_c_str_free(error_out);
  });
}

TEST(XlsCApiTest, MakeBitsFromBytesWithInvalidInputs) {
  uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04};
  {
    char* error_out = nullptr;
    xls_bits* bits = nullptr;
    ASSERT_FALSE(
        xls_bits_make_bits_from_bytes(16, nullptr, 0, &error_out, &bits));
    ASSERT_NE(error_out, nullptr);
    ASSERT_EQ(bits, nullptr);
    EXPECT_THAT(std::string_view{error_out}, HasSubstr("bytes is null"));
    absl::Cleanup free_error([&error_out] { xls_c_str_free(error_out); });
  }
  {
    char* error_out = nullptr;
    xls_bits* bits = nullptr;
    ASSERT_FALSE(
        xls_bits_make_bits_from_bytes(16, bytes, 0, &error_out, &bits));
    ASSERT_NE(error_out, nullptr);
    ASSERT_EQ(bits, nullptr);
    EXPECT_THAT(std::string_view{error_out}, HasSubstr("byte_count is 0"));
    absl::Cleanup free_error([&error_out] { xls_c_str_free(error_out); });
  }
  {
    char* error_out = nullptr;
    xls_bits* bits = nullptr;
    ASSERT_FALSE(xls_bits_make_bits_from_bytes(0, bytes, 5, &error_out, &bits));
    ASSERT_NE(error_out, nullptr);
    ASSERT_EQ(bits, nullptr);
    EXPECT_THAT(std::string_view{error_out}, HasSubstr("bit_count is 0"));
    absl::Cleanup free_error([&error_out] { xls_c_str_free(error_out); });
  }
  {
    char* error_out = nullptr;
    xls_bits* bits = nullptr;
    ASSERT_FALSE(xls_bits_make_bits_from_bytes(16, bytes, UINT64_MAX / 8 + 1,
                                               &error_out, &bits));
    ASSERT_NE(error_out, nullptr);
    ASSERT_EQ(bits, nullptr);
    EXPECT_THAT(std::string_view{error_out},
                HasSubstr("byte_count is too large"));
    absl::Cleanup free_error([&error_out] { xls_c_str_free(error_out); });
  }
  {
    char* error_out = nullptr;
    xls_bits* bits = nullptr;
    ASSERT_FALSE(
        xls_bits_make_bits_from_bytes(16, bytes, 1, &error_out, &bits));
    ASSERT_NE(error_out, nullptr);
    ASSERT_EQ(bits, nullptr);
    EXPECT_THAT(std::string_view{error_out},
                HasSubstr("byte_count*8 < bit_count"));
    absl::Cleanup free_error([&error_out] { xls_c_str_free(error_out); });
  }
}

TEST(XlsCApiTest, MakeSignedBits) {
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_sbits(5, -1, &error_out, &bits));
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });

  EXPECT_EQ(xls_bits_get_bit_count(bits), 5);
  EXPECT_EQ(xls_bits_get_bit(bits, 0), 1);

  xls_value* value = nullptr;
  ASSERT_TRUE(xls_value_make_sbits(5, -1, &error_out, &value));
  absl::Cleanup free_value([value] { xls_value_free(value); });

  xls_bits* value_bits = nullptr;
  ASSERT_TRUE(xls_value_get_bits(value, &error_out, &value_bits));
  absl::Cleanup free_value_bits([value_bits] { xls_bits_free(value_bits); });

  EXPECT_EQ(xls_bits_get_bit_count(value_bits), 5);
  EXPECT_EQ(xls_bits_get_bit(value_bits, 0), 1);
  // Check that they are equal even though they were created different ways, and
  // that their pointer are different because one is held inside a value (white
  // box knowledge but just a gut check).
  EXPECT_TRUE(xls_bits_eq(bits, value_bits));
  EXPECT_NE(bits, value_bits);
}

TEST(XlsCApiTest, MakeUnsignedBits) {
  // First create via the xls_bits_make_ubits API.
  char* error_out = nullptr;
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(5, 0b10101, &error_out, &bits));
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });

  // Now create via the xls_value_make_ubits API.
  xls_value* value = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(5, 0b10101, &error_out, &value));
  absl::Cleanup free_value([value] { xls_value_free(value); });

  // Check that the bits are the same.
  xls_bits* value_bits = nullptr;
  ASSERT_TRUE(xls_value_get_bits(value, &error_out, &value_bits));
  absl::Cleanup free_value_bits([value_bits] { xls_bits_free(value_bits); });

  EXPECT_TRUE(xls_bits_eq(bits, value_bits));
  EXPECT_NE(bits, value_bits);
}

TEST(XlsCApiTest, ParsePackageAndGetFunctions) {
  const std::string kPackage0 = R"(package p)";
  const std::string kPackage1 = R"(package p
fn f(x: bits[32] id=3) -> bits[32] {
  ret y: bits[32] = identity(x, id=2)
}
)";
  const std::string kPackage2 = R"(package p
fn f(x: bits[32] id=3) -> bits[32] {
  ret y: bits[32] = identity(x, id=2)
}
fn g(x: bits[32] id=4) -> bits[32] {
  ret y: bits[32] = identity(x, id=5)
}
)";

  char* error = nullptr;
  struct xls_package* package0 = nullptr;
  ASSERT_TRUE(
      xls_parse_ir_package(kPackage0.c_str(), "p.ir", &error, &package0))
      << "xls_parse_ir_package error: " << error;
  ASSERT_TRUE(error == nullptr);
  absl::Cleanup free_package0([&package0] { xls_package_free(package0); });

  struct xls_function** functions0 = nullptr;
  size_t function_count = 0;
  ASSERT_TRUE(xls_package_get_functions(package0, &error, &functions0,
                                        &function_count));
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package0, nullptr);

  ASSERT_EQ(function_count, 0);
  ASSERT_EQ(functions0, nullptr);

  ASSERT_EQ(error, nullptr);

  struct xls_package* package1 = nullptr;
  struct xls_function** functions1 = nullptr;
  ASSERT_TRUE(
      xls_parse_ir_package(kPackage1.c_str(), "p.ir", &error, &package1))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package1, nullptr);
  ASSERT_TRUE(xls_package_get_functions(package1, &error, &functions1,
                                        &function_count));
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_package_and_function_array1([&package1, &functions1] {
    xls_function_ptr_array_free(functions1);
    xls_package_free(package1);
  });
  ASSERT_EQ(function_count, 1);
  ASSERT_NE(functions1, nullptr);
  ASSERT_NE(functions1[0], nullptr);

  struct xls_package* package2 = nullptr;
  struct xls_function** functions2 = nullptr;
  ASSERT_TRUE(
      xls_parse_ir_package(kPackage2.c_str(), "p.ir", &error, &package2))
      << "xls_parse_ir_package error: " << error;
  ASSERT_NE(package2, nullptr);
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_package_get_functions(package2, &error, &functions2,
                                        &function_count));
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free__package_and_function_array2([&package2, &functions2] {
    xls_function_ptr_array_free(functions2);
    xls_package_free(package2);
  });
  ASSERT_EQ(error, nullptr);
  ASSERT_EQ(function_count, 2);
  ASSERT_NE(functions2, nullptr);
  ASSERT_NE(functions2[0], nullptr);
  ASSERT_NE(functions2[1], nullptr);
}

TEST(XlsCApiTest, ParsePackageAndInterpretFunctionInIt) {
  const std::string kPackage = R"(package p

fn f(x: bits[32] id=3) -> bits[32] {
  ret y: bits[32] = identity(x, id=2)
}
)";
  const std::string kFunction = R"(fn f(x: bits[32] id=3) -> bits[32] {
  ret y: bits[32] = identity(x, id=2)
}
)";

  char* error = nullptr;
  struct xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kPackage.c_str(), "p.ir", &error, &package))
      << "xls_parse_ir_package error: " << error;
  absl::Cleanup free_package([package] { xls_package_free(package); });

  struct xls_function** functions = nullptr;
  size_t function_count = 0;
  char* function_name = nullptr;
  ASSERT_TRUE(
      xls_package_get_functions(package, &error, &functions, &function_count));
  absl::Cleanup free_function_array_and_function_name(
      [&functions, &function_name] {
        xls_function_ptr_array_free(functions);
        xls_c_str_free(function_name);
      });
  ASSERT_EQ(function_count, 1);
  ASSERT_TRUE(xls_function_get_name(functions[0], &error, &function_name));
  EXPECT_EQ(std::string_view(function_name), "f");

  char* dumped = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &dumped));
  absl::Cleanup free_dumped([dumped] { xls_c_str_free(dumped); });
  EXPECT_EQ(std::string_view(dumped), kPackage);

  struct xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "f", &error, &function));

  char* function_dumped = nullptr;
  ASSERT_TRUE(xls_function_to_string(function, &function_dumped));
  absl::Cleanup free_function_dumped(
      [function_dumped] { xls_c_str_free(function_dumped); });
  EXPECT_EQ(std::string_view(function_dumped), kFunction);

  // Test out the get_name functionality on the function.
  char* name = nullptr;
  ASSERT_TRUE(xls_function_get_name(function, &error, &name));
  absl::Cleanup free_name([name] { xls_c_str_free(name); });
  EXPECT_EQ(std::string_view(name), "f");

  // Test out the get_type functionality on the function.
  struct xls_function_type* f_type = nullptr;
  ASSERT_TRUE(xls_function_get_type(function, &error, &f_type));

  char* type_str = nullptr;
  ASSERT_TRUE(xls_function_type_to_string(f_type, &error, &type_str));
  absl::Cleanup free_type_str([type_str] { xls_c_str_free(type_str); });
  EXPECT_EQ(std::string_view(type_str), "(bits[32]) -> bits[32]");

  int64_t param_count = xls_function_type_get_param_count(f_type);
  EXPECT_EQ(param_count, 1);

  struct xls_type* param_type = nullptr;
  ASSERT_TRUE(xls_function_type_get_param_type(f_type, /*index=*/0, &error,
                                               &param_type));
  ASSERT_NE(param_type, nullptr);

  xls_value_kind kind;
  char* kind_error = nullptr;
  ASSERT_TRUE(xls_type_get_kind(param_type, &kind_error, &kind));
  absl::Cleanup free_kind_error([kind_error] { xls_c_str_free(kind_error); });
  EXPECT_EQ(kind_error, nullptr);
  ASSERT_EQ(kind, xls_value_kind_bits);

  int64_t bit_count = xls_type_get_flat_bit_count(param_type);
  EXPECT_EQ(bit_count, 32);

  int64_t leaf_count = xls_type_get_leaf_count(param_type);
  EXPECT_EQ(leaf_count, 1);

  EXPECT_EQ(kind, xls_value_kind_bits);
  char* param_type_str = nullptr;
  ASSERT_TRUE(xls_type_to_string(param_type, &error, &param_type_str));
  absl::Cleanup free_param_type_str(
      [param_type_str] { xls_c_str_free(param_type_str); });
  EXPECT_EQ(std::string_view(param_type_str), "bits[32]");

  struct xls_type* ret_type = xls_function_type_get_return_type(f_type);
  ASSERT_NE(ret_type, nullptr);
  char* ret_type_str = nullptr;
  ASSERT_TRUE(xls_type_to_string(ret_type, &error, &ret_type_str));
  absl::Cleanup free_ret_type_str(
      [ret_type_str] { xls_c_str_free(ret_type_str); });
  EXPECT_EQ(std::string_view(ret_type_str), "bits[32]");

  struct xls_value* ft = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[32]:0x42", &error, &ft));
  absl::Cleanup free_ft([ft] { xls_value_free(ft); });

  // Check that we can get the type of the value.
  struct xls_type* ft_type = nullptr;
  ASSERT_TRUE(xls_package_get_type_for_value(package, ft, &error, &ft_type));

  // Now convert that type to a string so we can observe it.
  char* ft_type_str = nullptr;
  ASSERT_TRUE(xls_type_to_string(ft_type, &error, &ft_type_str));
  absl::Cleanup free_ft_type_str(
      [ft_type_str] { xls_c_str_free(ft_type_str); });
  EXPECT_EQ(std::string_view(ft_type_str), "bits[32]");

  const struct xls_value* args[] = {ft};

  struct xls_value* result = nullptr;
  ASSERT_TRUE(
      xls_interpret_function(function, /*argc=*/1, args, &error, &result));
  absl::Cleanup free_result([result] { xls_value_free(result); });

  ASSERT_TRUE(xls_value_eq(ft, result));
}

TEST(XlsCApiTest, ParsePackageAndOptimizeFunctionInIt) {
  const std::string kPackage = R"(
package p

fn f() -> bits[32] {
  one: bits[32] = literal(value=1)
  ret result: bits[32] = add(one, one)
}
)";

  char* error = nullptr;
  char* opt_ir = nullptr;
  ASSERT_TRUE(xls_optimize_ir(kPackage.c_str(), "f", &error, &opt_ir));
  absl::Cleanup free_opt_ir([opt_ir] { xls_c_str_free(opt_ir); });

  ASSERT_NE(opt_ir, nullptr);

  const std::string kWant = R"(package p

top fn f() -> bits[32] {
  ret result: bits[32] = literal(value=2, id=5)
}
)";

  EXPECT_EQ(std::string_view(opt_ir), kWant);
}

TEST(XlsCApiTest, MangleDslxName) {
  std::string module_name = "foo_bar";
  std::string function_name = "baz_bat";

  char* error = nullptr;
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name(module_name.c_str(), function_name.c_str(),
                                   &error, &mangled));
  absl::Cleanup free_mangled([mangled] { xls_c_str_free(mangled); });

  EXPECT_EQ(std::string_view(mangled), "__foo_bar__baz_bat");
}

TEST(XlsCApiTest, MangleDslxNameFullBasic) {
  char* error = nullptr;
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name_full(
      /*module_name=*/"my_mod", /*function_name=*/"f",
      xls_calling_convention_typical,
      /*free_keys=*/nullptr, /*free_keys_count=*/0,
      /*param_env=*/nullptr,
      /*scope=*/nullptr, &error, &mangled))
      << (error ? error : "");
  absl::Cleanup free_mangled([&] { xls_c_str_free(mangled); });
  EXPECT_EQ(std::string_view(mangled), "__my_mod__f");
}

TEST(XlsCApiTest, MangleDslxNameFullScope) {
  char* error = nullptr;
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name_full(
      /*module_name=*/"my_mod", /*function_name=*/"f",
      xls_calling_convention_typical,
      /*free_keys=*/nullptr, /*free_keys_count=*/0,
      /*param_env=*/nullptr,
      /*scope=*/"Point", &error, &mangled))
      << (error ? error : "");
  absl::Cleanup free_mangled([&] { xls_c_str_free(mangled); });
  EXPECT_EQ(std::string_view(mangled), "__my_mod__Point__f");
}

TEST(XlsCApiTest, MangleDslxNameFullParametrics) {
  char* error = nullptr;
  struct xls_dslx_interp_value* x_val =
      xls_dslx_interp_value_make_ubits(/*bit_count=*/32, /*value=*/42);
  struct xls_dslx_interp_value* y_val =
      xls_dslx_interp_value_make_ubits(/*bit_count=*/32, /*value=*/64);
  absl::Cleanup free_vals([&] {
    xls_dslx_interp_value_free(x_val);
    xls_dslx_interp_value_free(y_val);
  });
  const char* free_keys[] = {"X", "Y"};
  struct xls_dslx_parametric_env_item items[] = {
      {.identifier = "X", .value = x_val},
      {.identifier = "Y", .value = y_val},
  };
  struct xls_dslx_parametric_env* env = nullptr;
  ASSERT_TRUE(
      xls_dslx_parametric_env_create(items, /*items_count=*/2, &error, &env))
      << (error ? error : "");
  absl::Cleanup free_env([&] { xls_dslx_parametric_env_free(env); });
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name_full(
      /*module_name=*/"my_mod", /*function_name=*/"p",
      xls_calling_convention_typical, free_keys,
      /*free_keys_count=*/2, env,
      /*scope=*/nullptr, &error, &mangled))
      << (error ? error : "");
  absl::Cleanup free_mangled([&] { xls_c_str_free(mangled); });
  EXPECT_EQ(std::string_view(mangled), "__my_mod__p__42_64");
}

TEST(XlsCApiTest, MangleDslxNameFullImplicitToken) {
  char* error = nullptr;
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name_full(
      /*module_name=*/"my_mod", /*function_name=*/"f",
      xls_calling_convention_implicit_token,
      /*free_keys=*/nullptr, /*free_keys_count=*/0,
      /*param_env=*/nullptr,
      /*scope=*/nullptr, &error, &mangled))
      << (error ? error : "");
  absl::Cleanup free_mangled([&] { xls_c_str_free(mangled); });
  EXPECT_EQ(std::string_view(mangled), "__itok__my_mod__f");
}

TEST(XlsCApiTest, MangleDslxNameFullProcNext) {
  char* error = nullptr;
  char* mangled = nullptr;
  ASSERT_TRUE(xls_mangle_dslx_name_full(
      /*module_name=*/"my_mod", /*function_name=*/"f",
      xls_calling_convention_proc_next,
      /*free_keys=*/nullptr, /*free_keys_count=*/0,
      /*param_env=*/nullptr,
      /*scope=*/nullptr, &error, &mangled))
      << (error ? error : "");
  absl::Cleanup free_mangled([&] { xls_c_str_free(mangled); });
  EXPECT_EQ(std::string_view(mangled), "__my_mod__f_next");
}

TEST(XlsCApiTest, ValueToStringFormatPreferences) {
  char* error = nullptr;
  struct xls_value* value = nullptr;
  ASSERT_TRUE(xls_parse_typed_value("bits[32]:0x42", &error, &value));
  absl::Cleanup free_value([value] { xls_value_free(value); });

  struct TestCase {
    std::string name;
    std::string want;
  } kTestCases[] = {
      {.name = "default", .want = "bits[32]:66"},
      {.name = "binary", .want = "bits[32]:0b100_0010"},
      {.name = "signed_decimal", .want = "bits[32]:66"},
      {.name = "unsigned_decimal", .want = "bits[32]:66"},
      {.name = "hex", .want = "bits[32]:0x42"},
      {.name = "plain_binary", .want = "bits[32]:1000010"},
      {.name = "plain_hex", .want = "bits[32]:42"},
  };

  for (const auto& [name, want] : kTestCases) {
    xls_format_preference fmt_pref;
    ASSERT_TRUE(
        xls_format_preference_from_string(name.c_str(), &error, &fmt_pref));

    char* string_out = nullptr;
    ASSERT_TRUE(xls_value_to_string_format_preference(value, fmt_pref, &error,
                                                      &string_out));
    absl::Cleanup free_string_out([string_out] { xls_c_str_free(string_out); });
    EXPECT_EQ(std::string{string_out}, want);
  }
}

TEST(XlsCApiTest, InterpretDslxFailFunction) {
  // Convert the DSLX function to IR.
  const std::string kDslxModule = R"(fn just_fail() {
  fail!("only_failure_here", ())
})";

  const char* additional_search_paths[] = {};
  char* error = nullptr;
  char* ir = nullptr;
  const std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  ASSERT_TRUE(
      xls_convert_dslx_to_ir(kDslxModule.c_str(), "my_module.x", "my_module",
                             /*dslx_stdlib_path=*/dslx_stdlib_path.c_str(),
                             additional_search_paths, 0, &error, &ir))
      << error;

  absl::Cleanup free_cstrs([&] {
    xls_c_str_free(error);
    xls_c_str_free(ir);
  });

  struct xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(ir, "p.ir", &error, &package))
      << "xls_parse_ir_package error: " << error;
  absl::Cleanup free_package([package] { xls_package_free(package); });

  // Get the function.
  struct xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "__itok__my_module__just_fail",
                                       &error, &function));

  struct xls_value* token = xls_value_make_token();
  struct xls_value* activated = xls_value_make_true();
  absl::Cleanup free_values([&] {
    xls_value_free(token);
    xls_value_free(activated);
  });

  const struct xls_value* args[] = {token, activated};
  struct xls_value* result = nullptr;
  ASSERT_FALSE(
      xls_interpret_function(function, /*argc=*/2, args, &error, &result));
  EXPECT_EQ(std::string{error},
            "ABORTED: Assertion failure via fail! @ my_module.x:2:8-2:33");
}

TEST(XlsCApiTest, DslxInspectTypeDefinitions) {
  const char kProgram[] = R"(const EIGHT = u5:8;

struct MyStruct {
    some_field: s42,
    other_field: u64,
}

enum MyEnum : u5 {
    A = u5:2,
    B = u5:4,
    C = EIGHT,
}

enum MySum {
    Nothing,
    Count(u8),
}

struct MySumHolder {
    value: MySum,
}
)";
  const char* additional_search_paths[] = {};

  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram, "foo.x", "foo", import_data,
                                         &error, &tm);
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(ok) << "got not-ok result from parse-and-typecheck; error: "
                  << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);

  char* module_name = xls_dslx_module_get_name(module);
  absl::Cleanup free_module_name([=] { xls_c_str_free(module_name); });
  EXPECT_EQ(std::string_view{module_name}, std::string_view{"foo"});

  int64_t type_definition_count =
      xls_dslx_module_get_type_definition_count(module);
  ASSERT_EQ(type_definition_count, 4);

  xls_dslx_type_definition_kind kind0 =
      xls_dslx_module_get_type_definition_kind(module, 0);
  xls_dslx_type_definition_kind kind1 =
      xls_dslx_module_get_type_definition_kind(module, 1);
  xls_dslx_type_definition_kind kind2 =
      xls_dslx_module_get_type_definition_kind(module, 2);
  xls_dslx_type_definition_kind kind3 =
      xls_dslx_module_get_type_definition_kind(module, 3);
  EXPECT_EQ(kind0, xls_dslx_type_definition_kind_struct_def);
  EXPECT_EQ(kind1, xls_dslx_type_definition_kind_enum_def);
  EXPECT_EQ(kind2, xls_dslx_type_definition_kind_sum_def);
  EXPECT_EQ(kind3, xls_dslx_type_definition_kind_struct_def);

  {
    xls_dslx_sum_def* sum_def =
        xls_dslx_module_get_type_definition_as_sum_def(module, 2);
    char* identifier = xls_dslx_sum_def_get_identifier(sum_def);
    absl::Cleanup free_identifier([=] { xls_c_str_free(identifier); });
    EXPECT_EQ(std::string_view{identifier}, std::string_view{"MySum"});

    EXPECT_FALSE(xls_dslx_sum_def_is_parametric(sum_def));
    EXPECT_EQ(xls_dslx_sum_def_get_variant_count(sum_def), 2);

    const xls_dslx_type* sum_def_type =
        xls_dslx_type_info_get_type_sum_def(type_info, sum_def);
    int64_t total_bit_count = 0;
    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(sum_def_type, &error,
                                                  &total_bit_count))
        << "got not-ok result from get-total-bit-count; error: " << error;
    ASSERT_EQ(error, nullptr);
    EXPECT_EQ(total_bit_count, 1 + 8);

    xls_dslx_module_member* sum_member =
        xls_dslx_module_member_from_sum_def(sum_def);
    ASSERT_NE(sum_member, nullptr);
    EXPECT_EQ(xls_dslx_module_member_get_kind(sum_member),
              xls_dslx_module_member_kind_sum_def);
    EXPECT_EQ(xls_dslx_module_member_get_sum_def(sum_member), sum_def);
  }

  {
    xls_dslx_struct_def* struct_def =
        xls_dslx_module_get_type_definition_as_struct_def(module, 3);
    const xls_dslx_type* struct_def_type =
        xls_dslx_type_info_get_type_struct_def(type_info, struct_def);
    int64_t total_bit_count = -1;
    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(struct_def_type, &error,
                                                  &total_bit_count))
        << "got not-ok result from get-total-bit-count; error: " << error;
    ASSERT_EQ(error, nullptr);
    EXPECT_EQ(total_bit_count, 1 + 8);
  }

  {
    xls_dslx_struct_def* struct_def =
        xls_dslx_module_get_type_definition_as_struct_def(module, 0);
    char* identifier = xls_dslx_struct_def_get_identifier(struct_def);
    absl::Cleanup free_identifier([=] { xls_c_str_free(identifier); });
    EXPECT_EQ(std::string_view{identifier}, std::string_view{"MyStruct"});

    EXPECT_FALSE(xls_dslx_struct_def_is_parametric(struct_def));
    EXPECT_EQ(xls_dslx_struct_def_get_member_count(struct_def), 2);

    // Get the concrete type that this resolves to.
    const xls_dslx_type* struct_def_type =
        xls_dslx_type_info_get_type_struct_def(type_info, struct_def);
    int64_t total_bit_count = 0;
    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(struct_def_type, &error,
                                                  &total_bit_count))
        << "got not-ok result from get-total-bit-count; error: " << error;
    ASSERT_EQ(error, nullptr);
    EXPECT_EQ(total_bit_count, 42 + 64);

    // Get the two members and see how many bits they resolve to.
    xls_dslx_struct_member* member0 =
        xls_dslx_struct_def_get_member(struct_def, 0);
    xls_dslx_struct_member* member1 =
        xls_dslx_struct_def_get_member(struct_def, 1);

    char* member0_name = xls_dslx_struct_member_get_name(member0);
    absl::Cleanup free_member0_name([=] { xls_c_str_free(member0_name); });
    ASSERT_NE(member0_name, nullptr);
    EXPECT_EQ(std::string_view{member0_name}, "some_field");

    xls_dslx_type_annotation* member0_type_annotation =
        xls_dslx_struct_member_get_type(member0);
    xls_dslx_type_annotation* member1_type_annotation =
        xls_dslx_struct_member_get_type(member1);

    const xls_dslx_type* member0_type =
        xls_dslx_type_info_get_type_type_annotation(type_info,
                                                    member0_type_annotation);
    const xls_dslx_type* member1_type =
        xls_dslx_type_info_get_type_type_annotation(type_info,
                                                    member1_type_annotation);

    bool is_signed;
    ASSERT_TRUE(xls_dslx_type_is_signed_bits(member0_type, &error, &is_signed));
    EXPECT_TRUE(is_signed);
    ASSERT_TRUE(xls_dslx_type_is_signed_bits(member1_type, &error, &is_signed));
    EXPECT_FALSE(is_signed);

    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(member0_type, &error,
                                                  &total_bit_count));
    EXPECT_EQ(total_bit_count, 42);

    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(member1_type, &error,
                                                  &total_bit_count));
    EXPECT_EQ(total_bit_count, 64);
  }

  {
    xls_dslx_enum_def* enum_def =
        xls_dslx_module_get_type_definition_as_enum_def(module, 1);
    char* enum_identifier = xls_dslx_enum_def_get_identifier(enum_def);
    absl::Cleanup free_enum_identifier(
        [=] { xls_c_str_free(enum_identifier); });
    EXPECT_EQ(std::string_view{enum_identifier}, std::string_view{"MyEnum"});

    int64_t enum_member_count = xls_dslx_enum_def_get_member_count(enum_def);
    EXPECT_EQ(enum_member_count, 3);

    xls_dslx_enum_member* member2 = xls_dslx_enum_def_get_member(enum_def, 2);
    xls_dslx_expr* member2_expr = xls_dslx_enum_member_get_value(member2);

    char* member2_name = xls_dslx_enum_member_get_name(member2);
    absl::Cleanup free_member2_name([=] { xls_c_str_free(member2_name); });
    ASSERT_NE(member2_name, nullptr);
    EXPECT_EQ(std::string_view{member2_name}, "C");

    xls_dslx_interp_value* member2_value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, member2_expr,
                                                  &error, &member2_value));
    absl::Cleanup free_member2_value(
        [=] { xls_dslx_interp_value_free(member2_value); });
    ASSERT_NE(member2_value, nullptr);

    xls_value* member2_ir_value = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_convert_to_ir(member2_value, &error,
                                                    &member2_ir_value));
    absl::Cleanup free_member2_ir_value(
        [=] { xls_value_free(member2_ir_value); });

    char* value_str = nullptr;
    ASSERT_TRUE(xls_value_to_string(member2_ir_value, &value_str));
    absl::Cleanup free_value_str([=] { xls_c_str_free(value_str); });
    EXPECT_EQ(std::string_view{value_str}, "bits[5]:8");

    const xls_dslx_type* enum_def_type =
        xls_dslx_type_info_get_type_enum_def(type_info, enum_def);
    int64_t total_bit_count = 0;
    ASSERT_TRUE(xls_dslx_type_get_total_bit_count(enum_def_type, &error,
                                                  &total_bit_count))
        << "got not-ok result from get-total-bit-count; error: " << error;
    ASSERT_EQ(error, nullptr);
    EXPECT_EQ(total_bit_count, 5);

    // Check the signedness of the underlying type.
    bool is_signed = true;
    ASSERT_TRUE(
        xls_dslx_type_is_signed_bits(enum_def_type, &error, &is_signed));
    ASSERT_EQ(error, nullptr);
    EXPECT_FALSE(is_signed);
  }
}

TEST(XlsCApiTest, DslxInspectTypeRefTypeAnnotation) {
  const char kImported[] = "pub type SomeType = u32;";
  XLS_ASSERT_OK_AND_ASSIGN(xls::TempDirectory tempdir,
                           xls::TempDirectory::Create());
  const std::filesystem::path& tempdir_path = tempdir.path();
  const std::filesystem::path module_path =
      tempdir_path / "my_imported_module.x";
  XLS_ASSERT_OK(xls::SetFileContents(module_path, kImported));

  const char kProgram[] = R"(import my_imported_module;

type MyTypeAlias = my_imported_module::SomeType;
type MyOtherTypeAlias = MyTypeAlias;
)";
  const char* additional_search_paths[] = {tempdir_path.c_str()};

  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      std::size(additional_search_paths));
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram, "foo.x", "foo", import_data,
                                         &error, &tm);
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(ok) << "got not-ok result from parse-and-typecheck; error: "
                  << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_alias* my_type_alias =
      xls_dslx_module_get_type_definition_as_type_alias(module, 0);

  // Validate that the name is "MyTypeAlias".
  {
    char* identifier = xls_dslx_type_alias_get_identifier(my_type_alias);
    absl::Cleanup free_identifier([=] { xls_c_str_free(identifier); });
    EXPECT_EQ(std::string_view{identifier}, std::string_view{"MyTypeAlias"});
  }

  // Get the type definition for the right hand side -- it should be a
  // TypeRefTypeAnnotation, which we traverse to a TypeRef where we can resolve
  // its subject as an import.
  {
    xls_dslx_type_annotation* type =
        xls_dslx_type_alias_get_type_annotation(my_type_alias);
    xls_dslx_type_ref_type_annotation* type_ref_type_annotation =
        xls_dslx_type_annotation_get_type_ref_type_annotation(type);
    xls_dslx_type_ref* type_ref =
        xls_dslx_type_ref_type_annotation_get_type_ref(
            type_ref_type_annotation);
    xls_dslx_type_definition* type_definition =
        xls_dslx_type_ref_get_type_definition(type_ref);
    xls_dslx_colon_ref* colon_ref =
        xls_dslx_type_definition_get_colon_ref(type_definition);
    xls_dslx_import* import_subject =
        xls_dslx_colon_ref_resolve_import_subject(colon_ref);
    EXPECT_NE(import_subject, nullptr);

    char* attr = xls_dslx_colon_ref_get_attr(colon_ref);
    absl::Cleanup free_attr([=] { xls_c_str_free(attr); });
    EXPECT_EQ(std::string_view{attr}, std::string_view{"SomeType"});
  }

  // Validate that we can get the type definition for `MyOtherTypeAlias`.
  {
    xls_dslx_type_alias* other_type_alias =
        xls_dslx_module_get_type_definition_as_type_alias(module, 1);
    // Check it's the alias we were expecting via its identifier.
    char* other_type_alias_identifier =
        xls_dslx_type_alias_get_identifier(other_type_alias);
    absl::Cleanup free_other_type_alias_identifier(
        [=] { xls_c_str_free(other_type_alias_identifier); });
    EXPECT_EQ(std::string_view{other_type_alias_identifier},
              std::string_view{"MyOtherTypeAlias"});

    // Get the right hand side and understand it is referencing the other type
    // alias.
    xls_dslx_type_annotation* rhs =
        xls_dslx_type_alias_get_type_annotation(other_type_alias);
    xls_dslx_type_ref_type_annotation* rhs_type_ref_type_annotation =
        xls_dslx_type_annotation_get_type_ref_type_annotation(rhs);
    xls_dslx_type_ref* rhs_type_ref =
        xls_dslx_type_ref_type_annotation_get_type_ref(
            rhs_type_ref_type_annotation);
    xls_dslx_type_definition* rhs_type_definition =
        xls_dslx_type_ref_get_type_definition(rhs_type_ref);
    xls_dslx_type_alias* rhs_type_alias =
        xls_dslx_type_definition_get_type_alias(rhs_type_definition);
    EXPECT_EQ(rhs_type_alias, my_type_alias);
  }
}

TEST(XlsCApiTest, DslxInspectArrayTypeAnnotationElement) {
  const char kImported[] = "pub struct Widget { value: u32 }";
  XLS_ASSERT_OK_AND_ASSIGN(xls::TempDirectory tempdir,
                           xls::TempDirectory::Create());
  const std::filesystem::path& tempdir_path = tempdir.path();
  const std::filesystem::path module_path =
      tempdir_path / "my_imported_module.x";
  XLS_ASSERT_OK(xls::SetFileContents(module_path, kImported));

  const char kProgram[] = R"(import my_imported_module;

type Widgets = my_imported_module::Widget[2];
)";
  const char* additional_search_paths[] = {tempdir_path.c_str()};

  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      std::size(additional_search_paths));
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  bool ok = xls_dslx_parse_and_typecheck(kProgram, "foo.x", "foo", import_data,
                                         &error, &tm);
  ASSERT_TRUE(ok) << "got not-ok result from parse-and-typecheck; error: "
                  << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_EQ(xls_dslx_module_get_type_definition_count(module), 1);
  xls_dslx_type_alias* type_alias =
      xls_dslx_module_get_type_definition_as_type_alias(module, 0);
  xls_dslx_type_annotation* type =
      xls_dslx_type_alias_get_type_annotation(type_alias);
  xls_dslx_array_type_annotation* array_type =
      xls_dslx_type_annotation_get_array_type_annotation(type);
  ASSERT_NE(array_type, nullptr);

  xls_dslx_type_annotation* element_type =
      xls_dslx_array_type_annotation_get_element_type(array_type);
  ASSERT_NE(element_type, nullptr);
  xls_dslx_type_ref_type_annotation* element_type_ref_annotation =
      xls_dslx_type_annotation_get_type_ref_type_annotation(element_type);
  ASSERT_NE(element_type_ref_annotation, nullptr);
  xls_dslx_type_ref* element_type_ref =
      xls_dslx_type_ref_type_annotation_get_type_ref(
          element_type_ref_annotation);
  xls_dslx_type_definition* element_type_definition =
      xls_dslx_type_ref_get_type_definition(element_type_ref);
  xls_dslx_colon_ref* colon_ref =
      xls_dslx_type_definition_get_colon_ref(element_type_definition);
  ASSERT_NE(colon_ref, nullptr);

  xls_dslx_import* import_subject =
      xls_dslx_colon_ref_resolve_import_subject(colon_ref);
  ASSERT_NE(import_subject, nullptr);
  EXPECT_EQ(xls_dslx_import_get_subject_count(import_subject), 1);
  char* subject = xls_dslx_import_get_subject(import_subject, 0);
  absl::Cleanup free_subject([&] { xls_c_str_free(subject); });
  EXPECT_EQ(std::string_view{subject}, "my_imported_module");

  char* attr = xls_dslx_colon_ref_get_attr(colon_ref);
  absl::Cleanup free_attr([&] { xls_c_str_free(attr); });
  EXPECT_EQ(std::string_view{attr}, "Widget");
}

TEST(XlsCApiTest, DslxInspectTypeRefParametricExprsAndStructBindings) {
  const char kProgram[] = R"(
struct Box<N: u32> {
  value: bits[N],
}

type Box8 = Box<u32:8>;
)";
  const char* additional_search_paths[] = {};

  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  bool ok = xls_dslx_parse_and_typecheck(kProgram, "foo.x", "foo", import_data,
                                         &error, &tm);
  ASSERT_TRUE(ok) << "got not-ok result from parse-and-typecheck; error: "
                  << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  ASSERT_EQ(xls_dslx_module_get_type_definition_count(module), 2);

  xls_dslx_struct_def* box_struct =
      xls_dslx_module_get_type_definition_as_struct_def(module, 0);
  ASSERT_TRUE(xls_dslx_struct_def_is_parametric(box_struct));
  ASSERT_EQ(xls_dslx_struct_def_get_parametric_binding_count(box_struct), 1);
  xls_dslx_parametric_binding* binding =
      xls_dslx_struct_def_get_parametric_binding(box_struct, 0);
  ASSERT_NE(binding, nullptr);
  char* identifier = xls_dslx_parametric_binding_get_identifier(binding);
  absl::Cleanup free_identifier([&] { xls_c_str_free(identifier); });
  EXPECT_EQ(std::string_view{identifier}, "N");

  xls_dslx_type_alias* type_alias =
      xls_dslx_module_get_type_definition_as_type_alias(module, 1);
  xls_dslx_type_annotation* rhs =
      xls_dslx_type_alias_get_type_annotation(type_alias);
  xls_dslx_type_ref_type_annotation* type_ref_type_annotation =
      xls_dslx_type_annotation_get_type_ref_type_annotation(rhs);
  ASSERT_NE(type_ref_type_annotation, nullptr);
  ASSERT_EQ(xls_dslx_type_ref_type_annotation_get_parametric_count(
                type_ref_type_annotation),
            1);
  xls_dslx_expr* parametric_expr =
      xls_dslx_type_ref_type_annotation_get_parametric_expr(
          type_ref_type_annotation, 0);
  ASSERT_NE(parametric_expr, nullptr);

  xls_dslx_interp_value* parametric_value = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, parametric_expr,
                                                &error, &parametric_value))
      << error;
  ASSERT_NE(parametric_value, nullptr);
  absl::Cleanup free_parametric_value(
      [&] { xls_dslx_interp_value_free(parametric_value); });

  char* parametric_value_str =
      xls_dslx_interp_value_to_string(parametric_value);
  absl::Cleanup free_parametric_value_str(
      [&] { xls_c_str_free(parametric_value_str); });
  EXPECT_EQ(std::string_view{parametric_value_str}, "u32:8");

  const xls_dslx_type* box8_type =
      xls_dslx_type_info_get_type_type_annotation(type_info, rhs);
  ASSERT_NE(box8_type, nullptr);
  ASSERT_TRUE(xls_dslx_type_is_struct(box8_type));
  ASSERT_EQ(xls_dslx_type_struct_get_member_count(box8_type), 1);

  const xls_dslx_type* value_type =
      xls_dslx_type_struct_get_member_type(box8_type, 0);
  ASSERT_NE(value_type, nullptr);
  xls_dslx_type_dim* is_signed = nullptr;
  xls_dslx_type_dim* size = nullptr;
  ASSERT_TRUE(xls_dslx_type_is_bits_like(const_cast<xls_dslx_type*>(value_type),
                                         &is_signed, &size));
  absl::Cleanup free_type_dims([&] {
    xls_dslx_type_dim_free(is_signed);
    xls_dslx_type_dim_free(size);
  });
  bool is_signed_value = true;
  ASSERT_TRUE(
      xls_dslx_type_dim_get_as_bool(is_signed, &error, &is_signed_value));
  EXPECT_FALSE(is_signed_value);
  int64_t value_bit_count = 0;
  ASSERT_TRUE(xls_dslx_type_dim_get_as_int64(size, &error, &value_bit_count));
  EXPECT_EQ(value_bit_count, 8);
}

TEST(XlsCApiTest, DslxInspectImportModuleMember) {
  const char kImported[] = "pub const VALUE = u32:7;";
  XLS_ASSERT_OK_AND_ASSIGN(xls::TempDirectory tempdir,
                           xls::TempDirectory::Create());
  const std::filesystem::path& tempdir_path = tempdir.path();
  const std::filesystem::path module_path =
      tempdir_path / "my_imported_module.x";
  XLS_ASSERT_OK(xls::SetFileContents(module_path, kImported));

  const char kProgram[] = "import my_imported_module as mim;";
  const char* additional_search_paths[] = {tempdir_path.c_str()};

  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      std::size(additional_search_paths));
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  bool ok = xls_dslx_parse_and_typecheck(kProgram, "foo.x", "foo", import_data,
                                         &error, &tm);
  ASSERT_TRUE(ok) << "got not-ok result from parse-and-typecheck; error: "
                  << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_EQ(xls_dslx_module_get_member_count(module), 1);
  xls_dslx_module_member* member = xls_dslx_module_get_member(module, 0);
  EXPECT_EQ(xls_dslx_module_member_get_kind(member),
            xls_dslx_module_member_kind_import);
  xls_dslx_import* import = xls_dslx_module_member_get_import(member);
  ASSERT_NE(import, nullptr);
  EXPECT_EQ(xls_dslx_import_get_subject_count(import), 1);
  char* subject = xls_dslx_import_get_subject(import, 0);
  absl::Cleanup free_subject([&] { xls_c_str_free(subject); });
  EXPECT_EQ(std::string_view{subject}, "my_imported_module");
}

TEST(XlsCApiTest, DslxModuleMembers) {
  const std::string_view kProgram = R"(
    struct MyStruct {}
    enum MyEnum: u32 { A = u32:0 }
    type MyTypeAlias = ();
    const MY_CONSTANT: u32 = u32:42;
  )";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.data(), "<test>", "top",
                                         import_data, &error, &tm);
  ASSERT_TRUE(ok) << "error: " << error;
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);

  int64_t member_count = xls_dslx_module_get_member_count(module);
  EXPECT_EQ(member_count, 4);

  // module member 0: `MyStruct`
  {
    xls_dslx_module_member* struct_def_member =
        xls_dslx_module_get_member(module, 0);
    xls_dslx_struct_def* struct_def =
        xls_dslx_module_member_get_struct_def(struct_def_member);
    xls_dslx_module_member* struct_def_via_from =
        xls_dslx_module_member_from_struct_def(struct_def);
    EXPECT_EQ(struct_def_via_from, struct_def_member);
    char* struct_def_identifier =
        xls_dslx_struct_def_get_identifier(struct_def);
    absl::Cleanup free_struct_def_identifier(
        [&] { xls_c_str_free(struct_def_identifier); });
    EXPECT_EQ(std::string_view{struct_def_identifier}, "MyStruct");
  }

  // module member 1: `MyEnum`
  {
    xls_dslx_module_member* enum_def_member =
        xls_dslx_module_get_member(module, 1);
    xls_dslx_enum_def* enum_def =
        xls_dslx_module_member_get_enum_def(enum_def_member);
    xls_dslx_module_member* enum_def_via_from =
        xls_dslx_module_member_from_enum_def(enum_def);
    EXPECT_EQ(enum_def_via_from, enum_def_member);
    char* enum_def_identifier = xls_dslx_enum_def_get_identifier(enum_def);
    absl::Cleanup free_enum_def_identifier(
        [&] { xls_c_str_free(enum_def_identifier); });
    EXPECT_EQ(std::string_view{enum_def_identifier}, "MyEnum");
  }

  // module member 2: `MyTypeAlias`
  {
    xls_dslx_module_member* type_alias_member =
        xls_dslx_module_get_member(module, 2);
    xls_dslx_type_alias* type_alias =
        xls_dslx_module_member_get_type_alias(type_alias_member);
    xls_dslx_module_member* type_alias_via_from =
        xls_dslx_module_member_from_type_alias(type_alias);
    EXPECT_EQ(type_alias_via_from, type_alias_member);
    char* type_alias_identifier =
        xls_dslx_type_alias_get_identifier(type_alias);
    absl::Cleanup free_type_alias_identifier(
        [&] { xls_c_str_free(type_alias_identifier); });
    EXPECT_EQ(std::string_view{type_alias_identifier}, "MyTypeAlias");
  }

  // module member 3: `MY_CONSTANT`
  {
    xls_dslx_module_member* constant_def_member =
        xls_dslx_module_get_member(module, 3);
    xls_dslx_constant_def* constant_def =
        xls_dslx_module_member_get_constant_def(constant_def_member);
    xls_dslx_module_member* constant_def_via_from =
        xls_dslx_module_member_from_constant_def(constant_def);
    EXPECT_EQ(constant_def_via_from, constant_def_member);
    char* constant_def_name = xls_dslx_constant_def_get_name(constant_def);
    absl::Cleanup free_constant_def_name(
        [&] { xls_c_str_free(constant_def_name); });
    EXPECT_EQ(std::string_view{constant_def_name}, "MY_CONSTANT");

    xls_dslx_expr* interp_value = xls_dslx_constant_def_get_value(constant_def);
    // Get the constexpr value via the type information.
    char* error = nullptr;
    xls_dslx_interp_value* result = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, interp_value,
                                                  &error, &result));
    absl::Cleanup free_result([&] { xls_dslx_interp_value_free(result); });

    // Spot check the interpreter value we got from constexpr evaluation.
    char* interp_value_str = xls_dslx_interp_value_to_string(result);
    absl::Cleanup free_interp_value_str(
        [&] { xls_c_str_free(interp_value_str); });
    EXPECT_EQ(std::string_view{interp_value_str}, "u32:42");
  }
}

TEST(XlsCApiTest, DslxSemanticSumConstExprUsesConstructorFormatting) {
  constexpr const char* kProgram = R"(
enum Inner {
  None,
  Some(u8),
}

enum Outer {
  Wrap(Inner),
  Record { value: Inner },
}

const DIRECT: Inner = Inner::Some(u8:42);
const NESTED: Outer = Outer::Wrap(Inner::Some(u8:7));
const RECORD: Outer = Outer::Record { value: Inner::Some(u8:9) };
const TUPLE: (Inner, u8) = (Inner::Some(u8:3), u8:4);
const ARRAY: Inner[2] = [Inner::None, Inner::Some(u8:5)];
)";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "sum.x", "sum",
                                           import_data, &error, &module_owner))
      << (error == nullptr ? "unknown error" : error);
  absl::Cleanup free_module([&] {
    if (module_owner != nullptr) {
      xls_dslx_typechecked_module_free(module_owner);
    }
  });

  xls_dslx_module* module =
      xls_dslx_typechecked_module_get_module(module_owner);
  xls_dslx_type_info* type_info =
      xls_dslx_typechecked_module_get_type_info(module_owner);
  xls_dslx_interp_value* retained_clone = nullptr;
  absl::Cleanup free_clone([&] {
    if (retained_clone != nullptr) {
      xls_dslx_interp_value_free(retained_clone);
    }
  });

  const std::vector<std::pair<int64_t, std::string_view>> expected = {
      {2, "Inner::Some(u8:42)"},
      {3, "Outer::Wrap(Inner::Some(u8:7))"},
      {4, "Outer::Record {\n    value: Inner::Some(u8:9)\n}"},
      {5, "(\n    Inner::Some(u8:3),\n    u8:4\n)"},
      {6, "[\n    Inner::None,\n    Inner::Some(u8:5)\n]"},
  };
  for (const auto& [member_index, expected_text] : expected) {
    SCOPED_TRACE(member_index);
    xls_dslx_module_member* member =
        xls_dslx_module_get_member(module, member_index);
    xls_dslx_constant_def* constant =
        xls_dslx_module_member_get_constant_def(member);
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
        << (error == nullptr ? "unknown error" : error);
    absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });

    char* formatted = xls_dslx_interp_value_to_string(value);
    ASSERT_NE(formatted, nullptr);
    absl::Cleanup free_formatted([&] { xls_c_str_free(formatted); });
    EXPECT_EQ(std::string_view(formatted), expected_text);
    if (member_index == 2) {
      retained_clone = xls_dslx_interp_value_clone(value);
      ASSERT_NE(retained_clone, nullptr);
    }
  }

  xls_dslx_typechecked_module_free(module_owner);
  module_owner = nullptr;
  char* clone_text = xls_dslx_interp_value_to_string(retained_clone);
  ASSERT_NE(clone_text, nullptr);
  absl::Cleanup free_clone_text([&] { xls_c_str_free(clone_text); });
  EXPECT_STREQ(clone_text, "Inner::Some(u8:42)");

  xls_dslx_interp_value* scalar =
      xls_dslx_interp_value_make_ubits(/*bit_count=*/8, /*value=*/4);
  ASSERT_NE(scalar, nullptr);
  absl::Cleanup free_scalar([&] { xls_dslx_interp_value_free(scalar); });

  xls_dslx_interp_value* tuple_elements[] = {scalar, retained_clone};
  xls_dslx_interp_value* tuple = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_tuple(/*element_count=*/2,
                                               tuple_elements, &error, &tuple));
  absl::Cleanup free_tuple([&] { xls_dslx_interp_value_free(tuple); });
  char* tuple_text = xls_dslx_interp_value_to_string(tuple);
  absl::Cleanup free_tuple_text([&] { xls_c_str_free(tuple_text); });
  EXPECT_STREQ(tuple_text, "(\n    u8:4,\n    Inner::Some(u8:42)\n)");

  xls_dslx_interp_value* array_elements[] = {retained_clone, retained_clone};
  xls_dslx_interp_value* array = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_array(/*element_count=*/2,
                                               array_elements, &error, &array));
  absl::Cleanup free_array([&] { xls_dslx_interp_value_free(array); });
  char* array_text = xls_dslx_interp_value_to_string(array);
  absl::Cleanup free_array_text([&] { xls_c_str_free(array_text); });
  EXPECT_STREQ(array_text,
               "[\n    Inner::Some(u8:42),\n    Inner::Some(u8:42)\n]");
}

TEST(XlsCApiTest, DslxSemanticSumBindingViewsRetainConstructorFormatting) {
  constexpr const char* kProgram = R"(
enum Maybe {
  None,
  Some(u8),
}

const VALUE: Maybe = Maybe::Some(u8:42);

enum Other {
  None,
  Some(u8),
}

const OTHER: Other = Other::Some(u8:7);
)";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "sum.x", "sum",
                                           import_data, &error, &module_owner))
      << (error == nullptr ? "unknown error" : error);
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  xls_dslx_module* module =
      xls_dslx_typechecked_module_get_module(module_owner);
  xls_dslx_type_info* type_info =
      xls_dslx_typechecked_module_get_type_info(module_owner);
  xls_dslx_constant_def* constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(constant, nullptr);

  xls_dslx_interp_value* owned_value = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(constant), &error,
      &owned_value));
  absl::Cleanup free_value([&] { xls_dslx_interp_value_free(owned_value); });

  xls_dslx_parametric_env_item items[] = {{"VALUE", owned_value}};
  xls_dslx_parametric_env* env = nullptr;
  ASSERT_TRUE(xls_dslx_parametric_env_create(items, 1, &error, &env));
  absl::Cleanup free_env([&] {
    if (env != nullptr) {
      xls_dslx_parametric_env_free(env);
    }
  });

  char* env_text = xls_dslx_parametric_env_to_string(env);
  absl::Cleanup free_env_text([&] { xls_c_str_free(env_text); });
  EXPECT_STREQ(env_text, "{VALUE: Maybe::Some(u8:42)}");

  xls_dslx_interp_value* borrowed =
      xls_dslx_parametric_env_get_binding_value(env, 0);
  ASSERT_NE(borrowed, nullptr);
  char* borrowed_text = xls_dslx_interp_value_to_string(borrowed);
  absl::Cleanup free_borrowed_text([&] { xls_c_str_free(borrowed_text); });
  EXPECT_STREQ(borrowed_text, "Maybe::Some(u8:42)");

  xls_dslx_parametric_env* cloned_env = xls_dslx_parametric_env_clone(env);
  ASSERT_NE(cloned_env, nullptr);
  absl::Cleanup free_cloned_env(
      [&] { xls_dslx_parametric_env_free(cloned_env); });
  char* cloned_env_text = xls_dslx_parametric_env_to_string(cloned_env);
  absl::Cleanup free_cloned_env_text([&] { xls_c_str_free(cloned_env_text); });
  EXPECT_STREQ(cloned_env_text, "{VALUE: Maybe::Some(u8:42)}");
  xls_dslx_interp_value* cloned_binding =
      xls_dslx_parametric_env_get_binding_value(cloned_env, 0);
  char* cloned_binding_text = xls_dslx_interp_value_to_string(cloned_binding);
  absl::Cleanup free_cloned_binding_text(
      [&] { xls_c_str_free(cloned_binding_text); });
  EXPECT_STREQ(cloned_binding_text, "Maybe::Some(u8:42)");

  xls_dslx_interp_value* retained_clone = xls_dslx_interp_value_clone(borrowed);
  ASSERT_NE(retained_clone, nullptr);
  absl::Cleanup free_retained_clone(
      [&] { xls_dslx_interp_value_free(retained_clone); });
  xls_dslx_parametric_env_free(env);
  env = nullptr;
  char* retained_text = xls_dslx_interp_value_to_string(retained_clone);
  absl::Cleanup free_retained_text([&] { xls_c_str_free(retained_text); });
  EXPECT_STREQ(retained_text, "Maybe::Some(u8:42)");
  char* surviving_env_text = xls_dslx_parametric_env_to_string(cloned_env);
  absl::Cleanup free_surviving_env_text(
      [&] { xls_c_str_free(surviving_env_text); });
  EXPECT_STREQ(surviving_env_text, "{VALUE: Maybe::Some(u8:42)}");
  char* surviving_binding_text =
      xls_dslx_interp_value_to_string(cloned_binding);
  absl::Cleanup free_surviving_binding_text(
      [&] { xls_c_str_free(surviving_binding_text); });
  EXPECT_STREQ(surviving_binding_text, "Maybe::Some(u8:42)");

  xls_dslx_constant_def* other_constant =
      xls_dslx_module_member_get_constant_def(
          xls_dslx_module_get_member(module, 3));
  ASSERT_NE(other_constant, nullptr);
  xls_dslx_interp_value* other_value = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(other_constant), &error,
      &other_value));
  absl::Cleanup free_other_value(
      [&] { xls_dslx_interp_value_free(other_value); });

  xls_dslx_parametric_env_item duplicate_items[] = {{"DUPLICATE", owned_value},
                                                    {"DUPLICATE", other_value}};
  xls_dslx_parametric_env* duplicate_env = nullptr;
  ASSERT_TRUE(xls_dslx_parametric_env_create(duplicate_items, 2, &error,
                                             &duplicate_env));
  absl::Cleanup free_duplicate_env(
      [&] { xls_dslx_parametric_env_free(duplicate_env); });

  auto binding_texts = [](const xls_dslx_parametric_env* source) {
    std::vector<std::string> result;
    for (int64_t i = 0; i < xls_dslx_parametric_env_get_binding_count(source);
         ++i) {
      char* text = xls_dslx_interp_value_to_string(
          xls_dslx_parametric_env_get_binding_value(source, i));
      result.emplace_back(text);
      xls_c_str_free(text);
    }
    return result;
  };
  EXPECT_THAT(binding_texts(duplicate_env),
              ::testing::UnorderedElementsAre("Maybe::Some(u8:42)",
                                              "Other::Some(u8:7)"));

  xls_dslx_parametric_env* duplicate_clone =
      xls_dslx_parametric_env_clone(duplicate_env);
  ASSERT_NE(duplicate_clone, nullptr);
  absl::Cleanup free_duplicate_clone(
      [&] { xls_dslx_parametric_env_free(duplicate_clone); });
  EXPECT_THAT(binding_texts(duplicate_clone),
              ::testing::UnorderedElementsAre("Maybe::Some(u8:42)",
                                              "Other::Some(u8:7)"));
}

TEST(XlsCApiTest, DslxMetadataLookupDoesNotScanDistinctSumTypes) {
  // Preserve parsed types and their clone/equality behavior while observing
  // comparisons at the real C retrieval boundary, without production counters.
  class CountingSumType : public xls::dslx::SumType {
   public:
    CountingSumType(const SumType& type, int64_t& comparisons)
        : SumType(type), comparisons_(comparisons) {}

    bool operator==(const xls::dslx::Type& other) const override {
      ++comparisons_;
      return SumType::operator==(other);
    }

    std::unique_ptr<xls::dslx::Type> CloneToUnique() const override {
      return std::make_unique<CountingSumType>(*this);
    }

   private:
    int64_t& comparisons_;
  };

  constexpr int64_t kCount = 64;
  std::string program = R"(
#![feature(generics)]
enum Phantom<N: u32, T: type> { Only() }
)";
  for (int64_t i = 0; i < kCount; ++i) {
    absl::StrAppendFormat(&program,
                          "const V%d: Phantom<u32:%d, u8> = "
                          "Phantom<u32:%d, u8>::Only();\n",
                          i, i, i);
  }
  int64_t comparisons = 0;
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(program.c_str(), "metadata_lookup.x",
                                           "metadata_lookup", owner, &error,
                                           &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  std::vector<xls_dslx_expr*> expressions;
  for (int64_t i = 0; i < kCount; ++i) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, i + 1));
    ASSERT_NE(constant, nullptr);
    auto* expr = xls_dslx_constant_def_get_value(constant);
    expressions.push_back(expr);
    auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
    auto type = cpp_type_info->GetItem(cpp_expr);
    ASSERT_TRUE(type.has_value());
    auto* sum = dynamic_cast<xls::dslx::SumType*>(*type);
    ASSERT_NE(sum, nullptr);
    cpp_type_info->SetItem(
        cpp_expr, std::make_unique<CountingSumType>(*sum, comparisons));
  }

  std::vector<std::weak_ptr<const void>> metadata;
  for (int pass = 0; pass < 2; ++pass) {
    comparisons = 0;
    for (int64_t i = 0; i < kCount; ++i) {
      xls_dslx_interp_value* value = nullptr;
      ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expressions[i],
                                                    &error, &value))
          << error;
      absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
      char* text = xls_dslx_interp_value_to_string(value);
      absl::Cleanup free_text([&] { xls_c_str_free(text); });
      EXPECT_STREQ(text, "Phantom::Only()");
      auto current = xls::GetDslxValueMetadataForTesting(value);
      ASSERT_FALSE(current.expired());
      if (pass == 0) {
        metadata.push_back(current);
      } else {
        EXPECT_EQ(current.lock(), metadata[i].lock());
      }
    }
    RecordProperty(pass == 0 ? "cold_comparisons" : "warm_comparisons",
                   std::to_string(comparisons));
    EXPECT_GT(comparisons, 0);
    EXPECT_LE(comparisons, 4 * kCount);
  }
}

TEST(XlsCApiTest, DslxNestedSumDescriptionsAreSharedAcrossRetrievedRoots) {
  // Retrieving one enclosing root is not enough: the old construction-local
  // memo shares within a root but rebuilds its children for each later root.
  constexpr int64_t kCount = 64;
  std::string program;
  for (int64_t i = 0; i < kCount; ++i) {
    absl::StrAppendFormat(&program, "enum L%d { Wrap(%s), Other }\n", i,
                          i == 0 ? "u8" : absl::StrFormat("L%d", i - 1));
    absl::StrAppendFormat(&program, "const V%d: L%d = L%d::Wrap(%s);\n", i, i,
                          i, i == 0 ? "u8:7" : absl::StrFormat("V%d", i - 1));
  }
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      program.c_str(), "nested_descriptors.x", "nested_descriptors", owner,
      &error, &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);

  std::vector<xls_dslx_interp_value*> values;
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  std::unordered_set<const void*> descriptions;
  std::vector<const xls::dslx::ValueFormatDescriptor*> roots;
  std::string expected = "L0::Wrap(u8:7)";
  for (int64_t i = 0; i < kCount; ++i) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, 2 * i + 1));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
        << error;
    values.push_back(value);
    const auto* root = xls::GetDslxValueFormatDescriptorForTesting(value);
    ASSERT_NE(root, nullptr);
    roots.push_back(root);

    char* text = xls_dslx_interp_value_to_string(value);
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, expected.c_str());
    xls_c_str_free(text);
    expected = absl::StrFormat("L%d::Wrap(%s)", i + 1, expected);

    const auto* current = root;
    for (int64_t level = i; level >= 0; --level) {
      ASSERT_TRUE(current->IsSum());
      EXPECT_EQ(current->sum_name(), absl::StrFormat("L%d", level));
      descriptions.insert(current->sum_format_identity());
      const auto payload = current->sum_variant(0).payload_formats();
      ASSERT_EQ(payload.size(), 1);
      current = &payload.front();
    }
    EXPECT_TRUE(current->IsLeafValue());
  }
  RecordProperty("distinct_sum_descriptions",
                 std::to_string(descriptions.size()));
  EXPECT_EQ(descriptions.size(), kCount);
  // The smallest root and the same inner type under the deepest root must
  // share the immutable description, not merely produce equal strings.
  auto* inner = roots.back();
  for (int64_t i = kCount - 1; i > 0; --i) {
    inner = &inner->sum_variant(0).payload_formats().front();
  }
  EXPECT_EQ(inner->sum_format_identity(), roots.front()->sum_format_identity());
}

TEST(XlsCApiTest, DslxNestedSumDescriptionsHarvestFromFreedStructRoot) {
  constexpr const char* kProgram = R"(
enum Inner { Leaf(u8), Other }
enum Outer {
  Unit,
  Wrap(Inner),
  Pair((Inner[1], u8)),
  Record { item: Inner },
}
struct Box { value: (Outer[1], u8) }
const INNER: Inner = Inner::Leaf(u8:7);
const OUTER: Outer = Outer::Wrap(INNER);
const BOX: Box = Box { value: (Outer[1]:[OUTER], u8:0) };
const BOX_AGAIN: Box = Box { value: (Outer[1]:[OUTER], u8:0) };
)";
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "nested_struct_descriptors.x", "nested_struct_descriptors",
      owner, &error, &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);

  std::vector<xls_dslx_interp_value*> values;
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  auto* box_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 5));
  ASSERT_NE(box_constant, nullptr);
  auto* box_expr = xls_dslx_constant_def_get_value(box_constant);
  xls_dslx_interp_value* first_box = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, box_expr, &error,
                                                &first_box))
      << error;
  values.push_back(first_box);
  const auto first_metadata = xls::GetDslxValueMetadataForTesting(first_box);
  const auto* box = xls::GetDslxValueFormatDescriptorForTesting(values[0]);
  ASSERT_NE(box, nullptr);
  ASSERT_TRUE(box->IsStruct());
  ASSERT_EQ(box->struct_elements().size(), 1);
  const auto& box_tuple = box->struct_elements().front();
  ASSERT_TRUE(box_tuple.IsTuple());
  ASSERT_EQ(box_tuple.tuple_elements().size(), 2);
  const auto& box_array = box_tuple.tuple_elements().front();
  ASSERT_TRUE(box_array.IsArray());
  const auto& nested_outer = box_array.array_element_format();
  ASSERT_TRUE(nested_outer.IsSum());
  ASSERT_EQ(nested_outer.sum_variant_count(), 4);
  EXPECT_TRUE(nested_outer.sum_variant(0).payload_formats().empty());
  const auto wrap_payload = nested_outer.sum_variant(1).payload_formats();
  ASSERT_EQ(wrap_payload.size(), 1);
  const auto& nested_inner = wrap_payload.front();
  ASSERT_TRUE(nested_inner.IsSum());
  const auto pair_payload = nested_outer.sum_variant(2).payload_formats();
  ASSERT_EQ(pair_payload.size(), 1);
  ASSERT_TRUE(pair_payload.front().IsTuple());
  const auto& pair_array = pair_payload.front().tuple_elements().front();
  ASSERT_TRUE(pair_array.IsArray());
  const auto& pair_inner = pair_array.array_element_format();
  const auto record_payload = nested_outer.sum_variant(3).payload_formats();
  ASSERT_EQ(record_payload.size(), 1);
  const auto& record_inner = record_payload.front();
  ASSERT_TRUE(pair_inner.IsSum());
  ASSERT_TRUE(record_inner.IsSum());
  const void* outer_identity = nested_outer.sum_format_identity();
  const void* inner_identity = nested_inner.sum_format_identity();
  EXPECT_EQ(pair_inner.sum_format_identity(), inner_identity);
  EXPECT_EQ(record_inner.sum_format_identity(), inner_identity);

  // The first root can be freed without losing the completed descriptor graph.
  // A second uncached aggregate request promotes it even for the same Type.
  xls_dslx_interp_value_free(values[0]);
  values[0] = nullptr;
  EXPECT_FALSE(first_metadata.expired());
  auto* repeat_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 6));
  ASSERT_NE(repeat_constant, nullptr);
  xls_dslx_interp_value* repeat_box = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(repeat_constant), &error,
      &repeat_box))
      << error;
  values.push_back(repeat_box);
  const auto* repeat = xls::GetDslxValueFormatDescriptorForTesting(repeat_box);
  ASSERT_NE(repeat, nullptr);
  const auto& repeat_outer = repeat->struct_elements()
                                 .front()
                                 .tuple_elements()
                                 .front()
                                 .array_element_format();
  ASSERT_TRUE(repeat_outer.IsSum());
  EXPECT_EQ(repeat_outer.sum_format_identity(), outer_identity);
  EXPECT_TRUE(first_metadata.expired());
  xls_dslx_interp_value_free(values[1]);
  values[1] = nullptr;

  // Later nominal requests reuse descendants from inactive tuple and struct
  // payloads after both aggregate handles are freed.
  for (int member_index : {4, 3}) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, member_index));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
        << error;
    values.push_back(value);
  }
  const auto* outer = xls::GetDslxValueFormatDescriptorForTesting(values[2]);
  const auto* inner = xls::GetDslxValueFormatDescriptorForTesting(values[3]);
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(outer->IsSum());
  ASSERT_TRUE(inner->IsSum());
  EXPECT_EQ(outer_identity, outer->sum_format_identity());
  EXPECT_EQ(inner_identity, inner->sum_format_identity());

  // A matching nominal declaration and argument hash are not sufficient to
  // select the harvested description when the concrete tag width differs.
  auto* inner_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 3));
  ASSERT_NE(inner_constant, nullptr);
  auto* inner_expr = xls_dslx_constant_def_get_value(inner_constant);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto* cpp_inner_expr = reinterpret_cast<xls::dslx::Expr*>(inner_expr);
  auto original_type = cpp_type_info->GetItem(cpp_inner_expr);
  ASSERT_TRUE(original_type.has_value());
  auto* original_sum = dynamic_cast<const xls::dslx::SumType*>(*original_type);
  ASSERT_NE(original_sum, nullptr);
  const xls::dslx::SumType original(*original_sum);
  std::vector<xls::dslx::SumTypeVariant> variants;
  for (const auto& variant : original.variants()) {
    variants.push_back(variant.Clone());
  }
  auto different = std::make_unique<xls::dslx::SumType>(
      original.nominal_type(), std::move(variants),
      xls::dslx::TypeDim::CreateU32(2));
  ASSERT_EQ(original.parametric_arguments_hash(),
            different->parametric_arguments_hash());
  ASSERT_FALSE(original == *different);
  cpp_type_info->SetItem(cpp_inner_expr, std::move(different));
  xls_dslx_interp_value* collision_value = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, inner_expr, &error,
                                                &collision_value))
      << error;
  values.push_back(collision_value);
  const auto* collision =
      xls::GetDslxValueFormatDescriptorForTesting(collision_value);
  ASSERT_NE(collision, nullptr);
  EXPECT_NE(collision->sum_format_identity(), inner_identity);
  EXPECT_EQ(collision->sum_tag_bit_count(), 2);
  cpp_type_info->SetItem(cpp_inner_expr, original.CloneToUnique());
  xls_dslx_interp_value* restored_value = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, inner_expr, &error,
                                                &restored_value))
      << error;
  values.push_back(restored_value);
  const auto* restored =
      xls::GetDslxValueFormatDescriptorForTesting(restored_value);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->sum_format_identity(), inner_identity);

  auto* clone = xls_dslx_interp_value_clone(values[2]);
  ASSERT_NE(clone, nullptr);
  values.push_back(clone);
  xls_dslx_typechecked_module_free(module_owner);
  module_owner = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  char* text = xls_dslx_interp_value_to_string(clone);
  ASSERT_NE(text, nullptr);
  EXPECT_THAT(text, HasSubstr("Outer::Wrap(Inner::Leaf(u8:7))"));
  xls_c_str_free(text);
}

// Verifies: cold/warm lookup scales for distinct and generic structs.
// Catches: nominal scans in ordinary and sum-bearing struct lookup.
TEST(XlsCApiTest, DslxMetadataLookupDoesNotScanStructTypes) {
  class CountingStructType : public xls::dslx::StructType {
   public:
    CountingStructType(const StructType& type, int64_t& comparisons)
        : StructType(CloneSpan(type.members()), type.nominal_type(),
                     type.nominal_type_dims_by_identifier()),
          comparisons_(comparisons) {}

    bool operator==(const xls::dslx::Type& other) const override {
      ++comparisons_;
      return StructType::operator==(other);
    }

    std::unique_ptr<xls::dslx::Type> CloneToUnique() const override {
      return std::make_unique<CountingStructType>(*this, comparisons_);
    }

   private:
    int64_t& comparisons_;
  };

  // A declaration-only index fixes the first case but still scans the second.
  struct StructCase {
    const char* name;
    bool generic;
    bool contains_sum;
  };
  struct LookupComparisons {
    int64_t cold = 0;
    int64_t warm = 0;
  };
  auto measure = [](const StructCase& test_case, int64_t count,
                    LookupComparisons& result) {
    SCOPED_TRACE(absl::StrFormat("count=%d", count));
    const bool generic = test_case.generic;
    const bool contains_sum = test_case.contains_sum;
    std::string program;
    if (generic && contains_sum) {
      program = R"(
#![feature(generics)]
enum Phantom<N: u32> { Only() }
struct Wrapper<T: type> { value: T }
)";
    } else if (generic) {
      program = "struct Wrapper<N: u32> { value: uN[N] }\n";
    } else if (contains_sum) {
      program = "enum S { Only() }\n";
    }
    for (int64_t i = 0; i < count; ++i) {
      if (generic && contains_sum) {
        absl::StrAppendFormat(&program,
                              "const V%d = Wrapper<Phantom<u32:%d>> { "
                              "value: Phantom<u32:%d>::Only() };\n",
                              i, i, i);
      } else if (generic) {
        absl::StrAppendFormat(&program,
                              "const V%d = Wrapper<u32:%d> { "
                              "value: uN[%d]:0 };\n",
                              i, i + 1, i + 1);
      } else if (contains_sum) {
        absl::StrAppendFormat(&program,
                              "struct W%d { value: S }\n"
                              "const V%d = W%d { value: S::Only() };\n",
                              i, i, i);
      } else {
        absl::StrAppendFormat(&program,
                              "struct W%d { value: u8 }\n"
                              "const V%d = W%d { value: u8:0 };\n",
                              i, i, i);
      }
    }
    int64_t comparisons = 0;
    xls_dslx_import_data* owner = xls_dslx_import_data_create(
        std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
    ASSERT_NE(owner, nullptr);
    absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
    char* error = nullptr;
    absl::Cleanup free_error([&] { xls_c_str_free(error); });
    xls_dslx_typechecked_module* module_owner = nullptr;
    ASSERT_TRUE(xls_dslx_parse_and_typecheck(
        program.c_str(), "struct_metadata.x", "struct_metadata", owner, &error,
        &module_owner))
        << error;
    absl::Cleanup free_module(
        [&] { xls_dslx_typechecked_module_free(module_owner); });
    auto* module = xls_dslx_typechecked_module_get_module(module_owner);
    auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
    auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
    std::vector<xls_dslx_expr*> expressions;
    for (int64_t i = 0; i < count; ++i) {
      const int64_t index =
          (generic ? i + 1 : 2 * i + 1) + (contains_sum ? 1 : 0);
      auto* constant = xls_dslx_module_member_get_constant_def(
          xls_dslx_module_get_member(module, index));
      ASSERT_NE(constant, nullptr);
      auto* expr = xls_dslx_constant_def_get_value(constant);
      expressions.push_back(expr);
      auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
      auto type = cpp_type_info->GetItem(cpp_expr);
      ASSERT_TRUE(type.has_value());
      auto* structure = dynamic_cast<xls::dslx::StructType*>(*type);
      ASSERT_NE(structure, nullptr);
      cpp_type_info->SetItem(cpp_expr, std::make_unique<CountingStructType>(
                                           *structure, comparisons));
    }
    for (int pass = 0; pass < 2; ++pass) {
      SCOPED_TRACE(pass == 0 ? "cold" : "warm");
      comparisons = 0;
      for (int64_t i = 0; i < count; ++i) {
        xls_dslx_interp_value* value = nullptr;
        ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expressions[i],
                                                      &error, &value))
            << error;
        absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
        char* text = xls_dslx_interp_value_to_string(value);
        absl::Cleanup free_text([&] { xls_c_str_free(text); });
        if (contains_sum) {
          EXPECT_THAT(text,
                      HasSubstr(generic ? "Phantom::Only()" : "S::Only()"));
        } else {
          EXPECT_EQ(std::string_view(text),
                    absl::StrFormat("(u%d:0)", generic ? i + 1 : 8));
        }
      }
      RecordProperty(absl::StrFormat("%s_%s_%d_comparisons", test_case.name,
                                     pass == 0 ? "cold" : "warm", count),
                     std::to_string(comparisons));
      if (pass == 0) {
        result.cold = comparisons;
      } else {
        result.warm = comparisons;
      }
    }
  };

  constexpr int64_t kSmallCount = 32;
  constexpr int64_t kLargeCount = 128;
  for (const StructCase& test_case :
       {StructCase{"ordinary_distinct", false, false},
        {"ordinary_generic", true, false},
        {"sum_distinct", false, true},
        {"sum_generic", true, true}}) {
    SCOPED_TRACE(test_case.name);
    LookupComparisons small;
    LookupComparisons large;
    ASSERT_NO_FATAL_FAILURE(measure(test_case, kSmallCount, small));
    ASSERT_NO_FATAL_FAILURE(measure(test_case, kLargeCount, large));

    // Four times the input allows twice the linear growth plus room for hash
    // collisions or resizing. A scan of all previous types grows about 16-fold.
    {
      SCOPED_TRACE("cold");
      EXPECT_LE(large.cold, 8 * small.cold + kLargeCount)
          << "32 types: " << small.cold << "; 128 types: " << large.cold;
    }
    {
      SCOPED_TRACE("warm");
      EXPECT_GE(small.warm, kSmallCount);
      EXPECT_GE(large.warm, kLargeCount);
      EXPECT_LE(large.warm, 8 * small.warm + kLargeCount)
          << "32 types: " << small.warm << "; 128 types: " << large.warm;
    }
  }
}

// Verifies: ordinary enum retrieval scales and retains shared metadata.
// Catches: repeated scans of all previously retrieved enum declarations.
TEST(XlsCApiTest, DslxMetadataLookupDoesNotScanDistinctEnumTypes) {
  class CountingEnumType : public xls::dslx::EnumType {
   public:
    CountingEnumType(const EnumType& type, int64_t& comparisons)
        : EnumType(type), comparisons_(comparisons) {}

    bool operator==(const xls::dslx::Type& other) const override {
      ++comparisons_;
      return EnumType::operator==(other);
    }

    std::unique_ptr<xls::dslx::Type> CloneToUnique() const override {
      return std::make_unique<CountingEnumType>(*this);
    }

   private:
    int64_t& comparisons_;
  };

  constexpr int64_t kCount = 64;
  std::string program;
  for (int64_t i = 0; i < kCount; ++i) {
    absl::StrAppendFormat(&program,
                          "enum E%d : u8 { A = 0 }\n"
                          "const V%d = E%d::A;\n",
                          i, i, i);
  }
  int64_t comparisons = 0;
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(program.c_str(), "enum_lookup.x",
                                           "enum_lookup", owner, &error,
                                           &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  std::vector<xls_dslx_expr*> expressions;
  for (int64_t i = 0; i < kCount; ++i) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, 2 * i + 1));
    ASSERT_NE(constant, nullptr);
    auto* expr = xls_dslx_constant_def_get_value(constant);
    expressions.push_back(expr);
    auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
    auto type = cpp_type_info->GetItem(cpp_expr);
    ASSERT_TRUE(type.has_value());
    auto* enumeration = dynamic_cast<const xls::dslx::EnumType*>(*type);
    ASSERT_NE(enumeration, nullptr);
    cpp_type_info->SetItem(cpp_expr, std::make_unique<CountingEnumType>(
                                         *enumeration, comparisons));
  }
  std::vector<std::weak_ptr<const void>> metadata;
  for (int pass = 0; pass < 2; ++pass) {
    comparisons = 0;
    for (int64_t i = 0; i < kCount; ++i) {
      xls_dslx_interp_value* value = nullptr;
      ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expressions[i],
                                                    &error, &value))
          << error;
      absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
      char* text = xls_dslx_interp_value_to_string(value);
      absl::Cleanup free_text([&] { xls_c_str_free(text); });
      EXPECT_EQ(std::string_view(text), absl::StrFormat("E%d:0", i));
      auto current = xls::GetDslxValueMetadataForTesting(value);
      ASSERT_FALSE(current.expired());
      if (pass == 0) {
        metadata.push_back(current);
      } else {
        EXPECT_EQ(current.lock(), metadata[i].lock());
      }
    }
    RecordProperty(pass == 0 ? "cold_comparisons" : "warm_comparisons",
                   std::to_string(comparisons));
    // A field-key implementation can avoid virtual Type comparisons entirely.
    EXPECT_LE(comparisons, 4 * kCount);
  }
}

// Verifies: enum keys retain exact dimensions and sign across equal clones.
// Catches: numeric-only dimensions or signedness changing cache reuse.
TEST(XlsCApiTest, DslxEnumMetadataUsesExactTypeDimensions) {
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      "enum E : u8 { A = 0 } const VALUE = E::A; "
      "enum Marker { Here() } const MARKER = Marker::Here();",
      "enum_dimensions.x", "enum_dimensions", owner, &error, &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  auto* constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(constant, nullptr);
  auto* expr = xls_dslx_constant_def_get_value(constant);
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto type = cpp_type_info->GetItem(cpp_expr);
  ASSERT_TRUE(type.has_value());
  const xls::dslx::EnumType original((*type)->AsEnum());
  xls_dslx_interp_value* first = nullptr;
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &first))
      << error;
  absl::Cleanup free_first([&] { xls_dslx_interp_value_free(first); });
  auto metadata = xls::GetDslxValueMetadataForTesting(first).lock();
  ASSERT_NE(metadata, nullptr);
  auto* marker_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 3));
  ASSERT_NE(marker_constant, nullptr);
  xls_dslx_interp_value* marker = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(marker_constant), &error,
      &marker))
      << error;
  absl::Cleanup free_marker([&] { xls_dslx_interp_value_free(marker); });
  xls_dslx_interp_value* first_fields[] = {marker, first};
  xls_dslx_interp_value* first_tuple = nullptr;
  ASSERT_TRUE(
      xls_dslx_interp_value_make_tuple(2, first_fields, &error, &first_tuple))
      << error;
  absl::Cleanup free_first_tuple(
      [&] { xls_dslx_interp_value_free(first_tuple); });

  struct EnumCase {
    const char* name;
    xls::dslx::TypeDim size;
    bool is_signed;
    bool equal;
  };
  const EnumCase cases[] = {
      {"different bit count", xls::dslx::TypeDim::CreateU32(16), false, false},
      {"different dimension width",
       xls::dslx::TypeDim(xls::dslx::InterpValue::MakeU64(8)), false, false},
      {"signed enum", xls::dslx::TypeDim::CreateU32(8), true, false},
      {"signed dimension value",
       xls::dslx::TypeDim(xls::dslx::InterpValue::MakeSBits(32, 8)), false,
       true}};
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    auto replacement = std::make_unique<xls::dslx::EnumType>(
        original.nominal_type(), test_case.size.Clone(), test_case.is_signed,
        original.members());
    ASSERT_EQ(original == *replacement, test_case.equal);
    cpp_type_info->SetItem(cpp_expr, std::move(replacement));
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(
        xls_dslx_type_info_get_const_expr(type_info, expr, &error, &value))
        << error;
    absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
    auto current = xls::GetDslxValueMetadataForTesting(value).lock();
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(metadata == current, test_case.equal);

    // Sum-bearing composition observes the nominal identity, not merely the
    // descriptor address; ordinary enum arrays intentionally allow mixed types.
    xls_dslx_interp_value* fields[] = {marker, value};
    xls_dslx_interp_value* tuple = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_make_tuple(2, fields, &error, &tuple))
        << error;
    absl::Cleanup free_tuple([&] { xls_dslx_interp_value_free(tuple); });
    xls_dslx_interp_value* elements[] = {first_tuple, tuple};
    xls_dslx_interp_value* array = nullptr;
    char* array_error = nullptr;
    absl::Cleanup free_array([&] {
      xls_dslx_interp_value_free(array);
      xls_c_str_free(array_error);
    });
    EXPECT_EQ(
        xls_dslx_interp_value_make_array(2, elements, &array_error, &array),
        test_case.equal);
    if (!test_case.equal) {
      EXPECT_EQ(array, nullptr);
      EXPECT_THAT(
          array_error,
          HasSubstr("Sum-bearing array elements have incompatible DSLX types"));
    }
  }
  cpp_type_info->SetItem(cpp_expr, original.CloneToUnique());
  xls_dslx_interp_value* equal_clone = nullptr;
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &equal_clone))
      << error;
  absl::Cleanup free_clone([&] { xls_dslx_interp_value_free(equal_clone); });
  EXPECT_EQ(metadata, xls::GetDslxValueMetadataForTesting(equal_clone).lock());
}

// Verifies: ordinary identities survive mixed growth and owner teardown.
// Catches: incomplete struct keys or identity changes during growth.
TEST(XlsCApiTest, DslxOrdinaryStructIdentitySurvivesMixedGrowth) {
  std::string program = R"(
enum Marker { Here() }
struct W<N: u32, UNUSED: u32> { value: uN[N] }
struct Other { value: u8 }
const FIRST = W<u32:8, u32:0> { value: u8:0 };
const SAME = W<u32:8, u32:1> { value: u8:0 };
const WIDER = W<u32:16, u32:0> { value: u16:0 };
const OTHER = Other { value: u8:0 };
const MARKER = Marker::Here();
)";
  constexpr int64_t kGrowthCount = 17;
  for (int64_t i = 0; i < kGrowthCount; ++i) {
    absl::StrAppendFormat(&program,
                          "struct O%d { value: u8 }\n"
                          "struct S%d { value: Marker }\n"
                          "const ORDINARY%d = O%d { value: u8:0 };\n"
                          "const SUM%d = S%d { value: Marker::Here() };\n",
                          i, i, i, i, i, i);
  }
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(program.c_str(), "struct_growth.x",
                                           "struct_growth", owner, &error,
                                           &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  std::vector<xls_dslx_interp_value*> values;
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  for (int64_t index : {3, 4, 5, 6, 7}) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, index));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
        << error;
    values.push_back(value);
  }
  for (int64_t i = 0; i < kGrowthCount; ++i) {
    for (int64_t index : {8 + 4 * i + 2, 8 + 4 * i + 3}) {
      auto* constant = xls_dslx_module_member_get_constant_def(
          xls_dslx_module_get_member(module, index));
      ASSERT_NE(constant, nullptr);
      xls_dslx_interp_value* value = nullptr;
      ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
          type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
          << error;
      xls_dslx_interp_value_free(value);
    }
  }
  auto* first_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 3));
  xls_dslx_interp_value* repeat = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(first_constant), &error,
      &repeat))
      << error;
  values.push_back(repeat);
  xls_dslx_typechecked_module_free(module_owner);
  module_owner = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;

  // Ordinary heterogeneous arrays are permissive. A sum-bearing tuple makes
  // retained nominal identities observable through the public array check.
  std::vector<xls_dslx_interp_value*> tuples;
  absl::Cleanup free_tuples([&] {
    for (auto* tuple : tuples) {
      xls_dslx_interp_value_free(tuple);
    }
  });
  for (int64_t index : {0, 1, 2, 3, 5}) {
    xls_dslx_interp_value* elements[] = {values[4], values[index]};
    xls_dslx_interp_value* tuple = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_make_tuple(2, elements, &error, &tuple))
        << error;
    tuples.push_back(tuple);
  }
  for (int64_t index : {1, 2, 3, 4}) {
    SCOPED_TRACE(index);
    xls_dslx_interp_value* elements[] = {tuples[0], tuples[index]};
    xls_dslx_interp_value* array = nullptr;
    char* array_error = nullptr;
    absl::Cleanup free_array([&] {
      xls_dslx_interp_value_free(array);
      xls_c_str_free(array_error);
    });
    if (index == 1 || index == 4) {
      ASSERT_TRUE(
          xls_dslx_interp_value_make_array(2, elements, &array_error, &array))
          << array_error;
      char* text = xls_dslx_interp_value_to_string(array);
      absl::Cleanup free_text([&] { xls_c_str_free(text); });
      EXPECT_THAT(text, HasSubstr("Marker::Here()"));
      EXPECT_THAT(text, HasSubstr("W {"));
    } else {
      EXPECT_FALSE(
          xls_dslx_interp_value_make_array(2, elements, &array_error, &array));
      EXPECT_EQ(array, nullptr);
      EXPECT_THAT(
          array_error,
          HasSubstr("Sum-bearing array elements have incompatible DSLX types"));
    }
  }
}

TEST(XlsCApiTest, DslxSumMetadataUsesFullEqualityAfterHashMatch) {
  constexpr const char* kProgram = R"(
enum Maybe { None, Some(u8) }
const VALUE: Maybe = Maybe::None;
)";
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "metadata_collision.x",
                                        "metadata_collision", owner, &error,
                                        &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  auto* constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(constant, nullptr);
  auto* expr = xls_dslx_constant_def_get_value(constant);
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto type = cpp_type_info->GetItem(cpp_expr);
  ASSERT_TRUE(type.has_value());
  auto* sum = dynamic_cast<const xls::dslx::SumType*>(*type);
  ASSERT_NE(sum, nullptr);
  const xls::dslx::SumType original(*sum);

  // A different concrete tag width preserves the declaration and argument hash
  // but not full Type equality. Inject these descriptions at the metadata
  // boundary to force a routing-key collision without a production test hook.
  std::vector<xls::dslx::SumTypeVariant> variants;
  for (const auto& variant : original.variants()) {
    variants.push_back(variant.Clone());
  }
  auto different = std::make_unique<xls::dslx::SumType>(
      original.nominal_type(), std::move(variants),
      xls::dslx::TypeDim::CreateU32(2));
  ASSERT_EQ(&original.nominal_type(), &different->nominal_type());
  ASSERT_EQ(original.parametric_arguments_hash(),
            different->parametric_arguments_hash());
  ASSERT_FALSE(original == *different);

  xls_dslx_interp_value* values[3] = {};
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expr, &error,
                                             &values[0]))
      << error;
  cpp_type_info->SetItem(cpp_expr, std::move(different));
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expr, &error,
                                             &values[1]))
      << error;
  cpp_type_info->SetItem(cpp_expr, original.CloneToUnique());
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(type_info, expr, &error,
                                             &values[2]))
      << error;

  auto first_metadata = xls::GetDslxValueMetadataForTesting(values[0]).lock();
  auto different_metadata = xls::GetDslxValueMetadataForTesting(values[1]).lock();
  ASSERT_NE(first_metadata, nullptr);
  ASSERT_NE(different_metadata, nullptr);
  EXPECT_NE(first_metadata, different_metadata);
  EXPECT_EQ(first_metadata,
            xls::GetDslxValueMetadataForTesting(values[2]).lock());
}

TEST(XlsCApiTest, DslxSumBearingStructMetadataUsesFullEqualityAfterHashMatch) {
  constexpr const char* kProgram = R"(
enum Maybe { None, Some(u8) }
struct Wrapper { value: Maybe }
const VALUE = Wrapper { value: Maybe::None };
)";
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "struct_metadata_collision.x", "struct_metadata_collision",
      owner, &error, &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  auto* constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 2));
  ASSERT_NE(constant, nullptr);
  auto* expr = xls_dslx_constant_def_get_value(constant);
  auto* cpp_expr = reinterpret_cast<xls::dslx::Expr*>(expr);
  auto* cpp_type_info = reinterpret_cast<xls::dslx::TypeInfo*>(type_info);
  auto type = cpp_type_info->GetItem(cpp_expr);
  ASSERT_TRUE(type.has_value());
  auto* structure = dynamic_cast<const xls::dslx::StructType*>(*type);
  ASSERT_NE(structure, nullptr);
  auto original = structure->CloneToUnique();
  auto* sum =
      dynamic_cast<const xls::dslx::SumType*>(&structure->GetMemberType(0));
  ASSERT_NE(sum, nullptr);

  // The nested sum's tag width changes full Type equality without changing
  // either the struct declaration or its complete cache fingerprint.
  std::vector<xls::dslx::SumTypeVariant> variants;
  for (const auto& variant : sum->variants()) {
    variants.push_back(variant.Clone());
  }
  std::vector<std::unique_ptr<xls::dslx::Type>> members;
  members.push_back(std::make_unique<xls::dslx::SumType>(
      sum->nominal_type(), std::move(variants),
      xls::dslx::TypeDim::CreateU32(2)));
  auto different = std::make_unique<xls::dslx::StructType>(
      std::move(members), structure->nominal_type(),
      structure->nominal_type_dims_by_identifier());
  ASSERT_EQ(&structure->nominal_type(), &different->nominal_type());
  ASSERT_EQ(xls::dslx::HashTypeForSumCache(*original),
            xls::dslx::HashTypeForSumCache(*different));
  ASSERT_FALSE(*original == *different);

  xls_dslx_interp_value* values[3] = {};
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &values[0]))
      << error;
  cpp_type_info->SetItem(cpp_expr, std::move(different));
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &values[1]))
      << error;
  auto equal = original->CloneToUnique();
  ASSERT_TRUE(*original == *equal);
  cpp_type_info->SetItem(cpp_expr, std::move(equal));
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &values[2]))
      << error;

  // Struct descriptors are rebuilt, so test their nominal identities through
  // C array composition rather than comparing descriptor addresses.
  xls_dslx_interp_value* incompatible[] = {values[0], values[1]};
  xls_dslx_interp_value* rejected = nullptr;
  char* array_error = nullptr;
  absl::Cleanup free_rejected([&] {
    xls_dslx_interp_value_free(rejected);
    xls_c_str_free(array_error);
  });
  EXPECT_FALSE(xls_dslx_interp_value_make_array(2, incompatible, &array_error,
                                                &rejected));
  EXPECT_EQ(rejected, nullptr);
  EXPECT_THAT(
      array_error,
      HasSubstr("Sum-bearing array elements have incompatible DSLX types"));

  xls_dslx_interp_value* compatible[] = {values[0], values[2]};
  xls_dslx_interp_value* array = nullptr;
  absl::Cleanup free_array([&] { xls_dslx_interp_value_free(array); });
  ASSERT_TRUE(xls_dslx_interp_value_make_array(2, compatible, &error, &array))
      << error;
  auto* retained = xls_dslx_interp_value_clone(array);
  ASSERT_NE(retained, nullptr);
  absl::Cleanup free_retained([&] { xls_dslx_interp_value_free(retained); });
  xls_dslx_interp_value_free(array);
  array = nullptr;
  for (auto*& value : values) {
    xls_dslx_interp_value_free(value);
    value = nullptr;
  }
  original.reset();
  xls_dslx_typechecked_module_free(module_owner);
  module_owner = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;

  char* text = xls_dslx_interp_value_to_string(retained);
  absl::Cleanup free_text([&] { xls_c_str_free(text); });
  EXPECT_STREQ(text,
               "[\n"
               "    Wrapper {\n"
               "        value: Maybe::None\n"
               "    },\n"
               "    Wrapper {\n"
               "        value: Maybe::None\n"
               "    }\n"
               "]");
}

TEST(XlsCApiTest, DslxSumAndEnumMetadataSurvivesMixedGrowth) {
  constexpr int64_t kCount = 17;
  std::string program;
  for (int64_t i = 0; i < kCount; ++i) {
    absl::StrAppendFormat(&program,
                          "enum E%d : u8 { A = 0 }\n"
                          "enum S%d { Wrap(E%d) }\n"
                          "const SUM%d = S%d::Wrap(E%d::A);\n"
                          "const ENUM%d = E%d::A;\n",
                          i, i, i, i, i, i, i, i);
  }
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      program.c_str(), "metadata_growth.x", "metadata_growth", owner, &error,
      &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  std::vector<std::weak_ptr<const void>> metadata(2 * kCount);
  for (int pass = 0; pass < 3; ++pass) {
    for (int64_t offset = 0; offset < 2 * kCount; ++offset) {
      const int64_t index = pass == 1 ? 2 * kCount - 1 - offset : offset;
      const int64_t i = index / 2;
      const bool is_sum = index % 2 == 0;
      SCOPED_TRACE(absl::StrFormat("pass=%d index=%d", pass, index));
      auto* enum_def = xls_dslx_module_member_get_enum_def(
          xls_dslx_module_get_member(module, 4 * i));
      ASSERT_NE(enum_def, nullptr);
      const auto* enum_type =
          xls_dslx_type_info_get_type_enum_def(type_info, enum_def);
      ASSERT_NE(enum_type, nullptr);
      if (pass == 0 && is_sum) {
        // Each cold sum must publish its descriptor after recursively interning
        // a fresh enum leaf. Later pairs also grow the owner's sum storage.
        EXPECT_TRUE(xls::GetDslxCachedEnumMetadataForTesting(owner, enum_type)
                        .expired());
      }
      auto* constant = xls_dslx_module_member_get_constant_def(
          xls_dslx_module_get_member(module, 4 * i + 2 + index % 2));
      ASSERT_NE(constant, nullptr);
      xls_dslx_interp_value* value = nullptr;
      absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
      ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
          type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
          << error;
      ASSERT_NE(value, nullptr);
      auto current = xls::GetDslxValueMetadataForTesting(value);
      ASSERT_FALSE(current.expired());
      const auto enum_metadata =
          xls::GetDslxCachedEnumMetadataForTesting(owner, enum_type);
      ASSERT_FALSE(enum_metadata.expired());
      if (is_sum) {
        EXPECT_NE(current.lock(), enum_metadata.lock());
      } else {
        EXPECT_EQ(current.lock(), enum_metadata.lock());
      }
      if (pass == 0) {
        metadata[index] = current;
      } else {
        EXPECT_EQ(current.lock(), metadata[index].lock());
      }
      char* text = xls_dslx_interp_value_to_string(value);
      absl::Cleanup free_text([&] { xls_c_str_free(text); });
      ASSERT_NE(text, nullptr);
      if (is_sum) {
        EXPECT_THAT(text, HasSubstr(absl::StrFormat("S%d::Wrap(", i)));
        EXPECT_THAT(text, HasSubstr(absl::StrFormat("E%d::A", i)));
      } else {
        EXPECT_EQ(std::string_view(text), absl::StrFormat("E%d:0", i));
      }
    }
  }
}

TEST(XlsCApiTest,
     DslxConstExprMetadataDistinguishesBitsFromNominalAndEmptyValues) {
  constexpr const char* kProgram = R"(
type ByteAlias = u8;
enum Plain : u2 { A = 1 }
enum Marker { Present() }
const UNSIGNED = u32:7;
const SIGNED = s8:-2;
const ZERO = uN[0]:0;
const ALIAS: ByteAlias = u8:3;
const ENUM = Plain::A;
const SUM = Marker::Present();
const EMPTY = u8[0]:[];
const NESTED_EMPTY = ([EMPTY],);
)";
  struct MetadataCase {
    const char* name;
    bool needs_metadata;
  };
  constexpr MetadataCase kCases[] = {
      {"UNSIGNED", false}, {"SIGNED", false}, {"ZERO", false},
      {"ALIAS", false},    {"ENUM", true},    {"SUM", true},
      {"EMPTY", true},     {"NESTED_EMPTY", true}};
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(owner, nullptr);
  absl::Cleanup free_owner([&] { xls_dslx_import_data_free(owner); });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "metadata_bits.x", "metadata_bits", owner, &error,
      &module_owner))
      << error;
  absl::Cleanup free_module(
      [&] { xls_dslx_typechecked_module_free(module_owner); });
  auto* module = xls_dslx_typechecked_module_get_module(module_owner);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(module_owner);
  int64_t member_index = 3;
  for (const MetadataCase& test_case : kCases) {
    SCOPED_TRACE(test_case.name);
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, member_index++));
    ASSERT_NE(constant, nullptr);
    char* name = xls_dslx_constant_def_get_name(constant);
    absl::Cleanup free_name([&] { xls_c_str_free(name); });
    EXPECT_STREQ(name, test_case.name);
    xls_dslx_interp_value* value = nullptr;
    absl::Cleanup free_value([&] { xls_dslx_interp_value_free(value); });
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value))
        << error;
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(!xls::GetDslxValueMetadataForTesting(value).expired(),
              test_case.needs_metadata);
  }
}

TEST(XlsCApiTest, DslxSemanticSumArraysRequireCompatibleElementTypes) {
  constexpr const char* kProgram = R"(
#![feature(generics)]
enum Maybe { None, Some(u8) }
enum Other { None, Some(u8) }
enum Phantom<N: u32, T: type> { Only() }
type ByteAlias = u8;
const FIRST: Maybe = Maybe::Some(u8:42);
const EMPTY: Maybe = Maybe::None;
const SECOND: Other = Other::Some(u8:7);
const ONE: Phantom<u32:1, u8> = Phantom<u32:1, u8>::Only();
const TWO: Phantom<u32:2, u8> = Phantom<u32:2, u8>::Only();
const WIDER: Phantom<u32:1, u16> = Phantom<u32:1, u16>::Only();
const SAME: Phantom<u32:1, ByteAlias> = Phantom<u32:1, ByteAlias>::Only();
)";
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data([&] {
    if (import_data != nullptr) {
      xls_dslx_import_data_free(import_data);
    }
  });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* module_owner = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "array_types.x",
                                           "array_types", import_data, &error,
                                           &module_owner))
      << (error == nullptr ? "unknown error" : error);
  absl::Cleanup free_module([&] {
    if (module_owner != nullptr) {
      xls_dslx_typechecked_module_free(module_owner);
    }
  });
  xls_dslx_module* module =
      xls_dslx_typechecked_module_get_module(module_owner);
  xls_dslx_type_info* type_info =
      xls_dslx_typechecked_module_get_type_info(module_owner);
  std::vector<xls_dslx_interp_value*> values;
  absl::Cleanup free_values([&] {
    for (xls_dslx_interp_value* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  for (int64_t member_index : {4, 5, 6, 7, 8, 9, 10}) {
    xls_dslx_constant_def* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, member_index));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value));
    ASSERT_NE(value, nullptr);
    values.push_back(value);
  }
  values.push_back(xls_dslx_interp_value_make_ubits(8, 7));
  ASSERT_NE(values.back(), nullptr);

  // The retained values must carry their own identity and formatting. Freeing
  // only the typechecked-module wrapper would leave the AST owner alive.
  xls_dslx_typechecked_module_free(module_owner);
  module_owner = nullptr;
  xls_dslx_import_data_free(import_data);
  import_data = nullptr;
  values.push_back(xls_dslx_interp_value_clone(values[0]));
  ASSERT_NE(values.back(), nullptr);

  const std::vector<std::pair<xls_dslx_interp_value*, xls_dslx_interp_value*>>
      incompatible = {{values[0], values[2]},
                      {values[0], values[7]},
                      {values[7], values[0]},
                      {values[3], values[4]},
                      {values[3], values[5]}};
  for (const auto& [first, second] : incompatible) {
    EXPECT_NE(xls::GetDslxValueMetadataForTesting(first).lock(),
              xls::GetDslxValueMetadataForTesting(second).lock());
    xls_dslx_interp_value* elements[] = {first, second};
    xls_dslx_interp_value* array = nullptr;
    char* array_error = nullptr;
    absl::Cleanup free_array([&] {
      if (array != nullptr) {
        xls_dslx_interp_value_free(array);
      }
      xls_c_str_free(array_error);
    });
    EXPECT_FALSE(
        xls_dslx_interp_value_make_array(2, elements, &array_error, &array));
    EXPECT_EQ(array, nullptr);
    EXPECT_NE(array_error, nullptr);
  }

  // Constructors, aliases, and handle clones do not create new nominal types.
  const std::vector<std::pair<xls_dslx_interp_value*, xls_dslx_interp_value*>>
      compatible = {{values[1], values[0]},
                    {values[3], values[6]},
                    {values[0], values[8]}};
  for (const auto& [first, second] : compatible) {
    EXPECT_EQ(xls::GetDslxValueMetadataForTesting(first).lock(),
              xls::GetDslxValueMetadataForTesting(second).lock());
    xls_dslx_interp_value* elements[] = {first, second};
    xls_dslx_interp_value* array = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_make_array(2, elements, &error, &array));
    absl::Cleanup free_array([&] { xls_dslx_interp_value_free(array); });
    char* text = xls_dslx_interp_value_to_string(array);
    absl::Cleanup free_text([&] { xls_c_str_free(text); });
    EXPECT_THAT(text, HasSubstr(first == values[3] ? "Phantom::Only()"
                                                   : "Maybe::Some(u8:42)"));
  }
}

TEST(XlsCApiTest, DslxSemanticSumMetadataSurvivesNonoverlappingHandles) {
  constexpr const char* kProgram = R"(
enum Maybe { None, Some(u8) }
const FIRST: Maybe = Maybe::None;
const SECOND: Maybe = Maybe::Some(u8:42);
)";
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_interp_value* first = nullptr;
  xls_dslx_interp_value* second = nullptr;
  xls_dslx_interp_value* cloned = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_interp_value_free(first);
    xls_dslx_interp_value_free(second);
    xls_dslx_interp_value_free(cloned);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "sum_metadata.x",
                                           "sum_metadata", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* first_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(first_constant, nullptr);
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(first_constant), &error,
      &first));
  const auto metadata = xls::GetDslxValueMetadataForTesting(first);
  ASSERT_FALSE(metadata.expired());
  char* first_text = xls_dslx_interp_value_to_string(first);
  absl::Cleanup free_first_text([&] { xls_c_str_free(first_text); });
  EXPECT_STREQ(first_text, "Maybe::None");
  xls_dslx_interp_value_free(first);
  first = nullptr;
  // No live value or strong test witness bridges these independent handles.
  EXPECT_FALSE(metadata.expired());

  auto* second_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 2));
  ASSERT_NE(second_constant, nullptr);
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      type_info, xls_dslx_constant_def_get_value(second_constant), &error,
      &second));
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(second).lock());
  cloned = xls_dslx_interp_value_clone(second);
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(cloned).lock());
  xls_dslx_interp_value_free(second);
  second = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;

  // The clone retains the descriptor without retaining the compilation owner.
  EXPECT_FALSE(metadata.expired());
  char* clone_text = xls_dslx_interp_value_to_string(cloned);
  absl::Cleanup free_clone_text([&] { xls_c_str_free(clone_text); });
  EXPECT_STREQ(clone_text, "Maybe::Some(u8:42)");
  xls_dslx_interp_value_free(cloned);
  cloned = nullptr;
  EXPECT_TRUE(metadata.expired());
}

void ExpectEnumMetadataReuse(bool typed_first) {
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_interp_value* first = nullptr;
  xls_dslx_interp_value* second = nullptr;
  xls_dslx_interp_value* typed = nullptr;
  xls_dslx_interp_value* cloned = nullptr;
  xls_dslx_parametric_env* env = nullptr;
  xls_bits* first_bits = nullptr;
  xls_bits* last_bits = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_interp_value_free(first);
    xls_dslx_interp_value_free(second);
    xls_dslx_interp_value_free(typed);
    xls_dslx_interp_value_free(cloned);
    if (env != nullptr) {
      xls_dslx_parametric_env_free(env);
    }
    xls_bits_free(first_bits);
    xls_bits_free(last_bits);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      "enum E : u2 { A = 0, B = 1, AliasA = 0 } const VALUE: E = E::A;",
      "enum_metadata.x", "enum_metadata", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* def = xls_dslx_module_member_get_enum_def(
      xls_dslx_module_get_member(module, 0));
  auto* constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  const auto* type = xls_dslx_type_info_get_type_enum_def(type_info, def);
  ASSERT_NE(type, nullptr);
  auto* expr = xls_dslx_constant_def_get_value(constant);
  ASSERT_TRUE(xls_bits_make_ubits(2, 0, &error, &first_bits));
  ASSERT_TRUE(xls_bits_make_ubits(2, 1, &error, &last_bits));
  EXPECT_TRUE(xls::GetDslxCachedEnumMetadataForTesting(owner, type).expired());
  if (typed_first) {
    ASSERT_TRUE(
        xls_dslx_type_info_get_const_expr(type_info, expr, &error, &first));
  } else {
    ASSERT_TRUE(xls_dslx_interp_value_make_enum(def, false, first_bits, &error,
                                                &first));
  }
  const auto metadata = xls::GetDslxValueMetadataForTesting(first);
  ASSERT_FALSE(metadata.expired());
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxCachedEnumMetadataForTesting(owner, type).lock());
  xls_dslx_interp_value_free(first);
  first = nullptr;
  // No live value or strong test witness bridges these independent handles.
  EXPECT_FALSE(metadata.expired());
  ASSERT_TRUE(
      xls_dslx_interp_value_make_enum(def, false, last_bits, &error, &second));
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(second).lock());
  ASSERT_TRUE(
      xls_dslx_type_info_get_const_expr(type_info, expr, &error, &typed));
  EXPECT_EQ(metadata.lock(), xls::GetDslxValueMetadataForTesting(typed).lock());
  char* raw_text = xls_dslx_interp_value_to_string(second);
  EXPECT_STREQ(raw_text, "E:1");
  xls_c_str_free(raw_text);
  char* typed_text = xls_dslx_interp_value_to_string(typed);
  EXPECT_STREQ(typed_text, "E:0");
  xls_c_str_free(typed_text);

  xls_dslx_parametric_env_item items[] = {{"VALUE", second}};
  ASSERT_TRUE(xls_dslx_parametric_env_create(items, 1, &error, &env));
  auto* borrowed = xls_dslx_parametric_env_get_binding_value(env, 0);
  ASSERT_NE(borrowed, nullptr);
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(borrowed).lock());
  cloned = xls_dslx_interp_value_clone(borrowed);
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(cloned).lock());
  xls_dslx_parametric_env_free(env);
  env = nullptr;
  xls_dslx_interp_value_free(second);
  second = nullptr;
  xls_dslx_interp_value_free(typed);
  typed = nullptr;
  xls_dslx_interp_value_free(cloned);
  cloned = nullptr;
  EXPECT_FALSE(metadata.expired());
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  EXPECT_TRUE(metadata.expired());
}

TEST(XlsCApiTest, DslxEnumMetadataSurvivesNonoverlappingHandles) {
  ExpectEnumMetadataReuse(/*typed_first=*/false);
}

TEST(XlsCApiTest, DslxTypedEnumMetadataIsReusedByRawConstruction) {
  ExpectEnumMetadataReuse(/*typed_first=*/true);
}

TEST(XlsCApiTest, DslxAggregateMetadataReusesOwnerEnumLeaves) {
  struct AggregateCase {
    const char* expression;
    const char* text;
    bool is_sum;
  };
  for (const AggregateCase& aggregate :
       {AggregateCase{"(E::A,)", "(E:0)", false},
        {"E[1]:[E::A]", "[E:0]", false},
        {"S { e: E::A }", "(E:0)", false},
        {"Choice::Some((S { e: E::A }, E[1]:[E::A]))",
         "Choice::Some(\n"
         "    (\n"
         "        S {\n"
         "            e: E::AliasA  // u2:0\n"
         "        },\n"
         "        [\n"
         "            E::AliasA  // u2:0\n"
         "        ]\n"
         "    )\n"
         ")",
         true}}) {
    SCOPED_TRACE(aggregate.expression);
    std::string program = absl::StrCat(R"(
enum E : u2 { A = 0, B = 1, AliasA = 0 }
struct S { e: E }
enum Choice { None, Some((S, E[1])) }
const VALUE = )",
                                       aggregate.expression, ";");
    auto* owner = xls_dslx_import_data_create(
        std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
    xls_dslx_typechecked_module* tm = nullptr;
    xls_dslx_interp_value* first = nullptr;
    xls_dslx_interp_value* second = nullptr;
    xls_dslx_interp_value* clone = nullptr;
    char* error = nullptr;
    absl::Cleanup cleanup([&] {
      xls_c_str_free(error);
      xls_dslx_interp_value_free(first);
      xls_dslx_interp_value_free(second);
      xls_dslx_interp_value_free(clone);
      xls_dslx_typechecked_module_free(tm);
      xls_dslx_import_data_free(owner);
    });
    ASSERT_TRUE(
        xls_dslx_parse_and_typecheck(program.c_str(), "enum_aggregate.x",
                                     "enum_aggregate", owner, &error, &tm))
        << error;
    auto* module = xls_dslx_typechecked_module_get_module(tm);
    auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
    auto* enum_def = xls_dslx_module_member_get_enum_def(
        xls_dslx_module_get_member(module, 0));
    const auto* enum_type =
        xls_dslx_type_info_get_type_enum_def(type_info, enum_def);
    ASSERT_NE(enum_type, nullptr);
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, 3));
    auto* expr = xls_dslx_constant_def_get_value(constant);
    EXPECT_TRUE(
        xls::GetDslxCachedEnumMetadataForTesting(owner, enum_type).expired());
    ASSERT_TRUE(
        xls_dslx_type_info_get_const_expr(type_info, expr, &error, &first));
    const auto enum_metadata =
        xls::GetDslxCachedEnumMetadataForTesting(owner, enum_type);
    EXPECT_FALSE(enum_metadata.expired());
    const auto first_metadata = xls::GetDslxValueMetadataForTesting(first);
    ASSERT_TRUE(
        xls_dslx_type_info_get_const_expr(type_info, expr, &error, &second));
    EXPECT_EQ(
        enum_metadata.lock(),
        xls::GetDslxCachedEnumMetadataForTesting(owner, enum_type).lock());
    if (!aggregate.is_sum) {
      // Reuse the expensive enum leaves, not entire aggregate descriptors.
      EXPECT_NE(first_metadata.lock(),
                xls::GetDslxValueMetadataForTesting(second).lock());
    }
    clone = xls_dslx_interp_value_clone(second);
    const auto clone_metadata = xls::GetDslxValueMetadataForTesting(clone);
    char* text = xls_dslx_interp_value_to_string(clone);
    EXPECT_STREQ(text, aggregate.text);
    xls_c_str_free(text);
    xls_dslx_interp_value_free(first);
    first = nullptr;
    if (!aggregate.is_sum) {
      EXPECT_TRUE(first_metadata.expired());
    }
    xls_dslx_interp_value_free(second);
    second = nullptr;
    xls_dslx_typechecked_module_free(tm);
    tm = nullptr;
    xls_dslx_import_data_free(owner);
    owner = nullptr;
    EXPECT_FALSE(clone_metadata.expired());
    // Ordinary enum text still uses the live source definition. Sum formatting
    // instead owns all its names and can be observed after source teardown.
    if (aggregate.is_sum) {
      char* surviving_text = xls_dslx_interp_value_to_string(clone);
      EXPECT_STREQ(surviving_text, aggregate.text);
      xls_c_str_free(surviving_text);
    }
    xls_dslx_interp_value_free(clone);
    clone = nullptr;
    EXPECT_TRUE(clone_metadata.expired());
  }
}

TEST(XlsCApiTest, DslxColdInvalidEnumDoesNotPrimeMetadata) {
  struct RawEnumCase {
    int64_t bit_count;
    uint64_t value;
    bool is_signed;
    const char* text;
  };
  for (const RawEnumCase& raw : {RawEnumCase{2, 2, false, "E:2"},
                                 {3, 0, false, "E:0"},
                                 {2, 0, true, "E:0"}}) {
    SCOPED_TRACE(absl::StrFormat("width=%d value=%d signed=%d", raw.bit_count,
                                 raw.value, raw.is_signed));
    xls_dslx_import_data* owner = xls_dslx_import_data_create(
        std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
    xls_dslx_typechecked_module* tm = nullptr;
    xls_dslx_interp_value* invalid = nullptr;
    xls_dslx_interp_value* valid = nullptr;
    xls_dslx_interp_value* marker = nullptr;
    xls_bits* raw_bits = nullptr;
    xls_bits* valid_bits = nullptr;
    char* error = nullptr;
    absl::Cleanup cleanup([&] {
      xls_c_str_free(error);
      xls_bits_free(raw_bits);
      xls_bits_free(valid_bits);
      xls_dslx_interp_value_free(invalid);
      xls_dslx_interp_value_free(valid);
      xls_dslx_interp_value_free(marker);
      xls_dslx_typechecked_module_free(tm);
      xls_dslx_import_data_free(owner);
    });
    ASSERT_TRUE(xls_dslx_parse_and_typecheck(
        "enum E : u2 { A = 0, B = 1 } enum Marker { Present() } "
        "const MARKER: Marker = Marker::Present();",
        "invalid_enum.x", "invalid_enum", owner, &error, &tm))
        << error;
    auto* module = xls_dslx_typechecked_module_get_module(tm);
    auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
    auto* def = xls_dslx_module_member_get_enum_def(
        xls_dslx_module_get_member(module, 0));
    const auto* type = xls_dslx_type_info_get_type_enum_def(type_info, def);
    ASSERT_NE(type, nullptr);
    EXPECT_TRUE(
        xls::GetDslxCachedEnumMetadataForTesting(owner, type).expired());
    ASSERT_TRUE(
        xls_bits_make_ubits(raw.bit_count, raw.value, &error, &raw_bits));
    ASSERT_TRUE(xls_dslx_interp_value_make_enum(def, raw.is_signed, raw_bits,
                                                &error, &invalid));
    EXPECT_TRUE(
        xls::GetDslxCachedEnumMetadataForTesting(owner, type).expired());
    char* text = xls_dslx_interp_value_to_string(invalid);
    EXPECT_STREQ(text, raw.text);
    xls_c_str_free(text);

    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, 2));
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &marker));
    xls_dslx_interp_value* fields[] = {marker, invalid};
    xls_dslx_interp_value* rejected = nullptr;
    char* tuple_error = nullptr;
    EXPECT_FALSE(
        xls_dslx_interp_value_make_tuple(2, fields, &tuple_error, &rejected));
    EXPECT_EQ(rejected, nullptr);
    EXPECT_NE(tuple_error, nullptr);
    xls_dslx_interp_value_free(rejected);
    xls_c_str_free(tuple_error);
    xls_dslx_interp_value_free(invalid);
    invalid = nullptr;
    EXPECT_TRUE(
        xls::GetDslxCachedEnumMetadataForTesting(owner, type).expired());

    // Positive control: the observer sees a subsequent valid construction.
    ASSERT_TRUE(xls_bits_make_ubits(2, 0, &error, &valid_bits));
    ASSERT_TRUE(xls_dslx_interp_value_make_enum(def, false, valid_bits, &error,
                                                &valid));
    EXPECT_FALSE(
        xls::GetDslxCachedEnumMetadataForTesting(owner, type).expired());
  }
}

TEST(XlsCApiTest, DslxEnumMetadataUsesNominalDeclarationIdentity) {
  constexpr const char* kEnum = "pub enum E : u2 { A = 0, B = 1 }";
  xls_dslx_import_data* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_import_data* separate_owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* original = nullptr;
  xls_dslx_typechecked_module* cloned = nullptr;
  xls_dslx_typechecked_module* consumer = nullptr;
  xls_dslx_typechecked_module* separate = nullptr;
  std::vector<xls_dslx_interp_value*> values;
  xls_bits* bits = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_bits_free(bits);
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
    xls_dslx_typechecked_module_free(consumer);
    xls_dslx_typechecked_module_free(cloned);
    xls_dslx_typechecked_module_free(original);
    xls_dslx_typechecked_module_free(separate);
    xls_dslx_import_data_free(owner);
    xls_dslx_import_data_free(separate_owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kEnum, "enum.x", "a", owner, &error,
                                           &original))
      << error;
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      original, nullptr, 0, "b", owner, &error, &cloned))
      << error;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      "import a; import b; type Alias = a::E; "
      "const FIRST: Alias = Alias::A; const LAST: a::E = a::E::B; "
      "const CLONED: b::E = b::E::A;",
      "main.x", "main", owner, &error, &consumer))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(consumer);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(consumer);
  for (int64_t index : {3, 4, 5}) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, index));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value));
    values.push_back(value);
  }
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kEnum, "enum.x", "a", separate_owner,
                                           &error, &separate))
      << error;
  ASSERT_TRUE(xls_bits_make_ubits(2, 0, &error, &bits));
  for (auto* tm : {original, separate}) {
    auto* def = xls_dslx_module_member_get_enum_def(xls_dslx_module_get_member(
        xls_dslx_typechecked_module_get_module(tm), 0));
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(
        xls_dslx_interp_value_make_enum(def, false, bits, &error, &value));
    values.push_back(value);
  }
  const auto metadata = xls::GetDslxValueMetadataForTesting(values[0]);
  ASSERT_FALSE(metadata.expired());
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(values[1]).lock());
  EXPECT_EQ(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(values[3]).lock());
  EXPECT_NE(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(values[2]).lock());
  EXPECT_NE(metadata.lock(),
            xls::GetDslxValueMetadataForTesting(values[4]).lock());
}

TEST(XlsCApiTest, DslxRawEnumFromRetainedTransformedModule) {
  constexpr const char* kImported = "pub fn f<N: u32>() -> u32 { N }";
  constexpr const char* kProgram = R"(
import imported;
enum E: u2 { A = 0 }
const VALUE: E = E::A;
fn caller() -> u32 { imported::f<u32:1>() }
proc P {}
impl P {
  fn new() -> Self { P {} }
}
#[test]
fn test_spawn() {
  let p = P::new();
  p.spawn();
}
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* imported_tm = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_invocation_callee_data_array* invocations = nullptr;
  xls_bits* bits = nullptr;
  xls_dslx_interp_value* value = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_interp_value_free(value);
    xls_bits_free(bits);
    xls_dslx_invocation_callee_data_array_free(invocations);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_typechecked_module_free(imported_tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kImported, "imported.x", "imported",
                                           owner, &error, &imported_tm))
      << error;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "retained_enum.x",
                                           "retained_enum", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* imported = xls_dslx_typechecked_module_get_module(imported_tm);
  auto* function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(imported, 0));
  ASSERT_NE(function, nullptr);

  // Transforming the spawning test reparses the module, but imported calls
  // still expose callers from the retained original through public accessors.
  invocations =
      xls_dslx_type_info_get_all_invocation_callee_data(type_info, function);
  ASSERT_NE(invocations, nullptr);
  xls_dslx_module* retained_module = nullptr;
  for (int64_t i = 0;
       i < xls_dslx_invocation_callee_data_array_get_count(invocations); ++i) {
    auto* data = xls_dslx_invocation_callee_data_array_get(invocations, i);
    auto* invocation = xls_dslx_invocation_callee_data_get_invocation(data);
    auto* root_data =
        xls_dslx_type_info_get_root_invocation_data(type_info, invocation);
    ASSERT_NE(root_data, nullptr);
    auto* caller = xls_dslx_invocation_data_get_caller(root_data);
    ASSERT_NE(caller, nullptr);
    auto* caller_module =
        xls_dslx_expr_get_owner_module(xls_dslx_function_get_body(caller));
    if (caller_module != module) {
      retained_module = caller_module;
      break;
    }
  }
  ASSERT_NE(retained_module, nullptr);
  ASSERT_NE(retained_module, module);
  for (int64_t i = 0; i < xls_dslx_module_get_member_count(module); ++i) {
    EXPECT_NE(
        xls_dslx_module_member_get_kind(xls_dslx_module_get_member(module, i)),
        xls_dslx_module_member_kind_test_function);
  }
  auto* enum_def = xls_dslx_module_member_get_enum_def(
      xls_dslx_module_get_member(retained_module, 1));
  ASSERT_NE(enum_def, nullptr);
  ASSERT_TRUE(xls_bits_make_ubits(2, 0, &error, &bits));
  ASSERT_TRUE(
      xls_dslx_interp_value_make_enum(enum_def, false, bits, &error, &value))
      << error;
  char* text = xls_dslx_interp_value_to_string(value);
  EXPECT_STREQ(text, "E:0");
  xls_c_str_free(text);
}

enum class MetadataWaiter { kNone, kEnum, kConstExpr };
enum class MetadataTemperature { kCold, kWarm };

void ExpectMetadataDoesNotWaitForUnrelatedParse(
    MetadataWaiter waiter, MetadataTemperature temperature) {
  SCOPED_TRACE(temperature == MetadataTemperature::kWarm ? "warm" : "cold");
  // The cloned enum belongs to destination even though it keeps source's
  // FileTable. Pause a real source parse while constructing destination
  // metadata.
  xls_dslx_import_data* source = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_import_data* destination = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  absl::Cleanup free_owners([&] {
    xls_dslx_import_data_free(source);
    xls_dslx_import_data_free(destination);
  });
  char* error = nullptr;
  char* parse_error = nullptr;
  char* enum_error = nullptr;
  char* const_expr_error = nullptr;
  char* waiter_error = nullptr;
  absl::Cleanup free_errors([&] {
    xls_c_str_free(error);
    xls_c_str_free(parse_error);
    xls_c_str_free(enum_error);
    xls_c_str_free(const_expr_error);
    xls_c_str_free(waiter_error);
  });
  xls_dslx_typechecked_module* original = nullptr;
  xls_dslx_typechecked_module* cloned = nullptr;
  xls_dslx_typechecked_module* parsed = nullptr;
  absl::Cleanup free_modules([&] {
    xls_dslx_typechecked_module_free(original);
    xls_dslx_typechecked_module_free(cloned);
    xls_dslx_typechecked_module_free(parsed);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      "enum E : u2 { A = 0, B = 1 } const VALUE: E = E::A;", "owner.x", "owner",
      source, &error, &original))
      << error;
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      original, nullptr, 0, "destination", destination, &error, &cloned))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(cloned);
  auto* enum_def = xls_dslx_module_member_get_enum_def(
      xls_dslx_module_get_member(module, 0));
  ASSERT_NE(enum_def, nullptr);
  auto* original_module = xls_dslx_typechecked_module_get_module(original);
  auto* original_enum = xls_dslx_module_member_get_enum_def(
      xls_dslx_module_get_member(original_module, 0));
  auto* original_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(original_module, 1));
  auto* cloned_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(original_enum, nullptr);
  ASSERT_NE(original_constant, nullptr);
  ASSERT_NE(cloned_constant, nullptr);
  auto* original_type_info =
      xls_dslx_typechecked_module_get_type_info(original);
  auto* cloned_type_info = xls_dslx_typechecked_module_get_type_info(cloned);
  auto* original_expr = xls_dslx_constant_def_get_value(original_constant);
  auto* cloned_expr = xls_dslx_constant_def_get_value(cloned_constant);
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(2, 0, &error, &bits));
  absl::Cleanup free_bits([&] { xls_bits_free(bits); });
  xls_dslx_interp_value* value = nullptr;
  xls_dslx_interp_value* const_expr_value = nullptr;
  xls_dslx_interp_value* waiter_value = nullptr;
  absl::Cleanup free_values([&] {
    xls_dslx_interp_value_free(value);
    xls_dslx_interp_value_free(const_expr_value);
    xls_dslx_interp_value_free(waiter_value);
  });
  if (temperature == MetadataTemperature::kWarm) {
    ASSERT_TRUE(xls_dslx_interp_value_make_enum(original_enum, false, bits,
                                                &error, &waiter_value));
    ASSERT_TRUE(
        xls_dslx_interp_value_make_enum(enum_def, false, bits, &error, &value));
    xls_dslx_interp_value_free(waiter_value);
    waiter_value = nullptr;
    xls_dslx_interp_value_free(value);
    value = nullptr;
  }

  absl::Notification parse_entered;
  absl::Notification release_parse;
  absl::Notification waiter_selected;
  absl::Notification metadata_finished;
  std::thread parse_worker;
  std::thread waiter_worker;
  std::thread metadata_worker;
  absl::Cleanup stop_workers([&] {
    release_parse.Notify();
    if (parse_worker.joinable()) {
      parse_worker.join();
    }
    if (waiter_worker.joinable()) {
      waiter_worker.join();
    }
    if (metadata_worker.joinable()) {
      metadata_worker.join();
    }
    xls::SetDslxImporterStackObserverForTesting(source, {});
    xls::SetDslxMetadataLockObserverForTesting(source, {});
  });
  xls::SetDslxImporterStackObserverForTesting(
      source, [&](const xls::dslx::Span&, const std::filesystem::path& path) {
        if (path.filename() == "busy_parse.x") {
          parse_entered.Notify();
          release_parse.WaitForNotification();
        }
      });
  xls::SetDslxMetadataLockObserverForTesting(source,
                                             [&] { waiter_selected.Notify(); });
  bool parse_ok = false;
  bool metadata_ok = false;
  bool const_expr_ok = false;
  bool waiter_ok = waiter == MetadataWaiter::kNone;
  parse_worker = std::thread([&] {
    parse_ok = xls_dslx_parse_and_typecheck("fn identity(x: u8) -> u8 { x }",
                                            "busy_parse.x", "busy_parse",
                                            source, &parse_error, &parsed);
  });
  const bool parse_is_held =
      parse_entered.WaitForNotificationWithTimeout(absl::Seconds(10));
  bool waiter_is_selected = waiter == MetadataWaiter::kNone;
  if (parse_is_held && waiter != MetadataWaiter::kNone) {
    waiter_worker = std::thread([&] {
      if (waiter == MetadataWaiter::kEnum) {
        waiter_ok = xls_dslx_interp_value_make_enum(
            original_enum, false, bits, &waiter_error, &waiter_value);
      } else {
        waiter_ok = xls_dslx_type_info_get_const_expr(
            original_type_info, original_expr, &waiter_error, &waiter_value);
      }
    });
    waiter_is_selected =
        waiter_selected.WaitForNotificationWithTimeout(absl::Seconds(10));
  }
  bool completed_before_release = false;
  if (parse_is_held && waiter_is_selected) {
    metadata_worker = std::thread([&] {
      metadata_ok = xls_dslx_interp_value_make_enum(enum_def, false, bits,
                                                    &enum_error, &value);
      const_expr_ok = xls_dslx_type_info_get_const_expr(
          cloned_type_info, cloned_expr, &const_expr_error, &const_expr_value);
      metadata_finished.Notify();
    });
    completed_before_release =
        metadata_finished.WaitForNotificationWithTimeout(absl::Seconds(10));
  }
  // Release and join even when the watchdog expires, so baseline failure does
  // not strand workers or free their borrowed C handles.
  std::move(stop_workers).Invoke();
  EXPECT_TRUE(parse_is_held);
  EXPECT_TRUE(waiter_is_selected);
  EXPECT_TRUE(completed_before_release);
  EXPECT_TRUE(parse_ok) << parse_error;
  EXPECT_TRUE(waiter_ok) << waiter_error;
  EXPECT_TRUE(metadata_ok) << enum_error;
  EXPECT_TRUE(const_expr_ok) << const_expr_error;
  EXPECT_NE(value, nullptr);
  EXPECT_NE(const_expr_value, nullptr);
  if (waiter != MetadataWaiter::kNone) {
    EXPECT_NE(waiter_value, nullptr);
  }
}

TEST(XlsCApiTest, EnumMetadataDoesNotWaitForUnrelatedParse) {
  for (auto temperature :
       {MetadataTemperature::kCold, MetadataTemperature::kWarm}) {
    ExpectMetadataDoesNotWaitForUnrelatedParse(MetadataWaiter::kNone,
                                               temperature);
  }
}

TEST(XlsCApiTest, EnumMetadataDoesNotWaitForBlockedEnumMetadata) {
  for (auto temperature :
       {MetadataTemperature::kCold, MetadataTemperature::kWarm}) {
    ExpectMetadataDoesNotWaitForUnrelatedParse(MetadataWaiter::kEnum,
                                               temperature);
  }
}

TEST(XlsCApiTest, EnumMetadataDoesNotWaitForBlockedConstExprMetadata) {
  for (auto temperature :
       {MetadataTemperature::kCold, MetadataTemperature::kWarm}) {
    ExpectMetadataDoesNotWaitForUnrelatedParse(MetadataWaiter::kConstExpr,
                                               temperature);
  }
}

// Negative test: checks error handling for cross-module sum assignments.
TEST(XlsCApiTest, DslxClonedModulesHaveDistinctSumIdentity) {
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* original = nullptr;
  xls_dslx_typechecked_module* cloned = nullptr;
  xls_dslx_typechecked_module* consumer = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_typechecked_module_free(consumer);
    xls_dslx_typechecked_module_free(cloned);
    xls_dslx_typechecked_module_free(original);
    xls_dslx_import_data_free(import_data);
  });
  ASSERT_NE(import_data, nullptr);
  ASSERT_TRUE(xls_dslx_parse_and_typecheck("pub enum S { A(u8), B }", "same.x",
                                           "a", import_data, &error, &original))
      << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(original, nullptr);
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      original, nullptr, 0, "b", import_data, &error, &cloned))
      << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(cloned, nullptr);

  // All owners remain alive. Shared source spans do not equate independently
  // installed sum declarations.
  EXPECT_FALSE(xls_dslx_parse_and_typecheck(
      "import a; import b; fn f(x: a::S) -> b::S { x }", "main.x", "main",
      import_data, &error, &consumer));
  EXPECT_EQ(consumer, nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_THAT(error, HasSubstr("type mismatch"));
}

TEST(XlsCApiTest, DslxSemanticSumArrayIdentityAcrossClonedModuleAndCValues) {
  constexpr const char* kProgram = R"(
enum Maybe { None, Some(u8) }
enum E : u2 { A = 0, B = 1, AliasA = 0 }
enum F : u2 { A = 0, B = 1 }
const SUM: Maybe = Maybe::Some(u8:42);
const PAIR: (Maybe, E) = (SUM, E::A);
const OTHER: (Maybe, F) = (SUM, F::A);
const PAIRS: (Maybe, E)[2] = [PAIR, (SUM, E::B)];
const EMPTY = u8[0]:[];
const SIGNED_EMPTY = s8[0]:[];
const NESTED_EMPTY = ([EMPTY],);
const EMPTY_PAIR = (SUM, EMPTY);
const NESTED_EMPTY_PAIR = (SUM, NESTED_EMPTY);
const SIGNED_EMPTY_PAIR = (SUM, SIGNED_EMPTY);
)";
  xls_dslx_import_data* source = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_import_data* destination = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  absl::Cleanup free_owners([&] {
    xls_dslx_import_data_free(source);
    xls_dslx_import_data_free(destination);
  });
  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* original = nullptr;
  xls_dslx_typechecked_module* cloned = nullptr;
  absl::Cleanup free_modules([&] {
    xls_dslx_typechecked_module_free(original);
    xls_dslx_typechecked_module_free(cloned);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "array_clone.x", "array_clone", source, &error, &original))
      << error;
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      original, nullptr, 0, "array_clone_destination", destination, &error,
      &cloned))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(cloned);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(cloned);
  std::vector<xls_dslx_interp_value*> values;
  absl::Cleanup free_values([&] {
    for (auto* value : values) {
      xls_dslx_interp_value_free(value);
    }
  });
  for (int64_t index : {3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
    auto* constant = xls_dslx_module_member_get_constant_def(
        xls_dslx_module_get_member(module, index));
    ASSERT_NE(constant, nullptr);
    xls_dslx_interp_value* value = nullptr;
    ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
        type_info, xls_dslx_constant_def_get_value(constant), &error, &value));
    values.push_back(value);
  }
  auto* empty_clone = xls_dslx_interp_value_clone(values[4]);
  auto* nested_empty_clone = xls_dslx_interp_value_clone(values[6]);
  ASSERT_NE(empty_clone, nullptr);
  ASSERT_NE(nested_empty_clone, nullptr);
  EXPECT_EQ(xls::GetDslxValueMetadataForTesting(values[4]).lock(),
            xls::GetDslxValueMetadataForTesting(empty_clone).lock());
  EXPECT_EQ(xls::GetDslxValueMetadataForTesting(values[6]).lock(),
            xls::GetDslxValueMetadataForTesting(nested_empty_clone).lock());
  values.push_back(empty_clone);
  values.push_back(nested_empty_clone);
  xls_dslx_interp_value_free(values[4]);
  values[4] = nullptr;
  xls_dslx_interp_value_free(values[6]);
  values[6] = nullptr;
  auto* original_module = xls_dslx_typechecked_module_get_module(original);
  auto* original_constant = xls_dslx_module_member_get_constant_def(
      xls_dslx_module_get_member(original_module, 3));
  xls_dslx_interp_value* original_sum = nullptr;
  ASSERT_TRUE(xls_dslx_type_info_get_const_expr(
      xls_dslx_typechecked_module_get_type_info(original),
      xls_dslx_constant_def_get_value(original_constant), &error,
      &original_sum));
  values.push_back(original_sum);
  auto* enum_def = xls_dslx_module_member_get_enum_def(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(enum_def, nullptr);
  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(2, 1, &error, &bits));
  absl::Cleanup free_bits([&] { xls_bits_free(bits); });
  xls_dslx_interp_value* enum_value = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_enum(enum_def, false, bits, &error,
                                              &enum_value))
      << error;
  values.push_back(enum_value);
  struct RawEnumCase {
    int64_t bit_count;
    uint64_t value;
    bool is_signed;
  };
  std::vector<xls_dslx_interp_value*> untyped_values;
  for (const RawEnumCase& raw :
       {RawEnumCase{2, 2, false}, {3, 0, false}, {2, 0, true}}) {
    xls_bits* raw_bits = nullptr;
    ASSERT_TRUE(
        xls_bits_make_ubits(raw.bit_count, raw.value, &error, &raw_bits));
    absl::Cleanup free_raw_bits([&] { xls_bits_free(raw_bits); });
    xls_dslx_interp_value* raw_enum = nullptr;
    // The existing raw enum constructor remains permissive for ordinary values.
    ASSERT_TRUE(xls_dslx_interp_value_make_enum(enum_def, raw.is_signed,
                                                raw_bits, &error, &raw_enum));
    values.push_back(raw_enum);
    untyped_values.push_back(raw_enum);
    char* ordinary_text = xls_dslx_interp_value_to_string(raw_enum);
    EXPECT_NE(ordinary_text, nullptr);
    xls_c_str_free(ordinary_text);
  }

  // The clone keeps the source FileTable but belongs to destination. Neither
  // FileTable addresses nor borrowed AST pointers may become retained identity.
  xls_dslx_typechecked_module_free(original);
  original = nullptr;
  xls_dslx_typechecked_module_free(cloned);
  cloned = nullptr;
  xls_dslx_import_data_free(source);
  source = nullptr;
  xls_dslx_import_data_free(destination);
  destination = nullptr;

  // Both direct empties and empties below tuple/array layers retain their
  // known type after cloning and teardown of every compilation owner.
  std::vector<xls_dslx_interp_value*> empty_tuples;
  const std::vector<std::pair<xls_dslx_interp_value*, xls_dslx_interp_value*>>
      empty_cases = {{empty_clone, values[7]},
                     {nested_empty_clone, values[8]},
                     {values[5], values[9]}};
  for (const auto& [empty, source_tuple] : empty_cases) {
    xls_dslx_interp_value* elements[] = {values[0], empty};
    xls_dslx_interp_value* tuple = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_make_tuple(2, elements, &error, &tuple))
        << error;
    values.push_back(tuple);
    empty_tuples.push_back(tuple);
    char* text = xls_dslx_interp_value_to_string(tuple);
    char* source_text = xls_dslx_interp_value_to_string(source_tuple);
    EXPECT_STREQ(text, source_text);
    EXPECT_THAT(text, HasSubstr("Maybe::Some(u8:42)"));
    xls_c_str_free(text);
    xls_c_str_free(source_text);

    // Matching text alone cannot prove structural type compatibility.
    xls_dslx_interp_value* compatible[] = {tuple, source_tuple};
    xls_dslx_interp_value* array = nullptr;
    ASSERT_TRUE(xls_dslx_interp_value_make_array(2, compatible, &error, &array))
        << error;
    values.push_back(array);
  }

  // Ordinary C arrays remain permissive and keep their raw stringification.
  // Neither an untyped empty nor incompatible known element types may become
  // a wildcard when a later aggregate contains a sum.
  xls_dslx_interp_value* raw_empty = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_array(0, nullptr, &error, &raw_empty));
  values.push_back(raw_empty);
  untyped_values.push_back(raw_empty);
  char* raw_empty_text = xls_dslx_interp_value_to_string(raw_empty);
  char* typed_empty_text = xls_dslx_interp_value_to_string(empty_clone);
  EXPECT_STREQ(raw_empty_text, "[]");
  EXPECT_STREQ(typed_empty_text, "[]");
  xls_c_str_free(raw_empty_text);
  xls_c_str_free(typed_empty_text);
  xls_dslx_interp_value* mixed_elements[] = {empty_clone, values[5]};
  xls_dslx_interp_value* mixed_array = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_array(2, mixed_elements, &error,
                                               &mixed_array));
  values.push_back(mixed_array);
  untyped_values.push_back(mixed_array);
  char* mixed_text = xls_dslx_interp_value_to_string(mixed_array);
  EXPECT_STREQ(mixed_text, "[[], []]");
  xls_c_str_free(mixed_text);

  // Invalid raw enums and untyped arrays have no DSLX identity. Reject them at
  // sum composition through the existing error channel, not in stringification.
  for (auto* untyped_value : untyped_values) {
    xls_dslx_interp_value* invalid_fields[] = {values[0], untyped_value};
    xls_dslx_interp_value* invalid_tuple = nullptr;
    char* tuple_error = nullptr;
    EXPECT_FALSE(xls_dslx_interp_value_make_tuple(
        2, invalid_fields, &tuple_error, &invalid_tuple));
    EXPECT_EQ(invalid_tuple, nullptr);
    EXPECT_THAT(
        tuple_error,
        HasSubstr("Sum-bearing tuple contains an element with no DSLX type"));
    xls_dslx_interp_value_free(invalid_tuple);
    xls_c_str_free(tuple_error);
  }

  xls_dslx_interp_value* fields[] = {values[0], enum_value};
  xls_dslx_interp_value* c_tuple = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_tuple(2, fields, &error, &c_tuple));
  values.push_back(c_tuple);
  // The first element supplies the array descriptor. Its raw enum was B; that
  // descriptor must also know A's last-declared alias for the next element.
  xls_dslx_interp_value* compatible[] = {c_tuple, values[1]};
  xls_dslx_interp_value* array = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_array(2, compatible, &error, &array))
      << error;
  values.push_back(array);
  xls_dslx_interp_value* nested_elements[] = {array, values[3]};
  xls_dslx_interp_value* nested_array = nullptr;
  ASSERT_TRUE(xls_dslx_interp_value_make_array(2, nested_elements, &error,
                                               &nested_array))
      << error;
  values.push_back(nested_array);
  char* text = xls_dslx_interp_value_to_string(nested_array);
  absl::Cleanup free_text([&] { xls_c_str_free(text); });
  EXPECT_THAT(text, HasSubstr("Maybe::Some(u8:42)"));
  EXPECT_THAT(text, HasSubstr("E::AliasA  // u2:0"));
  EXPECT_THAT(text, HasSubstr("E::B  // u2:1"));

  // Equal storage does not erase enum declaration identity, independent sum
  // owners, or the element type of an empty array.
  const std::vector<std::pair<xls_dslx_interp_value*, xls_dslx_interp_value*>>
      incompatible = {{c_tuple, values[2]},
                      {values[0], original_sum},
                      {empty_tuples[0], empty_tuples[2]}};
  for (const auto& [first, second] : incompatible) {
    xls_dslx_interp_value* elements[] = {first, second};
    xls_dslx_interp_value* rejected = nullptr;
    char* array_error = nullptr;
    EXPECT_FALSE(
        xls_dslx_interp_value_make_array(2, elements, &array_error, &rejected));
    EXPECT_EQ(rejected, nullptr);
    EXPECT_NE(array_error, nullptr);
    xls_dslx_interp_value_free(rejected);
    xls_c_str_free(array_error);
  }
}

TEST(XlsCApiTest, DslxOrdinaryCArraysRetainPermissiveConstruction) {
  auto* byte = xls_dslx_interp_value_make_ubits(8, 7);
  auto* word = xls_dslx_interp_value_make_sbits(16, -1);
  absl::Cleanup free_elements([&] {
    xls_dslx_interp_value_free(byte);
    xls_dslx_interp_value_free(word);
  });
  xls_dslx_interp_value* elements[] = {byte, word};
  xls_dslx_interp_value* array = nullptr;
  char* error = nullptr;
  absl::Cleanup free_result([&] {
    xls_dslx_interp_value_free(array);
    xls_c_str_free(error);
  });
  ASSERT_TRUE(xls_dslx_interp_value_make_array(2, elements, &error, &array));
  char* text = xls_dslx_interp_value_to_string(array);
  absl::Cleanup free_text([&] { xls_c_str_free(text); });
  EXPECT_STREQ(text, "[u8:7, s16:-1]");
}

TEST(XlsCApiTest, DslxCloneTypecheckedModuleRemovingMembersSuccess) {
  const std::string_view kProgram = R"(
fn helper(x: u32) -> u32 {
    x + u32:1
}

fn unused() -> u32 {
    helper(u32:41)
}

fn main(x: u32) -> u32 {
    x
}
)";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.data(), "<test>", "top",
                                         import_data, &error, &tm);
  ASSERT_TRUE(ok) << "error: " << error;
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  auto find_function_member =
      [&](std::string_view target) -> xls_dslx_module_member* {
    int64_t member_count = xls_dslx_module_get_member_count(module);
    for (int64_t i = 0; i < member_count; ++i) {
      xls_dslx_module_member* member = xls_dslx_module_get_member(module, i);
      xls_dslx_function* fn = xls_dslx_module_member_get_function(member);
      if (fn == nullptr) {
        continue;
      }
      char* identifier = xls_dslx_function_get_identifier(fn);
      absl::Cleanup free_identifier([&] { xls_c_str_free(identifier); });
      if (std::string_view{identifier} == target) {
        return member;
      }
    }
    return nullptr;
  };

  xls_dslx_module_member* unused_member = find_function_member("unused");
  ASSERT_NE(unused_member, nullptr);
  xls_dslx_function* unused_fn =
      xls_dslx_module_member_get_function(unused_member);
  ASSERT_NE(unused_fn, nullptr);
  EXPECT_EQ(xls_dslx_module_member_from_function(unused_fn), unused_member);

  xls_dslx_module_member* removed[] = {unused_member};
  xls_dslx_typechecked_module* cloned_tm = nullptr;
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      tm, removed, std::size(removed), "top_clone_members", import_data, &error,
      &cloned_tm));
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_cloned_tm(
      [=] { xls_dslx_typechecked_module_free(cloned_tm); });

  xls_dslx_module* cloned_module =
      xls_dslx_typechecked_module_get_module(cloned_tm);
  EXPECT_EQ(xls_dslx_module_get_member_count(cloned_module), 2);
  xls_dslx_module_member* first_member =
      xls_dslx_module_get_member(cloned_module, 0);
  xls_dslx_function* helper_fn =
      xls_dslx_module_member_get_function(first_member);
  ASSERT_NE(helper_fn, nullptr);
  EXPECT_EQ(xls_dslx_module_member_from_function(helper_fn), first_member);
  char* helper_name = xls_dslx_function_get_identifier(helper_fn);
  absl::Cleanup free_helper_name([&] { xls_c_str_free(helper_name); });
  EXPECT_EQ(std::string_view{helper_name}, "helper");
  char* module_name = xls_dslx_module_get_name(cloned_module);
  absl::Cleanup free_module_name([&] { xls_c_str_free(module_name); });
  EXPECT_EQ(std::string_view{module_name}, "top_clone_members");

  xls_dslx_function* removed_functions[] = {unused_fn};
  xls_dslx_typechecked_module* cloned_tm_functions = nullptr;
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_functions(
      tm, removed_functions, std::size(removed_functions),
      "top_clone_functions", import_data, &error, &cloned_tm_functions));
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_cloned_tm_functions(
      [=] { xls_dslx_typechecked_module_free(cloned_tm_functions); });

  xls_dslx_module* cloned_module_functions =
      xls_dslx_typechecked_module_get_module(cloned_tm_functions);
  EXPECT_EQ(xls_dslx_module_get_member_count(cloned_module_functions), 2);
  xls_dslx_module_member* first_member_functions =
      xls_dslx_module_get_member(cloned_module_functions, 0);
  xls_dslx_function* helper_fn_functions =
      xls_dslx_module_member_get_function(first_member_functions);
  ASSERT_NE(helper_fn_functions, nullptr);
  char* helper_name_functions =
      xls_dslx_function_get_identifier(helper_fn_functions);
  absl::Cleanup free_helper_name_functions(
      [&] { xls_c_str_free(helper_name_functions); });
  EXPECT_EQ(std::string_view{helper_name_functions}, "helper");
  char* module_name_functions =
      xls_dslx_module_get_name(cloned_module_functions);
  absl::Cleanup free_module_name_functions(
      [&] { xls_c_str_free(module_name_functions); });
  EXPECT_EQ(std::string_view{module_name_functions}, "top_clone_functions");
}

TEST(XlsCApiTest, DslxCloneTypecheckedModuleRemovingMembersFailure) {
  const std::string_view kProgram = R"(
fn helper(x: u32) -> u32 {
    x + u32:1
}

fn main(x: u32) -> u32 {
    helper(x)
}
)";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.data(), "<test>", "top",
                                         import_data, &error, &tm);
  ASSERT_TRUE(ok) << "error: " << error;
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  auto find_function = [&](std::string_view target) -> xls_dslx_function* {
    int64_t member_count = xls_dslx_module_get_member_count(module);
    for (int64_t i = 0; i < member_count; ++i) {
      xls_dslx_module_member* member = xls_dslx_module_get_member(module, i);
      xls_dslx_function* fn = xls_dslx_module_member_get_function(member);
      if (fn == nullptr) {
        continue;
      }
      char* identifier = xls_dslx_function_get_identifier(fn);
      absl::Cleanup free_identifier([&] { xls_c_str_free(identifier); });
      if (std::string_view{identifier} == target) {
        return fn;
      }
    }
    return nullptr;
  };

  xls_dslx_function* helper_fn = find_function("helper");
  ASSERT_NE(helper_fn, nullptr);

  xls_dslx_function* removed[] = {helper_fn};
  xls_dslx_typechecked_module* cloned_tm = nullptr;
  EXPECT_FALSE(xls_dslx_typechecked_module_clone_removing_functions(
      tm, removed, std::size(removed), "top", import_data, &error, &cloned_tm));
  EXPECT_NE(error, nullptr);
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  EXPECT_THAT(error, HasSubstr("helper"));
  EXPECT_EQ(cloned_tm, nullptr);
}

// Pruning through the C API re-typechecks a cloned module. This previously
// threw std::bad_optional_access for imported parametric structs because the
// clone had lost the module span needed to instantiate their parametrics.
TEST(XlsCApiTest, DslxCloneRetypesImportedParametricStruct) {
  constexpr char kImported[] = R"(
pub struct Pair<A: u32, B: u32> {
  first: uN[A],
  second: uN[B],
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(xls::TempDirectory tempdir,
                           xls::TempDirectory::Create());
  XLS_ASSERT_OK(xls::SetFileContents(tempdir.path() / "imported.x", kImported));

  constexpr char kProgram[] = R"(
import imported;

pub fn unused(x: u4) -> u4 { x }

pub fn identity(x: imported::Pair<u32:2, u32:1>)
    -> imported::Pair<u32:2, u32:1> { x }
)";
  const char* search_paths[] = {tempdir.path().c_str()};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), search_paths,
      std::size(search_paths));
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  absl::Cleanup free_error([&] { xls_c_str_free(error); });
  xls_dslx_typechecked_module* tm = nullptr;
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "main.x", "main",
                                           import_data, &error, &tm))
      << (error == nullptr ? "" : error);
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_EQ(xls_dslx_module_get_member_count(module), 3);
  xls_dslx_module_member* unused = xls_dslx_module_get_member(module, 1);
  xls_dslx_function* unused_fn = xls_dslx_module_member_get_function(unused);
  ASSERT_NE(unused_fn, nullptr);
  char* unused_name = xls_dslx_function_get_identifier(unused_fn);
  absl::Cleanup free_unused_name([&] { xls_c_str_free(unused_name); });
  ASSERT_EQ(std::string_view{unused_name}, "unused");

  xls_dslx_module_member* removed[] = {unused};
  xls_dslx_typechecked_module* pruned = nullptr;
  absl::Cleanup free_pruned([&] { xls_dslx_typechecked_module_free(pruned); });
  ASSERT_TRUE(xls_dslx_typechecked_module_clone_removing_members(
      tm, removed, std::size(removed), "main_pruned", import_data, &error,
      &pruned))
      << (error == nullptr ? "" : error);
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(pruned, nullptr);

  xls_dslx_module* pruned_module =
      xls_dslx_typechecked_module_get_module(pruned);
  ASSERT_EQ(xls_dslx_module_get_member_count(pruned_module), 2);
  xls_dslx_module_member* identity =
      xls_dslx_module_get_member(pruned_module, 1);
  xls_dslx_function* identity_fn =
      xls_dslx_module_member_get_function(identity);
  ASSERT_NE(identity_fn, nullptr);
  char* identity_name = xls_dslx_function_get_identifier(identity_fn);
  absl::Cleanup free_identity_name([&] { xls_c_str_free(identity_name); });
  EXPECT_EQ(std::string_view{identity_name}, "identity");
}

TEST(XlsCApiTest, ValueGetElementCount) {
  const std::initializer_list<
      std::pair<const char*, std::variant<int64_t, std::string_view>>>
      kTestCases = {
          {"())", 0},
          {"(bits[32]:42)", 1},
          {"(bits[32]:42, bits[32]:43)", 2},
          // Arrays
          {"[bits[32]:42]", 1},
          {"[bits[32]:42, bits[32]:43]", 2},
          // Errors
          {"bits[32]:42", "no element count"},
      };
  for (const auto& [input, expected] : kTestCases) {
    xls_value* value = nullptr;
    char* error = nullptr;
    absl::Cleanup free_error([&] { xls_c_str_free(error); });
    ASSERT_TRUE(xls_parse_typed_value(input, &error, &value));
    absl::Cleanup free_value([&] { xls_value_free(value); });

    int64_t element_count = 0;
    bool success = xls_value_get_element_count(value, &error, &element_count);
    ASSERT_EQ(success, std::holds_alternative<int64_t>(expected));
    if (std::holds_alternative<int64_t>(expected)) {
      EXPECT_EQ(element_count, std::get<int64_t>(expected));
    } else {
      EXPECT_THAT(error, HasSubstr(std::get<std::string_view>(expected)));
    }
  }
}

// In the `_owned` variation of the API we don't need to free the bits value.
TEST(XlsCApiTest, ValueFromBitsOwned) {
  xls_bits* bits = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(32, 42, &error, &bits));

  {
    char* bits_str = xls_bits_to_debug_string(bits);
    absl::Cleanup free_bits_str([=] { xls_c_str_free(bits_str); });
    EXPECT_EQ(std::string_view{bits_str}, "0b00000000000000000000000000101010");
  }

  xls_value* value = xls_value_from_bits_owned(bits);
  absl::Cleanup free_value([=] { xls_value_free(value); });

  {
    char* value_str = nullptr;
    ASSERT_TRUE(xls_value_to_string(value, &value_str));
    absl::Cleanup free_value_str([=] { xls_c_str_free(value_str); });
    EXPECT_EQ(std::string_view{value_str}, "bits[32]:42");
  }
}

TEST(XlsCApiTest, ValueFromBitsUnowned) {
  xls_bits* bits = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(32, 42, &error, &bits));
  absl::Cleanup free_bits([=] { xls_bits_free(bits); });

  // We'll create two values from this one bits object to try to show its guts
  // are not moved or corrupted in some way.
  xls_value* value1 = xls_value_from_bits(bits);
  absl::Cleanup free_value1([=] { xls_value_free(value1); });
  xls_value* value2 = xls_value_from_bits(bits);
  absl::Cleanup free_value2([=] { xls_value_free(value2); });

  EXPECT_TRUE(xls_value_eq(value1, value2));
  EXPECT_TRUE(xls_value_eq(value2, value1));

  // Check that the bits object can be turned to string still.
  char* bits_str = xls_bits_to_debug_string(bits);
  absl::Cleanup free_bits_str([=] { xls_c_str_free(bits_str); });
  EXPECT_EQ(std::string_view{bits_str}, "0b00000000000000000000000000101010");
}

TEST(XlsCApiTest, FunctionJit) {
  const std::string_view kIr = R"(package my_package

top fn add_one(tok: token, x: bits[32]) -> bits[32] {
  one: bits[32] = literal(value=1)
  add: bits[32] = add(x, one)
  always_on: bits[1] = literal(value=1)
  trace: token = trace(tok, always_on, format="result: {}", data_operands=[add])
  ret result: bits[32] =identity(add)
}
)";
  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kIr.data(), "test.ir", &error, &package))
      << "error: " << error;
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "add_one", &error, &function));
  ASSERT_NE(function, nullptr);

  xls_function_jit* fn_jit = nullptr;
  ASSERT_TRUE(xls_make_function_jit(function, &error, &fn_jit));
  ASSERT_NE(fn_jit, nullptr);
  absl::Cleanup free_fn_jit([=] { xls_function_jit_free(fn_jit); });

  xls_bits* mol_bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(32, 42, &error, &mol_bits));
  xls_value* mol = xls_value_from_bits_owned(mol_bits);
  absl::Cleanup free_mol([=] { xls_value_free(mol); });

  xls_value* tok = xls_value_make_token();
  absl::Cleanup free_tok([=] { xls_value_free(tok); });

  std::vector<xls_value*> args = {tok, mol};
  xls_value* result = nullptr;
  xls_trace_message* trace_messages = nullptr;
  size_t trace_messages_count = 0;
  char** assert_messages = nullptr;
  size_t assert_messages_count = 0;
  ASSERT_TRUE(xls_function_jit_run(fn_jit, args.size(), args.data(), &error,
                                   &trace_messages, &trace_messages_count,
                                   &assert_messages, &assert_messages_count,
                                   &result));
  absl::Cleanup free_result([=] { xls_value_free(result); });
  absl::Cleanup free_trace_messages(
      [=] { xls_trace_messages_free(trace_messages, trace_messages_count); });
  absl::Cleanup free_assert_messages(
      [=] { xls_c_strs_free(assert_messages, assert_messages_count); });

  ASSERT_EQ(trace_messages_count, 1);
  ASSERT_EQ(assert_messages_count, 0);
  EXPECT_EQ(std::string_view{trace_messages[0].message}, "result: 43");
  EXPECT_EQ(trace_messages[0].verbosity, 0);

  char* result_str = nullptr;
  ASSERT_TRUE(xls_value_to_string(result, &result_str));
  absl::Cleanup free_result_str([=] { xls_c_str_free(result_str); });
  EXPECT_EQ(std::string_view{result_str}, "bits[32]:43");
}

TEST(XlsCApiTest, AotCompileFunction) {
  const std::string_view kIr = R"(package my_package

fn noisy_sibling(tok: token, x: bits[32]) -> bits[32] {
  always_on: bits[1] = literal(value=1)
  traced_tok: token = trace(tok, always_on, format="x: {}", data_operands=[x])
  ret out: bits[32] = identity(x)
}

top fn add_one(x: bits[32]) -> bits[32] {
  one: bits[32] = literal(value=1)
  ret result: bits[32] = add(x, one)
}
)";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kIr.data(), "test.ir", &error, &package))
      << "error: " << error;
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "add_one", &error, &function));
  ASSERT_NE(function, nullptr);

  uint8_t* object_bytes = nullptr;
  size_t object_byte_count = 0;
  uint8_t* proto_bytes = nullptr;
  size_t proto_byte_count = 0;
  ASSERT_TRUE(xls_aot_compile_function(function, &error, &object_bytes,
                                       &object_byte_count, &proto_bytes,
                                       &proto_byte_count))
      << "error: " << error;
  ASSERT_GT(object_byte_count, 0);
  absl::Cleanup free_object_bytes(
      [=] { xls_aot_object_code_free(object_bytes); });
  ASSERT_GT(proto_byte_count, 0);
  absl::Cleanup free_proto_bytes(
      [=] { xls_aot_entrypoints_proto_free(proto_bytes); });

  xls::AotPackageEntrypointsProto entrypoints;
  ASSERT_TRUE(entrypoints.ParseFromArray(proto_bytes, proto_byte_count));
  ASSERT_EQ(entrypoints.entrypoint_size(), 1);
  EXPECT_EQ(entrypoints.entrypoint(0).type(),
            xls::AotEntrypointProto::FUNCTION);
  EXPECT_TRUE(entrypoints.entrypoint(0).has_function_symbol());
  EXPECT_TRUE(entrypoints.entrypoint(0).has_packed_function_symbol());
  ASSERT_TRUE(
      entrypoints.entrypoint(0).has_standalone_runtime_feature_requirements());
  EXPECT_EQ(entrypoints.entrypoint(0)
                .standalone_runtime_feature_requirements()
                .required_feature_size(),
            0);
}

TEST(XlsCApiTest, AotCompileFunctionRecordsStandaloneRuntimeFeatures) {
  const std::string_view kIr = R"(package feature_pkg

fn featureful(tok: token, pred: bits[1], x: bits[8]) -> bits[8] {
  asserted_tok: token = assert(tok, pred, message="pred must hold")
  always_on: bits[1] = literal(value=1)
  traced_tok: token = trace(asserted_tok, always_on, format="x: {}", data_operands=[x])
  covered: () = cover(pred, label="pred_is_one")
  ret out: bits[8] = identity(x)
}

top fn calls_featureful(tok: token, pred: bits[1], x: bits[8]) -> bits[8] {
  ret out: bits[8] = invoke(tok, pred, x, to_apply=featureful)
}
)";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kIr.data(), "test.ir", &error, &package))
      << "error: " << error;
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_function* function = nullptr;
  ASSERT_TRUE(
      xls_package_get_function(package, "calls_featureful", &error, &function));
  ASSERT_NE(function, nullptr);

  uint8_t* object_bytes = nullptr;
  size_t object_byte_count = 0;
  uint8_t* proto_bytes = nullptr;
  size_t proto_byte_count = 0;
  ASSERT_TRUE(xls_aot_compile_function(function, &error, &object_bytes,
                                       &object_byte_count, &proto_bytes,
                                       &proto_byte_count))
      << "error: " << error;
  ASSERT_GT(object_byte_count, 0);
  absl::Cleanup free_object_bytes(
      [=] { xls_aot_object_code_free(object_bytes); });
  ASSERT_GT(proto_byte_count, 0);
  absl::Cleanup free_proto_bytes(
      [=] { xls_aot_entrypoints_proto_free(proto_bytes); });

  xls::AotPackageEntrypointsProto entrypoints;
  ASSERT_TRUE(entrypoints.ParseFromArray(proto_bytes, proto_byte_count));
  ASSERT_EQ(entrypoints.entrypoint_size(), 1);
  ASSERT_TRUE(
      entrypoints.entrypoint(0).has_standalone_runtime_feature_requirements());
  EXPECT_THAT(entrypoints.entrypoint(0)
                  .standalone_runtime_feature_requirements()
                  .required_feature(),
              ElementsAre(xls::AotRuntimeFeatureRequirementsProto::ASSERTIONS,
                          xls::AotRuntimeFeatureRequirementsProto::TRACES,
                          xls::AotRuntimeFeatureRequirementsProto::COVERS));
}

TEST(XlsCApiTest, AotEntrypointTrampoline) {
  const std::string_view kIr = R"(package my_package

top fn add_one(x: bits[8]) -> bits[8] {
  one: bits[8] = literal(value=1)
  ret result: bits[8] = add(x, one)
}
)";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kIr.data(), "test.ir", &error, &package))
      << "error: " << error;
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([=] { xls_package_free(package); });
  absl::Cleanup free_error([&] { xls_c_str_free(error); });

  xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "add_one", &error, &function));
  ASSERT_NE(function, nullptr);

  uint8_t* object_bytes = nullptr;
  size_t object_byte_count = 0;
  uint8_t* proto_bytes = nullptr;
  size_t proto_byte_count = 0;
  ASSERT_TRUE(xls_aot_compile_function(function, &error, &object_bytes,
                                       &object_byte_count, &proto_bytes,
                                       &proto_byte_count))
      << "error: " << error;
  ASSERT_GT(object_byte_count, 0);
  absl::Cleanup free_object_bytes(
      [=] { xls_aot_object_code_free(object_bytes); });
  ASSERT_GT(proto_byte_count, 0);
  absl::Cleanup free_proto_bytes(
      [=] { xls_aot_entrypoints_proto_free(proto_bytes); });

  xls::AotPackageEntrypointsProto entrypoints;
  ASSERT_TRUE(entrypoints.ParseFromArray(proto_bytes, proto_byte_count));
  ASSERT_EQ(entrypoints.entrypoint_size(), 1);
  const xls::AotEntrypointProto& entrypoint = entrypoints.entrypoint(0);
  ASSERT_TRUE(entrypoint.has_packed_function_symbol());

  llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> maybe_jit =
      llvm::orc::LLJITBuilder().create();
  ASSERT_TRUE(static_cast<bool>(maybe_jit))
      << llvm::toString(maybe_jit.takeError());
  std::unique_ptr<llvm::orc::LLJIT> jit = std::move(*maybe_jit);
  jit->getMainJITDylib().addGenerator(llvm::cantFail(
      llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          jit->getDataLayout().getGlobalPrefix())));

  auto object_buffer = llvm::MemoryBuffer::getMemBufferCopy(
      llvm::StringRef(reinterpret_cast<const char*>(object_bytes),
                      object_byte_count),
      "xls_aot_object_code");
  llvm::Error add_object_error = jit->addObjectFile(std::move(object_buffer));
  ASSERT_FALSE(static_cast<bool>(add_object_error))
      << llvm::toString(std::move(add_object_error));

  std::string packed_symbol = entrypoint.packed_function_symbol();
  llvm::Expected<llvm::orc::ExecutorAddr> maybe_packed_address =
      jit->lookup(packed_symbol);
  ASSERT_TRUE(static_cast<bool>(maybe_packed_address))
      << llvm::toString(maybe_packed_address.takeError());
  llvm::orc::ExecutorAddr packed_address = *maybe_packed_address;

  xls_aot_exec_context* context = nullptr;
  ASSERT_TRUE(xls_aot_exec_context_create(proto_bytes, proto_byte_count, &error,
                                          &context))
      << "error: " << error;
  ASSERT_NE(context, nullptr);
  absl::Cleanup free_context([=] { xls_aot_exec_context_free(context); });

  uint8_t input = 41;
  uint8_t output = 0;
  const uint8_t* inputs[1] = {&input};
  uint8_t* outputs[1] = {&output};

  int64_t alignment = entrypoint.temp_buffer_alignment();
  if (alignment < 1) {
    alignment = 1;
  }
  int64_t size = entrypoint.temp_buffer_size();
  if (size < 1) {
    size = 1;
  }
  void* temp_buffer = xls::AllocateAligned(alignment, size);
  ASSERT_NE(temp_buffer, nullptr);
  absl::Cleanup free_temp_buffer([=] { std::free(temp_buffer); });

  size_t trace_messages_count = 0;
  size_t assert_messages_count = 0;
  int64_t continuation = xls_aot_entrypoint_trampoline(
      packed_address.toPtr<void*>(), inputs, outputs, temp_buffer, context,
      /*continuation_point=*/0, &trace_messages_count, &assert_messages_count);

  EXPECT_EQ(continuation, 0);
  EXPECT_EQ(output, 42);
  EXPECT_EQ(trace_messages_count, 0);
  EXPECT_EQ(assert_messages_count, 0);
  xls_aot_exec_context_clear_events(context);
}

// Tests that we can build a simple sample function. For fun we make one that
// corresponds to an AOI21 gate.
//
// AOI21 formula is `fn aoi21(a, b, c) { !((a & b) | c) }`
//
// Just to test we can also handle tuple types we replicate the bit to be a
// member of the result tuple 2x.
TEST(XlsCApiTest, FnBuilder) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  // Note: this is tied to the package lifetime.
  xls_type* u1 = xls_package_get_bits_type(package, 1);
  xls_type* tuple_members[] = {u1, u1, u1};
  xls_type* tuple_u1_u1_u1 =
      xls_package_get_tuple_type(package, tuple_members, 3);

  const char kFunctionName[] = "aoi21";
  xls_function_builder* fn_builder = xls_function_builder_create(
      kFunctionName, package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  // This value aliases the `fn_builder` so it does not need to be freed.
  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* t =
      xls_function_builder_add_parameter(fn_builder, "inputs", tuple_u1_u1_u1);
  bvalues_to_free.push_back(t);

  xls_bvalue* a = xls_builder_base_add_tuple_index(fn_builder_base, t, 0, "a");
  bvalues_to_free.push_back(a);

  xls_bvalue* b = xls_builder_base_add_tuple_index(fn_builder_base, t, 1, "b");
  bvalues_to_free.push_back(b);

  xls_bvalue* c = xls_builder_base_add_tuple_index(fn_builder_base, t, 2, "c");
  bvalues_to_free.push_back(c);

  // Show passing nullptr for the name.
  xls_bvalue* a_and_b =
      xls_builder_base_add_and(fn_builder_base, a, b, /*name=*/nullptr);
  bvalues_to_free.push_back(a_and_b);

  xls_bvalue* a_and_b_or_c =
      xls_builder_base_add_or(fn_builder_base, a_and_b, c, "a_and_b_or_c");
  bvalues_to_free.push_back(a_and_b_or_c);

  xls_bvalue* not_a_and_b_or_c = xls_builder_base_add_not(
      fn_builder_base, a_and_b_or_c, "not_a_and_b_or_c");
  bvalues_to_free.push_back(not_a_and_b_or_c);

  xls_bvalue* tuple_operands[] = {not_a_and_b_or_c, not_a_and_b_or_c};
  xls_bvalue* result =
      xls_builder_base_add_tuple(fn_builder_base, tuple_operands, 2, "result");
  bvalues_to_free.push_back(result);

  xls_function* function = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                             &error, &function))
        << "error: " << error;
    ASSERT_NE(function, nullptr);
    // Note: the built function is placed in the package's lifetime and so there
    // is no need to free it.
  }

  // Mark this function we built as the package top.
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_package_set_top_by_name(package, kFunctionName, &error));
    ASSERT_EQ(error, nullptr);
  }

  // Convert the package to string and make sure it's what we expect the
  // contents are.
  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

top fn aoi21(inputs: (bits[1], bits[1], bits[1]) id=1) -> (bits[1], bits[1]) {
  a: bits[1] = tuple_index(inputs, index=0, id=2)
  b: bits[1] = tuple_index(inputs, index=1, id=3)
  and.5: bits[1] = and(a, b, id=5)
  c: bits[1] = tuple_index(inputs, index=2, id=4)
  a_and_b_or_c: bits[1] = or(and.5, c, id=6)
  not_a_and_b_or_c: bits[1] = not(a_and_b_or_c, id=7)
  ret result: (bits[1], bits[1]) = tuple(not_a_and_b_or_c, not_a_and_b_or_c, id=8)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);
}

TEST(XlsCApiTest, VerifyPackageOk) {
  xls_package* package = xls_package_create("verify_ok");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u1 = xls_package_get_bits_type(package, 1);
  xls_function_builder* fb =
      xls_function_builder_create("id1", package, /*should_verify=*/true);
  absl::Cleanup free_fb([=] { xls_function_builder_free(fb); });
  xls_builder_base* b = xls_function_builder_as_builder_base(fb);
  xls_bvalue* x = xls_function_builder_add_parameter(fb, "x", u1);
  absl::Cleanup free_x([=] { xls_bvalue_free(x); });
  xls_bvalue* ret = xls_builder_base_add_identity(b, x, "ret");
  absl::Cleanup free_ret([=] { xls_bvalue_free(ret); });
  xls_function* fn = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(
        xls_function_builder_build_with_return_value(fb, ret, &error, &fn))
        << "error: " << (error == nullptr ? "<none>" : error);
  }
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_verify_package(package, &error))
        << "error: " << (error == nullptr ? "<none>" : error);
    ASSERT_EQ(error, nullptr);
  }
}

TEST(XlsCApiTest, VerifyPackageDuplicateFunctionNameFails) {
  xls_package* package = xls_package_create("verify_dup");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u1 = xls_package_get_bits_type(package, 1);
  // Build two functions with the same name; skip verification on build so that
  // package-level verify detects the duplicate.
  for (int i = 0; i < 2; ++i) {
    xls_function_builder* fb =
        xls_function_builder_create("dup", package, /*should_verify=*/false);
    absl::Cleanup free_fb([=] { xls_function_builder_free(fb); });
    xls_builder_base* b = xls_function_builder_as_builder_base(fb);
    xls_bvalue* x = xls_function_builder_add_parameter(fb, "x", u1);
    absl::Cleanup free_x([=] { xls_bvalue_free(x); });
    xls_bvalue* ret = xls_builder_base_add_identity(b, x, "ret");
    absl::Cleanup free_ret([=] { xls_bvalue_free(ret); });
    xls_function* fn = nullptr;
    char* error = nullptr;
    ASSERT_TRUE(
        xls_function_builder_build_with_return_value(fb, ret, &error, &fn))
        << "error: " << (error == nullptr ? "<none>" : error);
  }

  char* error = nullptr;
  EXPECT_FALSE(xls_verify_package(package, &error));
  ASSERT_NE(error, nullptr);
  {
    const std::string_view got(error);
    const std::string_view want_suffix =
        "Function/proc/block with name dup is not unique within package "
        "verify_dup";
    EXPECT_NE(got.find(want_suffix), std::string_view::npos) << got;
  }
  xls_c_str_free(error);
}

TEST(XlsCApiTest, FnBuilderConcatAndSlice) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_function_builder* fn_builder = xls_function_builder_create(
      "concat_and_slice", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  // Concat two 16 bit values and slice out the last 8 bits statically and some
  // other 8 bits dynamically.
  xls_type* u16 = xls_package_get_bits_type(package, 16);

  xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u16);
  absl::Cleanup free_x([=] { xls_bvalue_free(x); });
  xls_bvalue* y = xls_function_builder_add_parameter(fn_builder, "y", u16);
  absl::Cleanup free_y([=] { xls_bvalue_free(y); });

  xls_bvalue* concat_operands[] = {x, y};
  xls_bvalue* concat = xls_builder_base_add_concat(
      fn_builder_base, concat_operands, 2, "concat");
  absl::Cleanup free_concat([=] { xls_bvalue_free(concat); });

  xls_bvalue* last_8b = xls_builder_base_add_bit_slice(fn_builder_base, concat,
                                                       32 - 8, 8, "last_8b");
  absl::Cleanup free_last_8b([=] { xls_bvalue_free(last_8b); });

  xls_value* dynamic_start_value = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_value_make_ubits(32, 0, &error, &dynamic_start_value));
  }
  absl::Cleanup free_dynamic_start_value(
      [=] { xls_value_free(dynamic_start_value); });

  xls_bvalue* dynamic_start = xls_builder_base_add_literal(
      fn_builder_base, dynamic_start_value, "dynamic_start");
  absl::Cleanup free_dynamic_start([=] { xls_bvalue_free(dynamic_start); });

  xls_bvalue* dynamic_slice = xls_builder_base_add_dynamic_bit_slice(
      fn_builder_base, concat, dynamic_start, 8, "dynamic_slice");
  absl::Cleanup free_dynamic_slice([=] { xls_bvalue_free(dynamic_slice); });

  xls_bvalue* slices_members[] = {last_8b, dynamic_slice};
  xls_bvalue* slices =
      xls_builder_base_add_tuple(fn_builder_base, slices_members, 2, "slices");
  absl::Cleanup free_slices([=] { xls_bvalue_free(slices); });

  xls_function* function = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(
        fn_builder, slices, &error, &function));
    ASSERT_NE(function, nullptr);
  }

  // Convert the package to string and make sure it's what we expect the
  // contents are.
  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

fn concat_and_slice(x: bits[16] id=1, y: bits[16] id=2) -> (bits[8], bits[8]) {
  concat: bits[32] = concat(x, y, id=3)
  dynamic_start: bits[32] = literal(value=0, id=5)
  last_8b: bits[8] = bit_slice(concat, start=24, width=8, id=4)
  dynamic_slice: bits[8] = dynamic_bit_slice(concat, dynamic_start, width=8, id=6)
  ret slices: (bits[8], bits[8]) = tuple(last_8b, dynamic_slice, id=7)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);
}

TEST(XlsCApiTest, FnBuilderBinops) {
  struct TestCase {
    std::string_view op_name;
    bool is_comparison;
    std::function<xls_bvalue*(xls_builder_base*, xls_bvalue*, xls_bvalue*,
                              const char*)>
        add_op;
  };
  const std::vector<TestCase> kBinops = {
      {"add", false, xls_builder_base_add_add},    // +
      {"umul", false, xls_builder_base_add_umul},  // *
      {"smul", false, xls_builder_base_add_smul},  // *
      {"sub", false, xls_builder_base_add_sub},    // -
      {"and", false, xls_builder_base_add_and},    // &
      {"nand", false, xls_builder_base_add_nand},  // !&
      {"or", false, xls_builder_base_add_or},      // |
      {"xor", false, xls_builder_base_add_xor},    // ^
      {"eq", true, xls_builder_base_add_eq},       // ==
      {"ne", true, xls_builder_base_add_ne},       // !=
      {"ult", true, xls_builder_base_add_ult},     // unsigned <
      {"ule", true, xls_builder_base_add_ule},     // unsigned <=
      {"ugt", true, xls_builder_base_add_ugt},     // unsigned >
      {"uge", true, xls_builder_base_add_uge},     // unsigned >=
      {"slt", true, xls_builder_base_add_slt},     // signed <
      {"sle", true, xls_builder_base_add_sle},     // signed <=
      {"sgt", true, xls_builder_base_add_sgt},     // signed >
      {"sge", true, xls_builder_base_add_sge},     // signed >=
      {"udiv", false, xls_builder_base_add_udiv},  // unsigned /
      {"sdiv", false, xls_builder_base_add_sdiv},  // signed /
      {"umod", false, xls_builder_base_add_umod},  // unsigned %
      {"smod", false, xls_builder_base_add_smod},  // signed %
  };

  for (const TestCase& test_case : kBinops) {
    xls_package* package = xls_package_create("my_package");
    absl::Cleanup free_package([=] { xls_package_free(package); });

    xls_type* u8 = xls_package_get_bits_type(package, 8);

    xls_function_builder* fn_builder =
        xls_function_builder_create("binop", package, /*should_verify=*/true);
    absl::Cleanup free_fn_builder(
        [=] { xls_function_builder_free(fn_builder); });

    xls_builder_base* fn_builder_base =
        xls_function_builder_as_builder_base(fn_builder);

    xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u8);
    absl::Cleanup free_x([=] { xls_bvalue_free(x); });
    xls_bvalue* y = xls_function_builder_add_parameter(fn_builder, "y", u8);
    absl::Cleanup free_y([=] { xls_bvalue_free(y); });

    xls_bvalue* result = test_case.add_op(fn_builder_base, x, y, "result");
    absl::Cleanup free_result([=] { xls_bvalue_free(result); });

    xls_function* function = nullptr;
    {
      char* error = nullptr;
      ASSERT_TRUE(xls_function_builder_build_with_return_value(
          fn_builder, result, &error, &function))
          << "error: " << error;
      ASSERT_NE(function, nullptr);
    }

    // Convert to string and extract the one node from the body.
    char* package_str = nullptr;
    ASSERT_TRUE(xls_package_to_string(package, &package_str));
    absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
    const std::string_view kWantTmpl =
        R"(package my_package

fn binop(x: bits[8] id=1, y: bits[8] id=2) -> bits[%d] {
  ret result: bits[%d] = %s(x, y, id=3)
}
)";

    int64_t result_bits = test_case.is_comparison ? 1 : 8;
    EXPECT_THAT(std::string_view{package_str},
                HasSubstr(absl::StrFormat(kWantTmpl, result_bits, result_bits,
                                          test_case.op_name)));
  }
}

TEST(XlsCApiTest, FnBuilderUnaryOps) {
  struct TestCase {
    // Operation name to expect in the IR text.
    std::string_view op_name;
    // The number of output result bits we expect for the op.
    int64_t result_bits;
    // Builder operation to add the unary operation.
    std::function<xls_bvalue*(xls_builder_base*, xls_bvalue*, const char*)>
        add_op;
    // Any extra attributes we expect in the unary operation output IR text.
    std::string extra_attributes;
  };
  const std::vector<TestCase> kUnaryOps = {
      TestCase{"not", 8, xls_builder_base_add_not},
      TestCase{"neg", 8, xls_builder_base_add_negate},
      TestCase{"reverse", 8, xls_builder_base_add_reverse},
      TestCase{"and_reduce", 1, xls_builder_base_add_and_reduce},
      TestCase{"or_reduce", 1, xls_builder_base_add_or_reduce},
      TestCase{"xor_reduce", 1, xls_builder_base_add_xor_reduce},
      TestCase{"one_hot", 9,
               [](xls_builder_base* builder, xls_bvalue* x, const char* name) {
                 return xls_builder_base_add_one_hot(
                     builder, x, /*lsb_is_priority=*/true, name);
               },
               ", lsb_prio=true"},
      TestCase{"one_hot", 9,
               [](xls_builder_base* builder, xls_bvalue* x, const char* name) {
                 return xls_builder_base_add_one_hot(
                     builder, x, /*lsb_is_priority=*/false, name);
               },
               ", lsb_prio=false"},
      TestCase{"sign_ext", 16,
               [](xls_builder_base* builder, xls_bvalue* x, const char* name) {
                 return xls_builder_base_add_sign_extend(builder, x, 16, name);
               },
               ", new_bit_count=16"},
      TestCase{"zero_ext", 16,
               [](xls_builder_base* builder, xls_bvalue* x, const char* name) {
                 return xls_builder_base_add_zero_extend(builder, x, 16, name);
               },
               ", new_bit_count=16"},
  };

  for (const TestCase& test_case : kUnaryOps) {
    xls_package* package = xls_package_create("my_package");
    absl::Cleanup free_package([=] { xls_package_free(package); });

    xls_type* u8 = xls_package_get_bits_type(package, 8);

    xls_function_builder* fn_builder =
        xls_function_builder_create("unaryop", package, /*should_verify=*/true);
    absl::Cleanup free_fn_builder(
        [=] { xls_function_builder_free(fn_builder); });

    xls_builder_base* fn_builder_base =
        xls_function_builder_as_builder_base(fn_builder);

    xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u8);
    absl::Cleanup free_x([=] { xls_bvalue_free(x); });

    xls_bvalue* result = test_case.add_op(fn_builder_base, x, "result");
    absl::Cleanup free_result([=] { xls_bvalue_free(result); });

    xls_function* function = nullptr;
    {
      char* error = nullptr;
      ASSERT_TRUE(xls_function_builder_build_with_return_value(
          fn_builder, result, &error, &function))
          << "error: " << error;
      ASSERT_NE(function, nullptr);
    }

    // Convert to string and extract the one node from the body.
    char* package_str = nullptr;
    ASSERT_TRUE(xls_package_to_string(package, &package_str));
    absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
    const std::string_view kWantTmpl =
        R"(package my_package

fn unaryop(x: bits[8] id=1) -> bits[%d] {
  ret result: bits[%d] = %s(x%s, id=2)
}
)";
    EXPECT_THAT(std::string_view{package_str},
                HasSubstr(absl::StrFormat(
                    kWantTmpl, test_case.result_bits, test_case.result_bits,
                    test_case.op_name, test_case.extra_attributes)));
  }
}

TEST(XlsCApiTest, FnBuilderArrayOps) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u8 = xls_package_get_bits_type(package, 8);
  xls_type* u32 = xls_package_get_bits_type(package, 32);
  xls_type* u8_arr3 = xls_package_get_array_type(package, u8, 3);

  xls_function_builder* fn_builder =
      xls_function_builder_create("array_ops", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u8_arr3);
  bvalues_to_free.push_back(x);
  xls_bvalue* y = xls_function_builder_add_parameter(fn_builder, "y", u8_arr3);
  bvalues_to_free.push_back(y);
  xls_bvalue* idx = xls_function_builder_add_parameter(fn_builder, "idx", u32);
  bvalues_to_free.push_back(idx);
  xls_bvalue* update_val =
      xls_function_builder_add_parameter(fn_builder, "update_val", u8);
  bvalues_to_free.push_back(update_val);

  // Array literal: lit = u8[3]:[1, 2, 3]
  xls_bvalue* lit = nullptr;
  {
    char* error = nullptr;
    xls_value* v1 = nullptr;
    ASSERT_TRUE(xls_value_make_ubits(8, 1, &error, &v1));
    absl::Cleanup free_v1([=] { xls_value_free(v1); });
    xls_value* v2 = nullptr;
    ASSERT_TRUE(xls_value_make_ubits(8, 2, &error, &v2));
    absl::Cleanup free_v2([=] { xls_value_free(v2); });
    xls_value* v3 = nullptr;
    ASSERT_TRUE(xls_value_make_ubits(8, 3, &error, &v3));
    absl::Cleanup free_v3([=] { xls_value_free(v3); });

    xls_value* elements[] = {v1, v2, v3};
    xls_value* arr_val = nullptr;
    ASSERT_TRUE(
        xls_value_make_array(/*element_count=*/3, elements, &error, &arr_val));
    absl::Cleanup free_arr_val([=] { xls_value_free(arr_val); });

    lit = xls_builder_base_add_literal(fn_builder_base, arr_val, "lit");
    bvalues_to_free.push_back(lit);
  }

  // Index into x: x_idx = x[idx]
  xls_bvalue* indices_idx[] = {idx};
  xls_bvalue* x_idx = xls_builder_base_add_array_index(
      fn_builder_base, x, indices_idx, 1, /*assumed_in_bounds=*/true, "x_idx");
  bvalues_to_free.push_back(x_idx);

  // Slice x: x_slice = x[idx: width 2]
  xls_bvalue* x_slice =
      xls_builder_base_add_array_slice(fn_builder_base, x, idx, 2, "x_slice");
  bvalues_to_free.push_back(x_slice);

  // Update x: x_upd = x[idx: update_val]
  xls_bvalue* indices_upd[] = {idx};
  xls_bvalue* x_upd = xls_builder_base_add_array_update(
      fn_builder_base, x, update_val, indices_upd, 1,
      /*assumed_in_bounds=*/true, "x_upd");
  bvalues_to_free.push_back(x_upd);

  // Concat x and y: concat = x ++ y
  xls_bvalue* concat_ops[] = {x, y};
  xls_bvalue* concat_arr = xls_builder_base_add_array_concat(
      fn_builder_base, concat_ops, 2, "concat_arr");
  bvalues_to_free.push_back(concat_arr);

  xls_bvalue* return_elts[] = {lit, x_idx, x_slice, x_upd, concat_arr};
  xls_bvalue* result =
      xls_builder_base_add_tuple(fn_builder_base, return_elts, 5, "result");
  bvalues_to_free.push_back(result);

  xls_function* function = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                             &error, &function))
        << "error: " << error;
    ASSERT_NE(function, nullptr);
  }

  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

fn array_ops(x: bits[8][3] id=1, y: bits[8][3] id=2, idx: bits[32] id=3, update_val: bits[8] id=4) -> (bits[8][3], bits[8], bits[8][2], bits[8][3], bits[8][6]) {
  lit: bits[8][3] = literal(value=[1, 2, 3], id=5)
  x_idx: bits[8] = array_index(x, indices=[idx], assumed_in_bounds=true, id=6)
  x_slice: bits[8][2] = array_slice(x, idx, width=2, id=7)
  x_upd: bits[8][3] = array_update(x, update_val, indices=[idx], assumed_in_bounds=true, id=8)
  concat_arr: bits[8][6] = array_concat(x, y, id=9)
  ret result: (bits[8][3], bits[8], bits[8][2], bits[8][3], bits[8][6]) = tuple(lit, x_idx, x_slice, x_upd, concat_arr, id=10)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);
}

TEST(XlsCApiTest, FnBuilderShiftOps) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u8 = xls_package_get_bits_type(package, 8);
  xls_type* u3 = xls_package_get_bits_type(package, 3);

  xls_function_builder* fn_builder =
      xls_function_builder_create("shift_ops", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u8);
  bvalues_to_free.push_back(x);
  xls_bvalue* amt = xls_function_builder_add_parameter(fn_builder, "amt", u3);
  bvalues_to_free.push_back(amt);

  xls_bvalue* shra_op =
      xls_builder_base_add_shra(fn_builder_base, x, amt, "shra_op");
  bvalues_to_free.push_back(shra_op);
  xls_bvalue* shrl_op =
      xls_builder_base_add_shrl(fn_builder_base, x, amt, "shrl_op");
  bvalues_to_free.push_back(shrl_op);
  xls_bvalue* shll_op =
      xls_builder_base_add_shll(fn_builder_base, x, amt, "shll_op");
  bvalues_to_free.push_back(shll_op);

  xls_bvalue* return_elts[] = {shra_op, shrl_op, shll_op};
  xls_bvalue* result =
      xls_builder_base_add_tuple(fn_builder_base, return_elts, 3, "result");
  bvalues_to_free.push_back(result);

  xls_function* function = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                             &error, &function))
        << "error: " << error;
    ASSERT_NE(function, nullptr);
  }

  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

fn shift_ops(x: bits[8] id=1, amt: bits[3] id=2) -> (bits[8], bits[8], bits[8]) {
  shra_op: bits[8] = shra(x, amt, id=3)
  shrl_op: bits[8] = shrl(x, amt, id=4)
  shll_op: bits[8] = shll(x, amt, id=5)
  ret result: (bits[8], bits[8], bits[8]) = tuple(shra_op, shrl_op, shll_op, id=6)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);
}

TEST(XlsCApiTest, FnBuilderBitwiseUpdateAndNor) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u16 = xls_package_get_bits_type(package, 16);
  xls_type* u8 = xls_package_get_bits_type(package, 8);
  xls_type* u4 = xls_package_get_bits_type(package, 4);

  xls_function_builder* fn_builder = xls_function_builder_create(
      "bitwise_update_nor", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u16);
  bvalues_to_free.push_back(x);
  xls_bvalue* start =
      xls_function_builder_add_parameter(fn_builder, "start", u4);
  bvalues_to_free.push_back(start);
  xls_bvalue* update =
      xls_function_builder_add_parameter(fn_builder, "update", u8);
  bvalues_to_free.push_back(update);
  xls_bvalue* y = xls_function_builder_add_parameter(fn_builder, "y", u16);
  bvalues_to_free.push_back(y);

  xls_bvalue* bsu = xls_builder_base_add_bit_slice_update(fn_builder_base, x,
                                                          start, update, "bsu");
  bvalues_to_free.push_back(bsu);

  xls_bvalue* nor_op =
      xls_builder_base_add_nor(fn_builder_base, x, y, "nor_op");
  bvalues_to_free.push_back(nor_op);

  xls_bvalue* return_elts[] = {bsu, nor_op};
  xls_bvalue* result =
      xls_builder_base_add_tuple(fn_builder_base, return_elts, 2, "result");
  bvalues_to_free.push_back(result);

  xls_function* function = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                           &error, &function))
      << "error: " << error;
  ASSERT_NE(function, nullptr);

  // Prepare inputs
  // x = 0b1111_0000_1111_0000 = 0xF0F0
  // start = 4
  // update = 0b_1010_1010 = 0xAA
  // y = 0b0000_1111_0000_1111 = 0x0F0F
  xls_value* x_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 0xF0F0, &error, &x_v));
  absl::Cleanup c_x([=] { xls_value_free(x_v); });
  xls_value* start_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(4, 4, &error, &start_v));
  absl::Cleanup c_start([=] { xls_value_free(start_v); });
  xls_value* update_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 0xAA, &error, &update_v));
  absl::Cleanup c_update([=] { xls_value_free(update_v); });
  xls_value* y_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 0x0F0F, &error, &y_v));
  absl::Cleanup c_y([=] { xls_value_free(y_v); });

  std::vector<xls_value*> args = {x_v, start_v, update_v, y_v};

  // Run Interpreter
  xls_value* actual_result = nullptr;
  ASSERT_TRUE(xls_interpret_function(function, args.size(), args.data(), &error,
                                     &actual_result))
      << "error: " << error;
  absl::Cleanup free_actual_result([=] { xls_value_free(actual_result); });

  // Prepare expected output
  // bsu = x with bits[11:4] replaced by update
  // x     = 1111_0000_1111_0000
  // update=       1010_1010
  // result= 1111_1010_1010_0000 = 0xFAAF
  xls_value* exp_bsu = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 0xFAA0, &error, &exp_bsu));
  absl::Cleanup c_exp_bsu([=] { xls_value_free(exp_bsu); });
  // nor = !(x | y)
  // x | y = 0xF0F0 | 0x0F0F = 0xFFF
  // !(0xFFF) = 0x0000
  xls_value* exp_nor = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 0x0000, &error, &exp_nor));
  absl::Cleanup c_exp_nor([=] { xls_value_free(exp_nor); });

  xls_value* expected_elts[] = {exp_bsu, exp_nor};
  xls_value* expected_result = xls_value_make_tuple(2, expected_elts);
  absl::Cleanup free_expected_result([=] { xls_value_free(expected_result); });

  // Compare results
  char* actual_str = nullptr;
  char* expected_str = nullptr;
  ASSERT_TRUE(xls_value_to_string(actual_result, &actual_str));
  absl::Cleanup free_actual_str([=] { xls_c_str_free(actual_str); });
  ASSERT_TRUE(xls_value_to_string(expected_result, &expected_str));
  absl::Cleanup free_expected_str([=] { xls_c_str_free(expected_str); });
  EXPECT_TRUE(xls_value_eq(actual_result, expected_result))
      << "Actual: " << actual_str << "\\nExpected: " << expected_str;
}

TEST(XlsCApiTest, FnBuilderMiscOps) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u2 = xls_package_get_bits_type(package, 2);
  xls_type* u4 = xls_package_get_bits_type(package, 4);
  xls_type* u8 = xls_package_get_bits_type(package, 8);
  xls_type* u16 = xls_package_get_bits_type(package, 16);

  xls_function_builder* fn_builder =
      xls_function_builder_create("misc_ops", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  // Parameters
  xls_bvalue* sel = xls_function_builder_add_parameter(fn_builder, "sel", u2);
  bvalues_to_free.push_back(sel);
  xls_bvalue* c0 = xls_function_builder_add_parameter(fn_builder, "c0", u8);
  bvalues_to_free.push_back(c0);
  xls_bvalue* c1 = xls_function_builder_add_parameter(fn_builder, "c1", u8);
  bvalues_to_free.push_back(c1);
  xls_bvalue* c2 = xls_function_builder_add_parameter(fn_builder, "c2", u8);
  bvalues_to_free.push_back(c2);
  xls_bvalue* c3 = xls_function_builder_add_parameter(fn_builder, "c3", u8);
  bvalues_to_free.push_back(c3);
  xls_bvalue* z_arg =
      xls_function_builder_add_parameter(fn_builder, "z_arg", u16);
  bvalues_to_free.push_back(z_arg);
  xls_bvalue* enc_arg =
      xls_function_builder_add_parameter(fn_builder, "enc_arg", u4);
  bvalues_to_free.push_back(enc_arg);
  xls_bvalue* dec_arg =
      xls_function_builder_add_parameter(fn_builder, "dec_arg", u4);
  bvalues_to_free.push_back(dec_arg);
  xls_bvalue* id_arg =
      xls_function_builder_add_parameter(fn_builder, "id_arg", u8);
  bvalues_to_free.push_back(id_arg);

  // Operations
  xls_bvalue* cases[] = {c0, c1, c2, c3};
  xls_bvalue* select_op = xls_builder_base_add_select(
      fn_builder_base, sel, cases, 4, /*default_value=*/nullptr, "select_op");
  bvalues_to_free.push_back(select_op);

  xls_bvalue* clz_op =
      xls_builder_base_add_clz(fn_builder_base, z_arg, "clz_op");
  bvalues_to_free.push_back(clz_op);
  xls_bvalue* ctz_op =
      xls_builder_base_add_ctz(fn_builder_base, z_arg, "ctz_op");
  bvalues_to_free.push_back(ctz_op);

  xls_bvalue* encode_op =
      xls_builder_base_add_encode(fn_builder_base, enc_arg, "encode_op");
  bvalues_to_free.push_back(encode_op);

  xls_bvalue* decode_op = xls_builder_base_add_decode(fn_builder_base, dec_arg,
                                                      nullptr, "decode_op");
  bvalues_to_free.push_back(decode_op);
  int64_t decode_width = 8;
  xls_bvalue* decode_op_wide = xls_builder_base_add_decode(
      fn_builder_base, dec_arg, &decode_width, "decode_op_wide");
  bvalues_to_free.push_back(decode_op_wide);

  xls_bvalue* id_op =
      xls_builder_base_add_identity(fn_builder_base, id_arg, "id_op");
  bvalues_to_free.push_back(id_op);

  // Result tuple
  xls_bvalue* return_elts[] = {select_op, clz_op,         ctz_op, encode_op,
                               decode_op, decode_op_wide, id_op};
  xls_bvalue* result =
      xls_builder_base_add_tuple(fn_builder_base, return_elts, 7, "result");
  bvalues_to_free.push_back(result);

  // Build function
  xls_function* function = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                           &error, &function))
      << "error: " << error;
  ASSERT_NE(function, nullptr);

  // Prepare inputs
  xls_value* sel_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(2, 1, &error, &sel_v));
  absl::Cleanup c_sel([=] { xls_value_free(sel_v); });
  xls_value* c0_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 10, &error, &c0_v));
  absl::Cleanup c_c0([=] { xls_value_free(c0_v); });
  xls_value* c1_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 20, &error, &c1_v));
  absl::Cleanup c_c1([=] { xls_value_free(c1_v); });
  xls_value* c2_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 30, &error, &c2_v));
  absl::Cleanup c_c2([=] { xls_value_free(c2_v); });
  xls_value* c3_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 40, &error, &c3_v));
  absl::Cleanup c_c3([=] { xls_value_free(c3_v); });
  xls_value* z_arg_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 0b0000101100001000, &error, &z_arg_v));
  absl::Cleanup c_z_arg([=] { xls_value_free(z_arg_v); });
  xls_value* enc_arg_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(4, 0b1000, &error, &enc_arg_v));
  absl::Cleanup c_enc_arg([=] { xls_value_free(enc_arg_v); });
  xls_value* dec_arg_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(4, 0b0100, &error, &dec_arg_v));
  absl::Cleanup c_dec_arg([=] { xls_value_free(dec_arg_v); });
  xls_value* id_arg_v = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 55, &error, &id_arg_v));
  absl::Cleanup c_id_arg([=] { xls_value_free(id_arg_v); });

  std::vector<xls_value*> args = {sel_v,   c0_v,      c1_v,      c2_v,    c3_v,
                                  z_arg_v, enc_arg_v, dec_arg_v, id_arg_v};

  // Run Interpreter
  xls_value* actual_result = nullptr;
  ASSERT_TRUE(xls_interpret_function(function, args.size(), args.data(), &error,
                                     &actual_result))
      << "error: " << error;
  absl::Cleanup free_actual_result([=] { xls_value_free(actual_result); });

  // Prepare expected output
  xls_value* exp_sel = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 20, &error, &exp_sel));
  absl::Cleanup c_exp_sel([=] { xls_value_free(exp_sel); });  // sel=1 -> c1_v
  xls_value* exp_clz = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 4, &error, &exp_clz));
  absl::Cleanup c_exp_clz([=] {
    xls_value_free(exp_clz);
  });  // clz(0b00001...) = 4. Actual type seems to be input width.
  xls_value* exp_ctz = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 3, &error, &exp_ctz));
  absl::Cleanup c_exp_ctz([=] {
    xls_value_free(exp_ctz);
  });  // ctz(...01000) = 3. Actual type seems to be input width.
  xls_value* exp_enc = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(2, 3, &error, &exp_enc));
  absl::Cleanup c_exp_enc(
      [=] { xls_value_free(exp_enc); });  // encode(0b1000) = 3. ceil(log2(4))=2
  xls_value* exp_dec = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(16, 16, &error, &exp_dec));
  absl::Cleanup c_exp_dec([=] {
    xls_value_free(exp_dec);
  });  // decode(bits[4]:4) = 16 (0b10000). default width 2^4=16
  xls_value* exp_dec_w = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 16, &error, &exp_dec_w));
  absl::Cleanup c_exp_dec_w([=] {
    xls_value_free(exp_dec_w);
  });  // decode(bits[4]:4, width=8) = 16 (0b10000)
  xls_value* exp_id = nullptr;
  ASSERT_TRUE(xls_value_make_ubits(8, 55, &error, &exp_id));
  absl::Cleanup c_exp_id([=] { xls_value_free(exp_id); });  // identity(55) = 55

  xls_value* expected_elts[] = {exp_sel, exp_clz,   exp_ctz, exp_enc,
                                exp_dec, exp_dec_w, exp_id};
  xls_value* expected_result = xls_value_make_tuple(7, expected_elts);
  absl::Cleanup free_expected_result([=] { xls_value_free(expected_result); });

  // Compare results
  char* actual_str = nullptr;
  char* expected_str = nullptr;
  ASSERT_TRUE(xls_value_to_string(actual_result, &actual_str));
  absl::Cleanup free_actual_str(
      [=] { xls_c_str_free(actual_str); });  // Ensure free
  ASSERT_TRUE(xls_value_to_string(expected_result, &expected_str));
  absl::Cleanup free_expected_str(
      [=] { xls_c_str_free(expected_str); });  // Ensure free
  EXPECT_TRUE(xls_value_eq(actual_result, expected_result))
      << "Actual: " << actual_str          // Use directly now
      << "\\nExpected: " << expected_str;  // Use directly now
}

TEST(XlsCApiTest, FnBuilderTokenOps) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* token_type = xls_package_get_token_type(package);

  xls_function_builder* fn_builder =
      xls_function_builder_create("token_ops", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* tok1 =
      xls_function_builder_add_parameter(fn_builder, "tok1", token_type);
  bvalues_to_free.push_back(tok1);
  xls_bvalue* tok2 =
      xls_function_builder_add_parameter(fn_builder, "tok2", token_type);
  bvalues_to_free.push_back(tok2);

  xls_bvalue* deps[] = {tok1, tok2};
  xls_bvalue* after_all_op =
      xls_builder_base_add_after_all(fn_builder_base, deps, 2, "after_all_op");
  bvalues_to_free.push_back(after_all_op);

  xls_function* function = nullptr;
  {
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(
        fn_builder, after_all_op, &error, &function))
        << "error: " << error;
    ASSERT_NE(function, nullptr);
  }

  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

fn token_ops(tok1: token id=1, tok2: token id=2) -> token {
  ret after_all_op: token = after_all(tok1, tok2, id=3)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);
}

TEST(XlsCApiTest, FnBuilderGetTypeAndLastValue) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  xls_type* u32 = xls_package_get_bits_type(package, 32);

  xls_function_builder* fn_builder = xls_function_builder_create(
      "get_type_last_val", package, /*should_verify=*/true);
  absl::Cleanup free_fn_builder([=] { xls_function_builder_free(fn_builder); });

  xls_builder_base* fn_builder_base =
      xls_function_builder_as_builder_base(fn_builder);

  std::vector<xls_bvalue*> bvalues_to_free;
  absl::Cleanup free_bvalues([&] {
    for (xls_bvalue* b : bvalues_to_free) {
      xls_bvalue_free(b);
    }
  });

  xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u32);
  bvalues_to_free.push_back(x);

  xls_bvalue* y = xls_builder_base_add_add(fn_builder_base, x, x, "y");
  bvalues_to_free.push_back(y);  // Add y to cleanup list

  // Test GetLastValue
  xls_bvalue* last_val = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(
      xls_builder_base_get_last_value(fn_builder_base, &error, &last_val))
      << "error: " << error;
  absl::Cleanup free_last_val([=] { xls_bvalue_free(last_val); });
  ASSERT_NE(last_val, nullptr);
  // Note: Cannot directly compare BValue pointers reliably. The IR string check
  // implicitly verifies GetLastValue returned the correct BValue `y`.

  // Test GetType
  xls_type* last_val_type =
      xls_builder_base_get_type(fn_builder_base, last_val);
  ASSERT_NE(last_val_type, nullptr);
  EXPECT_EQ(last_val_type, u32);  // Check if the type pointer matches u32

  // Test GetType on an earlier value
  xls_type* x_type = xls_builder_base_get_type(fn_builder_base, x);
  ASSERT_NE(x_type, nullptr);
  EXPECT_EQ(x_type, u32);  // Check if the type pointer matches u32

  xls_function* function = nullptr;
  ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, last_val,
                                                           &error, &function))
      << "error: " << error;
  ASSERT_NE(function, nullptr);

  char* package_str = nullptr;
  ASSERT_TRUE(xls_package_to_string(package, &package_str));
  absl::Cleanup free_package_str([=] { xls_c_str_free(package_str); });
  const std::string_view kWant = R"(package my_package

fn get_type_last_val(x: bits[32] id=1) -> bits[32] {
  ret y: bits[32] = add(x, x, id=2)
}
)";
  EXPECT_EQ(std::string_view{package_str}, kWant);

  char* param_name = nullptr;
  ASSERT_TRUE(
      xls_function_get_param_name(function, /*index=*/0, &error, &param_name));
  absl::Cleanup free_param_name([param_name] { xls_c_str_free(param_name); });
  EXPECT_EQ(std::string_view(param_name), "x");

  // Out-of-bounds param name.
  char* bogus_param_name = nullptr;
  ASSERT_FALSE(xls_function_get_param_name(function, /*index=*/1, &error,
                                           &bogus_param_name));
  EXPECT_THAT(std::string_view{error}, HasSubstr("out of range"));
  xls_c_str_free(error);
  error = nullptr;
}

TEST(XlsCApiTest, TypeGetFlatBitCount) {
  xls_package* package = xls_package_create("my_package");
  absl::Cleanup free_package([=] { xls_package_free(package); });

  // Simple bits type
  xls_type* u32_type = xls_package_get_bits_type(package, 32);
  EXPECT_EQ(xls_type_get_flat_bit_count(u32_type), 32);

  // Token type
  xls_type* token_type = xls_package_get_token_type(package);
  EXPECT_EQ(xls_type_get_flat_bit_count(token_type), 0);

  // Tuple type
  xls_type* u8_type = xls_package_get_bits_type(package, 8);
  xls_type* u16_type = xls_package_get_bits_type(package, 16);
  xls_type* tuple_members[] = {u8_type, u16_type};
  xls_type* tuple_type = xls_package_get_tuple_type(package, tuple_members, 2);
  EXPECT_EQ(xls_type_get_flat_bit_count(tuple_type), 24);  // 8 + 16

  // Array type
  xls_type* u4_type = xls_package_get_bits_type(package, 4);
  xls_type* array_type = xls_package_get_array_type(package, u4_type, 3);
  EXPECT_EQ(xls_type_get_flat_bit_count(array_type), 12);  // 4 * 3

  // Nested tuple and array
  // ((bits[2], bits[3]), bits[1][5])
  xls_type* u2_type = xls_package_get_bits_type(package, 2);
  xls_type* u3_type = xls_package_get_bits_type(package, 3);
  xls_type* inner_tuple_members[] = {u2_type, u3_type};
  xls_type* inner_tuple_type =
      xls_package_get_tuple_type(package, inner_tuple_members, 2);  // 2 + 3 = 5

  xls_type* u1_type = xls_package_get_bits_type(package, 1);
  xls_type* inner_array_type =
      xls_package_get_array_type(package, u1_type, 5);  // 1 * 5 = 5

  xls_type* outer_tuple_members[] = {inner_tuple_type, inner_array_type};
  xls_type* nested_type =
      xls_package_get_tuple_type(package, outer_tuple_members, 2);
  EXPECT_EQ(xls_type_get_flat_bit_count(nested_type), 10);  // 5 + 5
}

TEST(XlsCApiTest, BitsRopeCreateFree) {
  xls_bits_rope* rope = xls_create_bits_rope(10);
  ASSERT_NE(rope, nullptr);
  xls_bits_rope_free(rope);
}

TEST(XlsCApiTest, BitsRopePushBack) {
  char* error_out = nullptr;
  xls_bits_rope* rope = xls_create_bits_rope(10);
  ASSERT_NE(rope, nullptr);
  absl::Cleanup free_rope([rope] { xls_bits_rope_free(rope); });

  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(3, 0b101, &error_out, &bits));
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });

  xls_bits_rope_append_bits(rope, bits);
  ASSERT_EQ(error_out, nullptr);
}

TEST(XlsCApiTest, BitsRopeBuild) {
  char* error_out = nullptr;
  xls_bits_rope* rope = xls_create_bits_rope(3);
  ASSERT_NE(rope, nullptr);
  absl::Cleanup free_rope([rope] { xls_bits_rope_free(rope); });

  xls_bits* bits = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(3, 0b101, &error_out, &bits));
  absl::Cleanup free_bits([bits] { xls_bits_free(bits); });

  xls_bits_rope_append_bits(rope, bits);
  ASSERT_EQ(error_out, nullptr);

  xls_bits* result = xls_bits_rope_get_bits(rope);
  ASSERT_NE(result, nullptr);
  absl::Cleanup free_result([result] { xls_bits_free(result); });

  char* result_str = xls_bits_to_debug_string(result);
  absl::Cleanup free_result_str([result_str] { xls_c_str_free(result_str); });
  EXPECT_EQ(std::string(result_str), "0b101");
}

TEST(XlsCApiTest, BitsRopePushMultipleAndBuild) {
  char* error_out = nullptr;
  xls_bits_rope* rope = xls_create_bits_rope(5);
  ASSERT_NE(rope, nullptr);
  absl::Cleanup free_rope([rope] { xls_bits_rope_free(rope); });

  xls_bits* bits1 = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(3, 0b101, &error_out, &bits1));
  absl::Cleanup free_bits1([bits1] { xls_bits_free(bits1); });

  xls_bits* bits2 = nullptr;
  ASSERT_TRUE(xls_bits_make_ubits(2, 0b10, &error_out, &bits2));
  absl::Cleanup free_bits2([bits2] { xls_bits_free(bits2); });

  xls_bits_rope_append_bits(rope, bits1);
  ASSERT_EQ(error_out, nullptr);
  xls_bits_rope_append_bits(rope, bits2);
  ASSERT_EQ(error_out, nullptr);

  xls_bits* result = xls_bits_rope_get_bits(rope);
  ASSERT_NE(result, nullptr);
  absl::Cleanup free_result([result] { xls_bits_free(result); });

  char* result_str = xls_bits_to_debug_string(result);
  absl::Cleanup free_result_str([result_str] { xls_c_str_free(result_str); });
  EXPECT_EQ(std::string(result_str), "0b10101");
}

TEST(XlsCApiTest, FnBuilderPartialProductOps) {
  struct TestCase {
    std::string_view op_name;
    std::function<xls_bvalue*(xls_builder_base*, xls_bvalue*, xls_bvalue*,
                              const char*)>
        add_op;
  };
  const std::vector<TestCase> kCases = {
      {"umulp", xls_builder_base_add_umulp},
      {"smulp", xls_builder_base_add_smulp},
  };

  for (const auto& tc : kCases) {
    xls_package* package = xls_package_create("my_package");
    absl::Cleanup free_package([=] { xls_package_free(package); });

    xls_type* u8 = xls_package_get_bits_type(package, 8);
    xls_function_builder* fn_builder =
        xls_function_builder_create("pp", package, /*should_verify=*/true);
    absl::Cleanup free_fn_builder(
        [=] { xls_function_builder_free(fn_builder); });
    xls_builder_base* base = xls_function_builder_as_builder_base(fn_builder);

    xls_bvalue* x = xls_function_builder_add_parameter(fn_builder, "x", u8);
    absl::Cleanup free_x([=] { xls_bvalue_free(x); });
    xls_bvalue* y = xls_function_builder_add_parameter(fn_builder, "y", u8);
    absl::Cleanup free_y([=] { xls_bvalue_free(y); });

    xls_bvalue* result = tc.add_op(base, x, y, "result");
    absl::Cleanup free_result([=] { xls_bvalue_free(result); });

    xls_function* function = nullptr;
    char* error = nullptr;
    ASSERT_TRUE(xls_function_builder_build_with_return_value(fn_builder, result,
                                                             &error, &function))
        << error;

    // Check that the IR contains the operation.
    char* pkg_str = nullptr;
    ASSERT_TRUE(xls_package_to_string(package, &pkg_str));
    absl::Cleanup free_pkg_str([=] { xls_c_str_free(pkg_str); });
    std::string_view text(pkg_str);
    EXPECT_THAT(text, HasSubstr(absl::StrFormat(" = %s(", tc.op_name)));
  }
}

TEST(XlsCApiTest, FunctionToZ3Smtlib) {
  const std::string kPackage = R"(package p

top fn add(x: bits[32], y: bits[32]) -> bits[32] {
  ret result: bits[32] = add(x, y)
}
)";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(kPackage.c_str(), "p.ir", &error, &package))
      << "xls_parse_ir_package error: " << error;
  absl::Cleanup free_package([package] { xls_package_free(package); });

  xls_function* function = nullptr;
  ASSERT_TRUE(xls_package_get_function(package, "add", &error, &function));

  char* smtlib = nullptr;
  ASSERT_TRUE(xls_function_to_z3_smtlib(function, &error, &smtlib))
      << "xls_function_to_z3_smtlib error: " << error;
  absl::Cleanup free_smtlib([smtlib] { xls_c_str_free(smtlib); });

  EXPECT_EQ(
      std::string_view{smtlib},
      R"((declare-fun add () (Array (_ BitVec 32) (_ BitVec 32) (_ BitVec 32)))
(assert (= add (lambda ((x (_ BitVec 32)) (y (_ BitVec 32))) (bvadd x y))))
)");
}

// Tests that we can determine whether a DSLX function is parametric via the
// C API.
TEST(XlsCApiTest, DslxFunctionIsParametric) {
  const std::string_view kProgram = R"(
fn non_parametric(x: u32) -> u32 { x }

fn parametric_fn<N: u32>(x: bits[N]) -> bits[N] { x }
)";
  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  bool ok =
      xls_dslx_parse_and_typecheck(kProgram.data(), "test_module.x",
                                   "test_module", import_data, &error, &tm);
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(ok) << "parse-and-typecheck error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  int64_t member_count = xls_dslx_module_get_member_count(module);
  ASSERT_EQ(member_count, 2);

  // First function: non-parametric.
  xls_dslx_module_member* member0 = xls_dslx_module_get_member(module, 0);
  xls_dslx_function* fn0 = xls_dslx_module_member_get_function(member0);
  ASSERT_NE(fn0, nullptr);
  EXPECT_FALSE(xls_dslx_function_is_parametric(fn0));

  // Second function: parametric.
  xls_dslx_module_member* member1 = xls_dslx_module_get_member(module, 1);
  xls_dslx_function* fn1 = xls_dslx_module_member_get_function(member1);
  ASSERT_NE(fn1, nullptr);
  EXPECT_TRUE(xls_dslx_function_is_parametric(fn1));
}

// Tests that QuickCheck module members can be accessed and their properties
// inspected via the C API.
TEST(XlsCApiTest, DslxQuickCheckIntrospection) {
  // A simple property test with an explicit test_count.
  const std::string_view kProgram = R"(
#[quickcheck(test_count=123)]
fn prop(x: u8) -> bool {
  x == x
})";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&] { xls_dslx_import_data_free(import_data); });

  char* error = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.data(), "<test>", "top",
                                         import_data, &error, &tm);
  ASSERT_TRUE(ok) << "error: " << error;
  absl::Cleanup free_tm([&] { xls_dslx_typechecked_module_free(tm); });

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  ASSERT_NE(module, nullptr);

  int64_t member_count = xls_dslx_module_get_member_count(module);
  ASSERT_EQ(member_count, 1);  // Only the QuickCheck member is present.

  xls_dslx_module_member* member = xls_dslx_module_get_member(module, 0);
  ASSERT_NE(member, nullptr);

  // Retrieve the QuickCheck node.
  xls_dslx_quickcheck* qc = xls_dslx_module_member_get_quickcheck(member);
  ASSERT_NE(qc, nullptr);
  EXPECT_EQ(xls_dslx_module_member_from_quickcheck(qc), member);

  // Inspect the associated function.
  xls_dslx_function* fn = xls_dslx_quickcheck_get_function(qc);
  EXPECT_NE(fn, nullptr);

  // Inspect the test-cases specifier.
  EXPECT_FALSE(xls_dslx_quickcheck_is_exhaustive(qc));
  int64_t count = 0;
  ASSERT_TRUE(xls_dslx_quickcheck_get_count(qc, &count));
  EXPECT_EQ(count, 123);
}

TEST(XlsCApiTest, FunctionRequiresImplicitToken) {
  const std::string kProgram =
      R"(// Function that requires implicit token due to fail! macro.
fn need_token() {
  fail!("boom", ())
}

// Function that does not require implicit token.
fn no_token(x: u32) -> u32 {
  x
})";
  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.c_str(), "test.x", "test",
                                         import_data, &error, &tm);
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(ok) << "error: " << (error == nullptr ? "<none>" : error);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);

  int64_t member_count = xls_dslx_module_get_member_count(module);
  EXPECT_EQ(member_count, 2);

  // Function that requires implicit token.
  xls_dslx_module_member* member0 = xls_dslx_module_get_member(module, 0);
  xls_dslx_function* need_token_fn =
      xls_dslx_module_member_get_function(member0);
  ASSERT_NE(need_token_fn, nullptr);
  bool requires_implicit_token = false;
  ASSERT_TRUE(xls_dslx_type_info_get_requires_implicit_token(
      type_info, need_token_fn, &error, &requires_implicit_token));
  EXPECT_TRUE(requires_implicit_token);
  EXPECT_EQ(error, nullptr);

  // Function that does not require implicit token.
  xls_dslx_module_member* member1 = xls_dslx_module_get_member(module, 1);
  xls_dslx_function* no_token_fn = xls_dslx_module_member_get_function(member1);
  ASSERT_NE(no_token_fn, nullptr);
  requires_implicit_token = false;
  ASSERT_TRUE(xls_dslx_type_info_get_requires_implicit_token(
      type_info, no_token_fn, &error, &requires_implicit_token));
  EXPECT_FALSE(requires_implicit_token);
  EXPECT_EQ(error, nullptr);
}

TEST(XlsCApiTest, DslxInvocationCalleeDataIntrospection) {
  const std::string kProgram = R"(
fn id<N: u32>(x: bits[N]) -> bits[N] { x }

fn caller<N: u32>(x: bits[N]) -> bits[N] {
  let _ = id(x);
  id(x)
}

fn main() -> u32 {
  caller(u32:42)
})";

  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [=] { xls_dslx_import_data_free(import_data); });

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  bool ok = xls_dslx_parse_and_typecheck(kProgram.c_str(), "test.x", "test",
                                         import_data, &error, &tm);
  absl::Cleanup free_tm([=] { xls_dslx_typechecked_module_free(tm); });
  ASSERT_TRUE(ok) << "error: " << (error == nullptr ? "<none>" : error);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  xls_dslx_type_info* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  ASSERT_NE(module, nullptr);
  ASSERT_NE(type_info, nullptr);

  xls_dslx_function* id_fn = nullptr;
  xls_dslx_function* caller_fn = nullptr;
  int64_t member_count = xls_dslx_module_get_member_count(module);
  for (int64_t i = 0; i < member_count; ++i) {
    xls_dslx_module_member* member = xls_dslx_module_get_member(module, i);
    if (member == nullptr) {
      continue;
    }
    xls_dslx_function* fn = xls_dslx_module_member_get_function(member);
    if (fn == nullptr) {
      continue;
    }
    char* name = xls_dslx_function_get_identifier(fn);
    ASSERT_NE(name, nullptr);
    absl::Cleanup free_name([name] { xls_c_str_free(name); });
    if (std::string_view{name} == "id") {
      id_fn = fn;
    } else if (std::string_view{name} == "caller") {
      caller_fn = fn;
    }
  }

  ASSERT_NE(id_fn, nullptr);
  ASSERT_NE(caller_fn, nullptr);

  xls_dslx_invocation_callee_data_array* unique_array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, id_fn);
  ASSERT_NE(unique_array, nullptr);
  absl::Cleanup free_unique_array([&unique_array] {
    if (unique_array != nullptr) {
      xls_dslx_invocation_callee_data_array_free(unique_array);
    }
  });

  int64_t unique_count =
      xls_dslx_invocation_callee_data_array_get_count(unique_array);
  ASSERT_EQ(unique_count, 1);

  xls_dslx_invocation_callee_data* callee_data_unique =
      xls_dslx_invocation_callee_data_array_get(unique_array, 0);
  ASSERT_NE(callee_data_unique, nullptr);

  const xls_dslx_parametric_env* callee_env =
      xls_dslx_invocation_callee_data_get_callee_bindings(callee_data_unique);
  ASSERT_NE(callee_env, nullptr);
  EXPECT_EQ(xls_dslx_parametric_env_get_binding_count(callee_env), 1);
  char* callee_env_string = xls_dslx_parametric_env_to_string(callee_env);
  absl::Cleanup free_callee_env_string(
      [callee_env_string] { xls_c_str_free(callee_env_string); });
  EXPECT_STREQ(callee_env_string, "{N: u32:32}");
  const char* callee_identifier =
      xls_dslx_parametric_env_get_binding_identifier(callee_env, 0);
  ASSERT_NE(callee_identifier, nullptr);
  EXPECT_STREQ(callee_identifier, "N");
  xls_dslx_interp_value* callee_value =
      xls_dslx_parametric_env_get_binding_value(callee_env, 0);
  ASSERT_NE(callee_value, nullptr);
  char* callee_value_string = xls_dslx_interp_value_to_string(callee_value);
  ASSERT_NE(callee_value_string, nullptr);
  absl::Cleanup free_callee_value_string(
      [callee_value_string] { xls_c_str_free(callee_value_string); });
  EXPECT_EQ(std::string_view{callee_value_string}, "u32:32");

  xls_dslx_invocation_callee_data* callee_data_clone =
      xls_dslx_invocation_callee_data_clone(callee_data_unique);
  ASSERT_NE(callee_data_clone, nullptr);
  absl::Cleanup free_callee_data_clone([callee_data_clone] {
    xls_dslx_invocation_callee_data_free(callee_data_clone);
  });
  const xls_dslx_parametric_env* cloned_callee_env =
      xls_dslx_invocation_callee_data_get_callee_bindings(callee_data_clone);
  ASSERT_NE(cloned_callee_env, nullptr);
  xls_dslx_interp_value* cloned_callee_value =
      xls_dslx_parametric_env_get_binding_value(cloned_callee_env, 0);
  ASSERT_NE(cloned_callee_value, nullptr);
  char* cloned_callee_value_string =
      xls_dslx_interp_value_to_string(cloned_callee_value);
  absl::Cleanup free_cloned_callee_value_string([cloned_callee_value_string] {
    xls_c_str_free(cloned_callee_value_string);
  });
  EXPECT_STREQ(cloned_callee_value_string, "u32:32");

  const xls_dslx_parametric_env* caller_env =
      xls_dslx_invocation_callee_data_get_caller_bindings(callee_data_unique);
  ASSERT_NE(caller_env, nullptr);
  EXPECT_EQ(xls_dslx_parametric_env_get_binding_count(caller_env), 1);
  const char* caller_identifier =
      xls_dslx_parametric_env_get_binding_identifier(caller_env, 0);
  ASSERT_NE(caller_identifier, nullptr);
  EXPECT_STREQ(caller_identifier, "N");

  xls_dslx_type_info* derived_type_info =
      xls_dslx_invocation_callee_data_get_derived_type_info(callee_data_unique);
  ASSERT_NE(derived_type_info, nullptr);

  xls_dslx_invocation* invocation =
      xls_dslx_invocation_callee_data_get_invocation(callee_data_unique);
  ASSERT_NE(invocation, nullptr);

  xls_dslx_invocation_data* invocation_data =
      xls_dslx_type_info_get_root_invocation_data(type_info, invocation);
  ASSERT_NE(invocation_data, nullptr);

  EXPECT_EQ(xls_dslx_invocation_data_get_callee(invocation_data), id_fn);
  EXPECT_EQ(xls_dslx_invocation_data_get_caller(invocation_data), caller_fn);
  EXPECT_EQ(xls_dslx_invocation_data_get_invocation(invocation_data),
            invocation);

  xls_dslx_invocation_callee_data_array* all_array =
      xls_dslx_type_info_get_all_invocation_callee_data(type_info, id_fn);
  ASSERT_NE(all_array, nullptr);
  absl::Cleanup free_all_array(
      [all_array] { xls_dslx_invocation_callee_data_array_free(all_array); });
  int64_t all_count =
      xls_dslx_invocation_callee_data_array_get_count(all_array);
  ASSERT_EQ(all_count, 2);
  xls_dslx_invocation_callee_data* callee_data_all_0 =
      xls_dslx_invocation_callee_data_array_get(all_array, 0);
  ASSERT_NE(callee_data_all_0, nullptr);
  xls_dslx_invocation_callee_data* callee_data_all_1 =
      xls_dslx_invocation_callee_data_array_get(all_array, 1);
  ASSERT_NE(callee_data_all_1, nullptr);
  xls_dslx_invocation* invocation0 =
      xls_dslx_invocation_callee_data_get_invocation(callee_data_all_0);
  xls_dslx_invocation* invocation1 =
      xls_dslx_invocation_callee_data_get_invocation(callee_data_all_1);
  ASSERT_NE(invocation0, nullptr);
  ASSERT_NE(invocation1, nullptr);
  EXPECT_NE(invocation0, invocation1);

  xls_dslx_invocation_callee_data_array_free(unique_array);
  unique_array = nullptr;
  const xls_dslx_parametric_env* surviving_callee_env =
      xls_dslx_invocation_callee_data_get_callee_bindings(callee_data_clone);
  ASSERT_NE(surviving_callee_env, nullptr);
  char* surviving_callee_env_string =
      xls_dslx_parametric_env_to_string(surviving_callee_env);
  absl::Cleanup free_surviving_callee_env_string([surviving_callee_env_string] {
    xls_c_str_free(surviving_callee_env_string);
  });
  EXPECT_STREQ(surviving_callee_env_string, "{N: u32:32}");
  xls_dslx_interp_value* surviving_callee_value =
      xls_dslx_parametric_env_get_binding_value(surviving_callee_env, 0);
  ASSERT_NE(surviving_callee_value, nullptr);
  char* surviving_callee_value_string =
      xls_dslx_interp_value_to_string(surviving_callee_value);
  absl::Cleanup free_surviving_callee_value_string(
      [surviving_callee_value_string] {
        xls_c_str_free(surviving_callee_value_string);
      });
  EXPECT_STREQ(surviving_callee_value_string, "u32:32");
}

TEST(XlsCApiTest, DslxInvocationSumBindingsRetainFormattingThroughClones) {
  constexpr const char* kProgram = R"(
enum Choice { None, Some(u8) }
fn f<V: Choice>() -> Choice { V }
pub fn main() -> Choice { f<{Choice::Some(u8:1)}>() }
)";
  for (auto get_data : {xls_dslx_type_info_get_unique_invocation_callee_data,
                        xls_dslx_type_info_get_all_invocation_callee_data}) {
    auto* owner = xls_dslx_import_data_create(
        std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
    xls_dslx_typechecked_module* tm = nullptr;
    xls_dslx_invocation_callee_data_array* array = nullptr;
    xls_dslx_invocation_callee_data* data_clone = nullptr;
    xls_dslx_parametric_env* env_clone = nullptr;
    xls_dslx_interp_value* value_clone = nullptr;
    char* error = nullptr;
    absl::Cleanup cleanup([&] {
      xls_c_str_free(error);
      xls_dslx_interp_value_free(value_clone);
      xls_dslx_parametric_env_free(env_clone);
      xls_dslx_invocation_callee_data_free(data_clone);
      xls_dslx_invocation_callee_data_array_free(array);
      xls_dslx_typechecked_module_free(tm);
      xls_dslx_import_data_free(owner);
    });
    ASSERT_TRUE(xls_dslx_parse_and_typecheck(
        kProgram, "invocation_sum.x", "invocation_sum", owner, &error, &tm))
        << error;
    auto* module = xls_dslx_typechecked_module_get_module(tm);
    auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
    auto* function = xls_dslx_module_member_get_function(
        xls_dslx_module_get_member(module, 1));
    ASSERT_NE(function, nullptr);
    array = get_data(type_info, function);
    ASSERT_NE(array, nullptr);
    ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
    auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
    const auto* env = xls_dslx_invocation_callee_data_get_callee_bindings(data);
    ASSERT_EQ(xls_dslx_parametric_env_get_binding_count(env), 1);
    auto* value = xls_dslx_parametric_env_get_binding_value(env, 0);
    EXPECT_EQ(env, xls_dslx_invocation_callee_data_get_callee_bindings(data));
    EXPECT_EQ(value, xls_dslx_parametric_env_get_binding_value(env, 0));
    char* value_text = xls_dslx_interp_value_to_string(value);
    EXPECT_STREQ(value_text, "Choice::Some(u8:1)");
    xls_c_str_free(value_text);
    char* env_text = xls_dslx_parametric_env_to_string(env);
    EXPECT_STREQ(env_text, "{V: Choice::Some(u8:1)}");
    xls_c_str_free(env_text);
    const auto* caller_env =
        xls_dslx_invocation_callee_data_get_caller_bindings(data);
    EXPECT_EQ(xls_dslx_parametric_env_get_binding_count(caller_env), 0);
    char* caller_text = xls_dslx_parametric_env_to_string(caller_env);
    EXPECT_STREQ(caller_text, "{}");
    xls_c_str_free(caller_text);

    data_clone = xls_dslx_invocation_callee_data_clone(data);
    env_clone = xls_dslx_parametric_env_clone(env);
    value_clone = xls_dslx_interp_value_clone(value);
    xls_dslx_invocation_callee_data_array_free(array);
    array = nullptr;
    xls_dslx_typechecked_module_free(tm);
    tm = nullptr;
    xls_dslx_import_data_free(owner);
    owner = nullptr;

    // The independent clones own values and formatting, not the source AST.
    // The cloned entry's AST/TypeInfo accessors still require the source owner.
    for (const auto* surviving_env :
         {xls_dslx_invocation_callee_data_get_callee_bindings(data_clone),
          static_cast<const xls_dslx_parametric_env*>(env_clone)}) {
      char* text = xls_dslx_parametric_env_to_string(surviving_env);
      EXPECT_STREQ(text, "{V: Choice::Some(u8:1)}");
      xls_c_str_free(text);
      char* binding_text = xls_dslx_interp_value_to_string(
          xls_dslx_parametric_env_get_binding_value(surviving_env, 0));
      EXPECT_STREQ(binding_text, "Choice::Some(u8:1)");
      xls_c_str_free(binding_text);
    }
    char* clone_text = xls_dslx_interp_value_to_string(value_clone);
    EXPECT_STREQ(clone_text, "Choice::Some(u8:1)");
    xls_c_str_free(clone_text);
  }
}

TEST(XlsCApiTest, DslxInvocationCallerBindingsUseConcreteTypesAndBindingOrder) {
  constexpr const char* kProgram = R"(
#![feature(generics)]
enum Choice { None, Some(u8) }
fn f<T: type, Z: Choice, A: u32>() -> Choice { Z }
fn caller<Z: Choice, A: u32>() -> Choice { f<Choice, Z, A>() }
pub fn main() -> Choice { caller<{Choice::Some(u8:1)}, u32:7>() }
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_invocation_callee_data* clone = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_invocation_callee_data_free(clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "caller_sum.x",
                                           "caller_sum", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(function, nullptr);
  array =
      xls_dslx_type_info_get_all_invocation_callee_data(type_info, function);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
  clone = xls_dslx_invocation_callee_data_clone(data);
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;

  // Environment order is alphabetical, not declaration order. Neither the
  // ordinary A nor the type-valued T binding should acquire Z's sum format.
  const auto* callee_env =
      xls_dslx_invocation_callee_data_get_callee_bindings(clone);
  ASSERT_EQ(xls_dslx_parametric_env_get_binding_count(callee_env), 3);
  char* callee_text = xls_dslx_parametric_env_to_string(callee_env);
  EXPECT_STREQ(callee_text, "{A: u32:7, T: Choice, Z: Choice::Some(u8:1)}");
  xls_c_str_free(callee_text);
  char* type_text = xls_dslx_interp_value_to_string(
      xls_dslx_parametric_env_get_binding_value(callee_env, 1));
  EXPECT_STREQ(type_text, "Choice");
  xls_c_str_free(type_text);
  const auto* caller_env =
      xls_dslx_invocation_callee_data_get_caller_bindings(clone);
  ASSERT_EQ(xls_dslx_parametric_env_get_binding_count(caller_env), 2);
  char* caller_text = xls_dslx_parametric_env_to_string(caller_env);
  EXPECT_STREQ(caller_text, "{A: u32:7, Z: Choice::Some(u8:1)}");
  xls_c_str_free(caller_text);
  char* value_text = xls_dslx_interp_value_to_string(
      xls_dslx_parametric_env_get_binding_value(caller_env, 1));
  EXPECT_STREQ(value_text, "Choice::Some(u8:1)");
  xls_c_str_free(value_text);
}

TEST(XlsCApiTest, DslxInvocationCallerBindingsDoNotUseCanonicalCalleeContext) {
  constexpr const char* kImported = "pub fn f<N: u32>() -> u32 { N }";
  constexpr const char* kProgram = R"(
import imported;
enum First { None, Some(u8) }
enum Second { None, Other(u8) }
fn first<V: First>() -> u32 { imported::f<u32:1>() }
fn second<W: Second>() -> u32 { imported::f<u32:1>() }
pub fn main() -> u32 {
  first<{First::Some(u8:1)}>() + second<{Second::Other(u8:1)}>()
}
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* imported_tm = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_invocation_callee_data* unique_clone = nullptr;
  std::vector<xls_dslx_invocation_callee_data*> clones;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    for (auto* clone : clones) {
      xls_dslx_invocation_callee_data_free(clone);
    }
    xls_dslx_invocation_callee_data_free(unique_clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_typechecked_module_free(imported_tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kImported, "imported.x", "imported",
                                           owner, &error, &imported_tm))
      << error;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "callers.x", "callers",
                                           owner, &error, &tm))
      << error;
  auto* imported = xls_dslx_typechecked_module_get_module(imported_tm);
  auto* function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(imported, 0));
  ASSERT_NE(function, nullptr);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  // Uniqueness is by callee bindings, not caller: both callers use f<N=1>.
  array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, function);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  unique_clone = xls_dslx_invocation_callee_data_clone(
      xls_dslx_invocation_callee_data_array_get(array, 0));
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;

  // GetAll retains both caller contexts even though the callee is canonical.
  array =
      xls_dslx_type_info_get_all_invocation_callee_data(type_info, function);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 2);
  for (int64_t i = 0; i < 2; ++i) {
    clones.push_back(xls_dslx_invocation_callee_data_clone(
        xls_dslx_invocation_callee_data_array_get(array, i)));
  }
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_typechecked_module_free(imported_tm);
  imported_tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;

  // Both calls share f<N=1>'s canonical TypeInfo, but the caller bindings
  // belong to different functions and nominal types with identical raw
  // representations.
  std::vector<std::string> caller_texts;
  for (auto* clone : clones) {
    const auto* caller_env =
        xls_dslx_invocation_callee_data_get_caller_bindings(clone);
    char* text = xls_dslx_parametric_env_to_string(caller_env);
    caller_texts.emplace_back(text);
    xls_c_str_free(text);
  }
  EXPECT_THAT(caller_texts,
              testing::UnorderedElementsAre("{V: First::Some(u8:1)}",
                                            "{W: Second::Other(u8:1)}"));

  char* unique_callee_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_callee_bindings(unique_clone));
  EXPECT_STREQ(unique_callee_text, "{N: u32:1}");
  xls_c_str_free(unique_callee_text);
  char* unique_caller_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_caller_bindings(unique_clone));
  // The unique representative must retain its caller's semantic type; no
  // particular caller is required to be the first publication.
  EXPECT_THAT(unique_caller_text,
              testing::AnyOf(testing::StrEq("{V: First::Some(u8:1)}"),
                             testing::StrEq("{W: Second::Other(u8:1)}")));
  xls_c_str_free(unique_caller_text);
}

TEST(XlsCApiTest, DslxBuiltinInvocationBindingsWithoutDerivedTypeInfo) {
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_call_graph* graph = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_parametric_env* clone = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_parametric_env_free(clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_call_graph_free(graph);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      "pub fn main() -> u8 { clz(u8:1) }", "builtin_bindings.x",
      "builtin_bindings", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* main = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 0));
  ASSERT_TRUE(xls_dslx_type_info_build_function_call_graph_for_module(
      type_info, module, &error, &graph));
  ASSERT_EQ(xls_dslx_call_graph_get_callee_count(graph, main), 1);
  auto* builtin = xls_dslx_call_graph_get_callee_function(graph, main, 0);
  array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, builtin);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
  EXPECT_EQ(xls_dslx_invocation_callee_data_get_derived_type_info(data),
            nullptr);
  clone = xls_dslx_parametric_env_clone(
      xls_dslx_invocation_callee_data_get_callee_bindings(data));
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_call_graph_free(graph);
  graph = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  char* text = xls_dslx_parametric_env_to_string(clone);
  EXPECT_STREQ(text, "{N: u32:8}");
  xls_c_str_free(text);
}

TEST(XlsCApiTest, DslxInvocationCallerBindingsFromParametricStructDefault) {
  constexpr const char* kProgram = R"(
enum Choice { None, Some(u8) }
fn f<W: Choice>() -> u32 { u32:1 }
struct S<V: Choice, N: u32 = {f<V>()}> {}
type T = S<{Choice::Some(u8:3)}>;
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_parametric_env* clone = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_parametric_env_free(clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "struct_default.x", "struct_default", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(function, nullptr);
  array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, function);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
  char* callee_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_callee_bindings(data));
  EXPECT_STREQ(callee_text, "{W: Choice::Some(u8:3)}");
  xls_c_str_free(callee_text);

  // The type alias evaluates S's default without constructing a struct value
  // or rewriting member types through the bits-only inference path. The call
  // belongs to S, not a function; V is an env key and the defaulted N is not.
  clone = xls_dslx_parametric_env_clone(
      xls_dslx_invocation_callee_data_get_caller_bindings(data));
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  char* caller_text = xls_dslx_parametric_env_to_string(clone);
  EXPECT_STREQ(caller_text, "{V: Choice::Some(u8:3)}");
  xls_c_str_free(caller_text);
}

TEST(XlsCApiTest, DslxInvocationCallerBindingsFromParametricSumDiscriminant) {
  constexpr const char* kProgram = R"(
enum Choice { None, Some(u8) }
fn f<W: Choice>() -> u32 { u32:1 }
enum E<V: Choice>: u32 { A() = f<V>() }
type T = E<{Choice::Some(u8:3)}>;
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_parametric_env* clone = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_parametric_env_free(clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "sum_discriminant.x", "sum_discriminant", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* function = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(function, nullptr);
  array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, function);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
  char* callee_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_callee_bindings(data));
  EXPECT_STREQ(callee_text, "{W: Choice::Some(u8:3)}");
  xls_c_str_free(callee_text);

  // The call belongs to E's discriminant, not an enclosing function. Its
  // cloned environment must retain nominal formatting after the owner dies.
  clone = xls_dslx_parametric_env_clone(
      xls_dslx_invocation_callee_data_get_caller_bindings(data));
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  char* caller_text = xls_dslx_parametric_env_to_string(clone);
  EXPECT_STREQ(caller_text, "{V: Choice::Some(u8:3)}");
  xls_c_str_free(caller_text);
}

TEST(XlsCApiTest, DslxBuiltinInvocationRetainsSumCallerBindings) {
  constexpr const char* kProgram = R"(
enum Choice { None, Some(u8) }
fn caller<V: Choice>() -> u8 { clz(u8:1) }
pub fn main() -> u8 { caller<{Choice::Some(u8:3)}>() }
)";
  auto* owner = xls_dslx_import_data_create(
      std::string(xls::kDefaultDslxStdlibPath).c_str(), nullptr, 0);
  xls_dslx_typechecked_module* tm = nullptr;
  xls_dslx_call_graph* graph = nullptr;
  xls_dslx_invocation_callee_data_array* array = nullptr;
  xls_dslx_invocation_callee_data* clone = nullptr;
  char* error = nullptr;
  absl::Cleanup cleanup([&] {
    xls_c_str_free(error);
    xls_dslx_invocation_callee_data_free(clone);
    xls_dslx_invocation_callee_data_array_free(array);
    xls_dslx_call_graph_free(graph);
    xls_dslx_typechecked_module_free(tm);
    xls_dslx_import_data_free(owner);
  });
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(
      kProgram, "builtin_caller.x", "builtin_caller", owner, &error, &tm))
      << error;
  auto* module = xls_dslx_typechecked_module_get_module(tm);
  auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
  auto* caller = xls_dslx_module_member_get_function(
      xls_dslx_module_get_member(module, 1));
  ASSERT_NE(caller, nullptr);
  ASSERT_TRUE(xls_dslx_type_info_build_function_call_graph_for_module(
      type_info, module, &error, &graph));
  ASSERT_EQ(xls_dslx_call_graph_get_callee_count(graph, caller), 1);
  auto* builtin = xls_dslx_call_graph_get_callee_function(graph, caller, 0);
  array =
      xls_dslx_type_info_get_unique_invocation_callee_data(type_info, builtin);
  ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
  auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
  EXPECT_EQ(xls_dslx_invocation_callee_data_get_derived_type_info(data),
            nullptr);
  clone = xls_dslx_invocation_callee_data_clone(data);
  xls_dslx_invocation_callee_data_array_free(array);
  array = nullptr;
  xls_dslx_call_graph_free(graph);
  graph = nullptr;
  xls_dslx_typechecked_module_free(tm);
  tm = nullptr;
  xls_dslx_import_data_free(owner);
  owner = nullptr;
  char* callee_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_callee_bindings(clone));
  EXPECT_STREQ(callee_text, "{N: u32:8}");
  xls_c_str_free(callee_text);
  char* caller_text = xls_dslx_parametric_env_to_string(
      xls_dslx_invocation_callee_data_get_caller_bindings(clone));
  EXPECT_STREQ(caller_text, "{V: Choice::Some(u8:3)}");
  xls_c_str_free(caller_text);
}

TEST(XlsCApiTest, DslxFunctionParamIntrospection) {
  const char kProgram[] = R"(enum MyE : u3 { A = u3:0, B = u3:1 }
fn top(x: u32, y: MyE) -> u32 { x }
)";
  const char* additional_search_paths[] = {};
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      std::string{xls::kDefaultDslxStdlibPath}.c_str(), additional_search_paths,
      0);
  ASSERT_NE(import_data, nullptr);

  xls_dslx_typechecked_module* tm = nullptr;
  char* error = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kProgram, "m.x", "m", import_data,
                                           &error, &tm))
      << (error ? error : "");
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(tm, nullptr);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  int64_t members = xls_dslx_module_get_member_count(module);

  xls_dslx_function* fn = nullptr;
  for (int64_t i = 0; i < members; ++i) {
    xls_dslx_module_member* mm = xls_dslx_module_get_member(module, i);
    fn = xls_dslx_module_member_get_function(mm);
    if (fn != nullptr) {
      break;
    }
  }
  ASSERT_NE(fn, nullptr);

  EXPECT_EQ(xls_dslx_function_get_param_count(fn), 2);

  xls_dslx_param* p0 = xls_dslx_function_get_param(fn, 0);
  xls_dslx_param* p1 = xls_dslx_function_get_param(fn, 1);
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);

  char* p0_name = xls_dslx_param_get_name(p0);
  char* p1_name = xls_dslx_param_get_name(p1);
  ASSERT_NE(p0_name, nullptr);
  ASSERT_NE(p1_name, nullptr);
  EXPECT_STREQ(p0_name, "x");
  EXPECT_STREQ(p1_name, "y");
  xls_c_str_free(p0_name);
  xls_c_str_free(p1_name);

  EXPECT_NE(xls_dslx_param_get_type_annotation(p0), nullptr);
  EXPECT_NE(xls_dslx_param_get_type_annotation(p1), nullptr);

  xls_dslx_typechecked_module_free(tm);
  xls_dslx_import_data_free(import_data);
}

TEST(XlsCApiTest, DslxStringLiteralAttribute) {
  const char* kDslx = R"DSLX(
#[dslx_format_disable("fmt-off")]
fn fmt_fn(x: u32) -> u32 { x }
)DSLX";
  const char* additional_search_paths[] = {};
  std::string dslx_stdlib_path = std::string(xls::kDefaultDslxStdlibPath);
  xls_dslx_import_data* import_data = xls_dslx_import_data_create(
      dslx_stdlib_path.c_str(), additional_search_paths, 0);
  ASSERT_NE(import_data, nullptr);
  absl::Cleanup free_import_data(
      [&]() { xls_dslx_import_data_free(import_data); });
  char* error = nullptr;
  xls_dslx_typechecked_module* tm = nullptr;
  ASSERT_TRUE(xls_dslx_parse_and_typecheck(kDslx, "attr_test.x", "attr_test",
                                           import_data, &error, &tm))
      << (error ? error : "");
  absl::Cleanup free_tm([&]() { xls_dslx_typechecked_module_free(tm); });
  xls_c_str_free(error);

  xls_dslx_module* module = xls_dslx_typechecked_module_get_module(tm);
  auto find_function = [&](std::string_view target) -> xls_dslx_function* {
    int64_t member_count = xls_dslx_module_get_member_count(module);
    for (int64_t i = 0; i < member_count; ++i) {
      xls_dslx_module_member* member = xls_dslx_module_get_member(module, i);
      xls_dslx_function* fn = xls_dslx_module_member_get_function(member);
      if (fn) {
        char* id = xls_dslx_function_get_identifier(fn);
        absl::Cleanup free_id([&]() { xls_c_str_free(id); });
        if (std::string_view(id) == target) {
          return fn;
        }
      }
    }
    return nullptr;
  };

  xls_dslx_function* fmt_fn = find_function("fmt_fn");
  ASSERT_NE(fmt_fn, nullptr);

  ASSERT_EQ(xls_dslx_function_get_attribute_count(fmt_fn), 1);
  xls_dslx_attribute* fmt_attr = xls_dslx_function_get_attribute(fmt_fn, 0);
  EXPECT_EQ(xls_dslx_attribute_get_kind(fmt_attr),
            xls_dslx_attribute_kind_dslx_format_disable);
  ASSERT_EQ(xls_dslx_attribute_get_argument_count(fmt_attr), 1);
  EXPECT_EQ(xls_dslx_attribute_get_argument_kind(fmt_attr, 0),
            xls_dslx_attribute_argument_kind_string_literal);
  char* fmt_arg = xls_dslx_attribute_get_string_literal_argument(fmt_attr, 0);
  absl::Cleanup free_fmt_arg([&]() { xls_c_str_free(fmt_arg); });
  EXPECT_EQ(std::string(fmt_arg), "fmt-off");
}

TEST(XlsCApiTest, IrAnalysisKnownBitsAndIntervalsByNodeId) {
  const std::string_view kIr = R"(package p
top fn f() -> bits[8] {
  ret lit: bits[8] = literal(value=42, id=1)
})";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(std::string(kIr).c_str(), "test.ir", &error,
                                   &package))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([&] { xls_package_free(package); });

  xls_ir_analysis* analysis = nullptr;
  ASSERT_TRUE(xls_ir_analysis_create_from_package(package, &error, &analysis))
      << "xls_ir_analysis_create_from_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(analysis, nullptr);
  absl::Cleanup free_analysis([&] { xls_ir_analysis_free(analysis); });

  xls_bits* known_mask = nullptr;
  xls_bits* known_value = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_known_bits_for_node_id(
      analysis, /*node_id=*/1, &error, &known_mask, &known_value))
      << "xls_ir_analysis_get_known_bits_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(known_mask, nullptr);
  ASSERT_NE(known_value, nullptr);
  absl::Cleanup free_known_bits([&] {
    xls_bits_free(known_mask);
    xls_bits_free(known_value);
  });

  char* known_mask_s = nullptr;
  char* known_value_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(known_mask, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_mask_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(known_value, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_value_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_known_bits_strs([&] {
    xls_c_str_free(known_mask_s);
    xls_c_str_free(known_value_s);
  });

  EXPECT_EQ(std::string_view(known_mask_s), "0xff [8 bits]");
  EXPECT_EQ(std::string_view(known_value_s), "0x2a [8 bits]");

  xls_interval_set* intervals = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_intervals_for_node_id(analysis, /*node_id=*/1,
                                                        &error, &intervals))
      << "xls_ir_analysis_get_intervals_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(intervals, nullptr);
  absl::Cleanup free_intervals([&] { xls_interval_set_free(intervals); });

  EXPECT_EQ(xls_interval_set_get_interval_count(intervals), 1);

  xls_bits* lo = nullptr;
  xls_bits* hi = nullptr;
  ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/0, &error,
                                                   &lo, &hi))
      << "xls_interval_set_get_interval_bounds error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(lo, nullptr);
  ASSERT_NE(hi, nullptr);
  absl::Cleanup free_bounds([&] {
    xls_bits_free(lo);
    xls_bits_free(hi);
  });

  char* lo_s = nullptr;
  char* hi_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(lo, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &lo_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(hi, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &hi_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_bounds_strs([&] {
    xls_c_str_free(lo_s);
    xls_c_str_free(hi_s);
  });

  EXPECT_EQ(std::string_view(lo_s), "0x2a [8 bits]");
  EXPECT_EQ(std::string_view(hi_s), "0x2a [8 bits]");
}

TEST(XlsCApiTest, IrAnalysisFullIntervalByNodeId) {
  const std::string_view kIr = R"(package p
top fn f(x: bits[8] id=1) -> bits[8] {
  ret y: bits[8] = identity(x, id=2)
})";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(std::string(kIr).c_str(), "test.ir", &error,
                                   &package))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([&] { xls_package_free(package); });

  xls_ir_analysis* analysis = nullptr;
  ASSERT_TRUE(xls_ir_analysis_create_from_package(package, &error, &analysis))
      << "xls_ir_analysis_create_from_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(analysis, nullptr);
  absl::Cleanup free_analysis([&] { xls_ir_analysis_free(analysis); });

  xls_bits* known_mask = nullptr;
  xls_bits* known_value = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_known_bits_for_node_id(
      analysis, /*node_id=*/2, &error, &known_mask, &known_value))
      << "xls_ir_analysis_get_known_bits_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(known_mask, nullptr);
  ASSERT_NE(known_value, nullptr);
  absl::Cleanup free_known_bits([&] {
    xls_bits_free(known_mask);
    xls_bits_free(known_value);
  });

  char* known_mask_s = nullptr;
  char* known_value_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(known_mask, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_mask_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(known_value, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_value_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_known_strs([&] {
    xls_c_str_free(known_mask_s);
    xls_c_str_free(known_value_s);
  });

  EXPECT_EQ(std::string_view(known_mask_s), "0x0 [8 bits]");
  EXPECT_EQ(std::string_view(known_value_s), "0x0 [8 bits]");

  xls_interval_set* intervals = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_intervals_for_node_id(analysis, /*node_id=*/2,
                                                        &error, &intervals))
      << "xls_ir_analysis_get_intervals_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(intervals, nullptr);
  absl::Cleanup free_intervals([&] { xls_interval_set_free(intervals); });

  EXPECT_EQ(xls_interval_set_get_interval_count(intervals), 1);

  xls_bits* lo = nullptr;
  xls_bits* hi = nullptr;
  ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/0, &error,
                                                   &lo, &hi))
      << "xls_interval_set_get_interval_bounds error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(lo, nullptr);
  ASSERT_NE(hi, nullptr);
  absl::Cleanup free_bounds([&] {
    xls_bits_free(lo);
    xls_bits_free(hi);
  });

  char* lo_s = nullptr;
  char* hi_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(lo, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &lo_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(hi, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &hi_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_bounds_strs([&] {
    xls_c_str_free(lo_s);
    xls_c_str_free(hi_s);
  });

  EXPECT_EQ(std::string_view(lo_s), "0x0 [8 bits]");
  EXPECT_EQ(std::string_view(hi_s), "0xff [8 bits]");
}

TEST(XlsCApiTest, IrAnalysisMultiIntervalByNodeId) {
  const std::string_view kIr = R"(package p
top fn f(s: bits[1] id=1) -> bits[8] {
  zero: bits[8] = literal(value=0, id=2)
  two: bits[8] = literal(value=2, id=3)
  ret result: bits[8] = sel(s, cases=[zero, two], id=4)
})";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(std::string(kIr).c_str(), "test.ir", &error,
                                   &package))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([&] { xls_package_free(package); });

  xls_ir_analysis* analysis = nullptr;
  ASSERT_TRUE(xls_ir_analysis_create_from_package(package, &error, &analysis))
      << "xls_ir_analysis_create_from_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(analysis, nullptr);
  absl::Cleanup free_analysis([&] { xls_ir_analysis_free(analysis); });

  xls_bits* known_mask = nullptr;
  xls_bits* known_value = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_known_bits_for_node_id(
      analysis, /*node_id=*/4, &error, &known_mask, &known_value))
      << "xls_ir_analysis_get_known_bits_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(known_mask, nullptr);
  ASSERT_NE(known_value, nullptr);
  absl::Cleanup free_known_bits([&] {
    xls_bits_free(known_mask);
    xls_bits_free(known_value);
  });

  char* known_mask_s = nullptr;
  char* known_value_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(known_mask, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_mask_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(known_value, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error,
                                 &known_value_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_known_strs([&] {
    xls_c_str_free(known_mask_s);
    xls_c_str_free(known_value_s);
  });

  // Possible values are {0, 2}, so all bits are known to be 0 except bit 1.
  EXPECT_EQ(std::string_view(known_mask_s), "0xfd [8 bits]");
  EXPECT_EQ(std::string_view(known_value_s), "0x0 [8 bits]");

  xls_interval_set* intervals = nullptr;
  ASSERT_TRUE(xls_ir_analysis_get_intervals_for_node_id(analysis, /*node_id=*/4,
                                                        &error, &intervals))
      << "xls_ir_analysis_get_intervals_for_node_id error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(intervals, nullptr);
  absl::Cleanup free_intervals([&] { xls_interval_set_free(intervals); });

  EXPECT_EQ(xls_interval_set_get_interval_count(intervals), 2);

  xls_bits* lo0 = nullptr;
  xls_bits* hi0 = nullptr;
  ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/0, &error,
                                                   &lo0, &hi0))
      << "xls_interval_set_get_interval_bounds error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(lo0, nullptr);
  ASSERT_NE(hi0, nullptr);
  absl::Cleanup free_bounds0([&] {
    xls_bits_free(lo0);
    xls_bits_free(hi0);
  });

  xls_bits* lo1 = nullptr;
  xls_bits* hi1 = nullptr;
  ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/1, &error,
                                                   &lo1, &hi1))
      << "xls_interval_set_get_interval_bounds error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(lo1, nullptr);
  ASSERT_NE(hi1, nullptr);
  absl::Cleanup free_bounds1([&] {
    xls_bits_free(lo1);
    xls_bits_free(hi1);
  });

  char* lo0_s = nullptr;
  char* hi0_s = nullptr;
  char* lo1_s = nullptr;
  char* hi1_s = nullptr;
  ASSERT_TRUE(xls_bits_to_string(lo0, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &lo0_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(hi0, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &hi0_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(lo1, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &lo1_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_TRUE(xls_bits_to_string(hi1, xls_format_preference_hex,
                                 /*include_bit_count=*/true, &error, &hi1_s))
      << "xls_bits_to_string error: " << error;
  ASSERT_EQ(error, nullptr);
  absl::Cleanup free_bounds_strs([&] {
    xls_c_str_free(lo0_s);
    xls_c_str_free(hi0_s);
    xls_c_str_free(lo1_s);
    xls_c_str_free(hi1_s);
  });

  EXPECT_EQ(std::string_view(lo0_s), "0x0 [8 bits]");
  EXPECT_EQ(std::string_view(hi0_s), "0x0 [8 bits]");
  EXPECT_EQ(std::string_view(lo1_s), "0x2 [8 bits]");
  EXPECT_EQ(std::string_view(hi1_s), "0x2 [8 bits]");
}

TEST(XlsCApiTest, IrAnalysisBddPredicateQueriesByNodeId) {
  const std::string_view kIr = R"(package p
top fn f(x: bits[8] id=1) -> bits[1] {
  zero: bits[8] = literal(value=0, id=2)
  one: bits[8] = literal(value=1, id=3)
  two: bits[8] = literal(value=2, id=4)
  x_eq_0: bits[1] = eq(x, zero, id=5)
  x_ne_0: bits[1] = not(x_eq_0, id=6)
  x_eq_1: bits[1] = eq(x, one, id=7)
  x_lt_2: bits[1] = ult(x, two, id=8)
  exclusive_eqs: bits[2] = concat(x_eq_0, x_eq_1, id=9)
  exhaustive_pair: bits[2] = concat(x_eq_0, x_ne_0, id=10)
  ret result: bits[1] = identity(x_lt_2, id=11)
})";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(std::string(kIr).c_str(), "test.ir", &error,
                                   &package))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([&] { xls_package_free(package); });

  xls_ir_analysis* analysis = nullptr;
  ASSERT_TRUE(xls_ir_analysis_create_from_package(package, &error, &analysis))
      << "xls_ir_analysis_create_from_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(analysis, nullptr);
  absl::Cleanup free_analysis([&] { xls_ir_analysis_free(analysis); });

  bool result = false;
  ASSERT_TRUE(xls_ir_analysis_at_most_one_bit_true(analysis, /*node_id=*/9,
                                                   &error, &result))
      << "xls_ir_analysis_at_most_one_bit_true error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_TRUE(result);

  ASSERT_TRUE(xls_ir_analysis_at_least_one_bit_true(analysis, /*node_id=*/9,
                                                    &error, &result))
      << "xls_ir_analysis_at_least_one_bit_true error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_FALSE(result);

  ASSERT_TRUE(xls_ir_analysis_at_least_one_bit_true(analysis, /*node_id=*/10,
                                                    &error, &result))
      << "xls_ir_analysis_at_least_one_bit_true error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_TRUE(result);

  ASSERT_TRUE(xls_ir_analysis_exactly_one_bit_true(analysis, /*node_id=*/10,
                                                   &error, &result))
      << "xls_ir_analysis_exactly_one_bit_true error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_TRUE(result);

  ASSERT_TRUE(xls_ir_analysis_exactly_one_bit_true(analysis, /*node_id=*/9,
                                                   &error, &result))
      << "xls_ir_analysis_exactly_one_bit_true error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_FALSE(result);

  ASSERT_TRUE(xls_ir_analysis_known_not_equals(
      analysis, /*lhs_node_id=*/10, /*lhs_bit_index=*/1, /*rhs_node_id=*/10,
      /*rhs_bit_index=*/0, &error, &result))
      << "xls_ir_analysis_known_not_equals error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_TRUE(result);

  ASSERT_TRUE(xls_ir_analysis_known_not_equals(
      analysis, /*lhs_node_id=*/9, /*lhs_bit_index=*/1, /*rhs_node_id=*/9,
      /*rhs_bit_index=*/0, &error, &result))
      << "xls_ir_analysis_known_not_equals error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_FALSE(result);

  ASSERT_TRUE(xls_ir_analysis_implies(analysis, /*lhs_node_id=*/9,
                                      /*lhs_bit_index=*/1, /*rhs_node_id=*/8,
                                      /*rhs_bit_index=*/0, &error, &result))
      << "xls_ir_analysis_implies error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_TRUE(result);

  ASSERT_TRUE(xls_ir_analysis_implies(analysis, /*lhs_node_id=*/8,
                                      /*lhs_bit_index=*/0, /*rhs_node_id=*/9,
                                      /*rhs_bit_index=*/1, &error, &result))
      << "xls_ir_analysis_implies error: " << error;
  ASSERT_EQ(error, nullptr);
  EXPECT_FALSE(result);
}

TEST(XlsCApiTest, IrAnalysisOptionsEnableContextSensitiveRange) {
  const std::string_view kIr = R"(package p
top fn f(x: bits[4] id=1) -> bits[4] {
  k: bits[4] = literal(value=2, id=2)
  p: bits[1] = sgt(x, k, id=3)
  ret y: bits[4] = sel(p, cases=[k, x], id=4)
})";

  char* error = nullptr;
  xls_package* package = nullptr;
  ASSERT_TRUE(xls_parse_ir_package(std::string(kIr).c_str(), "test.ir", &error,
                                   &package))
      << "xls_parse_ir_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(package, nullptr);
  absl::Cleanup free_package([&] { xls_package_free(package); });

  // Default analysis (fast).
  xls_ir_analysis* default_analysis = nullptr;
  ASSERT_TRUE(
      xls_ir_analysis_create_from_package(package, &error, &default_analysis))
      << "xls_ir_analysis_create_from_package error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(default_analysis, nullptr);
  absl::Cleanup free_default_analysis(
      [&] { xls_ir_analysis_free(default_analysis); });

  // Context-sensitive analysis.
  xls_ir_analysis* context_analysis = nullptr;
  xls_ir_analysis_options options;
  options.level = xls_ir_analysis_level_range_with_context;
  ASSERT_TRUE(xls_ir_analysis_create_from_package_with_options(
      package, &options, &error, &context_analysis))
      << "xls_ir_analysis_create_from_package_with_options error: " << error;
  ASSERT_EQ(error, nullptr);
  ASSERT_NE(context_analysis, nullptr);
  absl::Cleanup free_context_analysis(
      [&] { xls_ir_analysis_free(context_analysis); });

  // Node id 4 is the clamp select: sel(sgt(x, 2), cases=[2, x]).
  uint64_t default_lo = 0;
  uint64_t default_hi = 0;
  {
    xls_interval_set* intervals = nullptr;
    ASSERT_TRUE(xls_ir_analysis_get_intervals_for_node_id(
        default_analysis, /*node_id=*/4, &error, &intervals))
        << "xls_ir_analysis_get_intervals_for_node_id error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_NE(intervals, nullptr);
    absl::Cleanup free_intervals([&] { xls_interval_set_free(intervals); });

    ASSERT_EQ(xls_interval_set_get_interval_count(intervals), 1);

    xls_bits* lo = nullptr;
    xls_bits* hi = nullptr;
    ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/0, &error,
                                                     &lo, &hi))
        << "xls_interval_set_get_interval_bounds error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_NE(lo, nullptr);
    ASSERT_NE(hi, nullptr);
    absl::Cleanup free_bounds([&] {
      xls_bits_free(lo);
      xls_bits_free(hi);
    });

    ASSERT_TRUE(xls_bits_to_uint64(lo, &error, &default_lo))
        << "xls_bits_to_uint64(lo) error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_TRUE(xls_bits_to_uint64(hi, &error, &default_hi))
        << "xls_bits_to_uint64(hi) error: " << error;
    ASSERT_EQ(error, nullptr);
  }

  uint64_t context_lo = 0;
  uint64_t context_hi = 0;
  {
    xls_interval_set* intervals = nullptr;
    ASSERT_TRUE(xls_ir_analysis_get_intervals_for_node_id(
        context_analysis, /*node_id=*/4, &error, &intervals))
        << "xls_ir_analysis_get_intervals_for_node_id error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_NE(intervals, nullptr);
    absl::Cleanup free_intervals([&] { xls_interval_set_free(intervals); });

    ASSERT_EQ(xls_interval_set_get_interval_count(intervals), 1);

    xls_bits* lo = nullptr;
    xls_bits* hi = nullptr;
    ASSERT_TRUE(xls_interval_set_get_interval_bounds(intervals, /*i=*/0, &error,
                                                     &lo, &hi))
        << "xls_interval_set_get_interval_bounds error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_NE(lo, nullptr);
    ASSERT_NE(hi, nullptr);
    absl::Cleanup free_bounds([&] {
      xls_bits_free(lo);
      xls_bits_free(hi);
    });

    ASSERT_TRUE(xls_bits_to_uint64(lo, &error, &context_lo))
        << "xls_bits_to_uint64(lo) error: " << error;
    ASSERT_EQ(error, nullptr);
    ASSERT_TRUE(xls_bits_to_uint64(hi, &error, &context_hi))
        << "xls_bits_to_uint64(hi) error: " << error;
    ASSERT_EQ(error, nullptr);
  }

  // The clamp implies the result is always >= 2, and always <= 7.
  EXPECT_EQ(context_lo, 2);
  EXPECT_EQ(context_hi, 7);

  // Default analysis is expected to be weaker (at least on the lower bound).
  EXPECT_LT(default_lo, 2);
  EXPECT_GE(default_hi, context_hi);
}

}  // namespace
