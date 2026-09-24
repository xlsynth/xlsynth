// Copyright 2023 The XLS Authors
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

#include "xls/dslx/ir_convert/function_converter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/codegen/combinational_generator.h"
#include "xls/common/proto_test_utils.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/bytecode/bytecode.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/ir_convert/conversion_info.h"
#include "xls/dslx/ir_convert/convert_options.h"
#include "xls/dslx/ir_convert/ir_converter.h"
#include "xls/dslx/ir_convert/test_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/clone_package.h"
#include "xls/ir/function.h"
#include "xls/ir/nodes.h"
#include "xls/ir/package.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/xls_ir_interface.pb.h"
#include "xls/jit/function_jit.h"
#include "xls/passes/optimization_pass_pipeline.h"
#include "xls/simulation/default_verilog_simulator.h"
#include "xls/simulation/module_simulator.h"

namespace xls::dslx {
namespace {
using ::xls::proto_testing::EqualsProto;

void ExpectIr(std::string_view got) {
  return ::xls::dslx::ExpectIr(got, TestName(), "function_converter_test");
}

PackageConversionData MakeConversionData(std::string_view n) {
  return {.package = std::make_unique<Package>(n)};
}

bool NodeExpressionContainsOp(const xls::Node* node, xls::Op op) {
  if (node->op() == op) {
    return true;
  }
  for (const xls::Node* operand : node->operands()) {
    if (NodeExpressionContainsOp(operand, op)) {
      return true;
    }
  }
  return false;
}

bool SelectCaseContainsOp(const xls::Select& select, xls::Op op) {
  for (const xls::Node* case_node : select.cases()) {
    if (NodeExpressionContainsOp(case_node, op)) {
      return true;
    }
  }
  if (select.default_value().has_value()) {
    return NodeExpressionContainsOp(*select.default_value(), op);
  }
  return false;
}

TEST(FunctionConverterTest, ConvertsSimpleFunctionWithoutError) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("fn f() -> u32 { u32:42 }", "test_module.x",
                        "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  result_type { type_enum: BITS bit_count: 32 }
                }
              )pb"));
}
TEST(FunctionConverterTest, ConvertsSimpleFunctionWithAsserts) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(fn f() -> () {
        assert!(u32:42 == u32:31 + u32:1, "foo");
        assert_eq(u32:42, u32:31 + u32:1);
        assert_lt(u32:41, u32:31 + u32:1);
      })",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__itok__test_module__f" }
                  parameters {
                    name: "__token"
                    type { type_enum: TOKEN }
                  }
                  parameters {
                    name: "__activated"
                    type { type_enum: BITS bit_count: 1 }
                  }
                  result_type {
                    type_enum: TUPLE
                    tuple_elements { type_enum: TOKEN }
                    tuple_elements { type_enum: TUPLE }
                  }
                }
                functions { base { name: "__test_module__f" } }
              )pb"));
}

TEST(FunctionConverterTest, TracksMultipleTypeAliasSvType) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#[sv_type("something::cool")]
                           type FooBar = u32;
                           type Baz = u32;
                           fn f(b: Baz) -> FooBar { b + u32:42 })",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "b"
                    type { type_enum: BITS bit_count: 32 }
                  }
                  result_type { type_enum: BITS bit_count: 32 }
                  sv_result_type: "something::cool"
                }
              )pb"));
}

TEST(FunctionConverterTest, TracksTypeAliasSvType) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#[sv_type("something::cool")]
                           type FooBar = u32;
                           #[sv_type("even::cooler")]
                           type Baz = u32;
                           fn f(b: Baz) -> FooBar { b + u32:42 })",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "b"
                    type { type_enum: BITS bit_count: 32 }
                    sv_type: "even::cooler"
                  }
                  result_type { type_enum: BITS bit_count: 32 }
                  sv_result_type: "something::cool"
                }
              )pb"));
}

TEST(FunctionConverterTest, TracksTypeAliasStopsAtFirstSvType) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
#[sv_type("something::cool")]
type FooBar = u32;
#[sv_type("even::cooler")]
type Baz = FooBar;
fn f(b: Baz) -> FooBar { b + u32:42 })",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "b"
                    type { type_enum: BITS bit_count: 32 }
                    sv_type: "even::cooler"
                  }
                  result_type { type_enum: BITS bit_count: 32 }
                  sv_result_type: "something::cool"
                }
              )pb"));
}

TEST(FunctionConverterTest, ExternFunctionAttributePreservedInIR) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
#[extern_verilog("extern_foobar {fn} (.out({return}));")]
fn f() -> u32 { u32:42 }
)",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  // We expect a single function, that contains the FFI info for "extern_foobar"
  ASSERT_FALSE(package_data.conversion_info->package->functions().empty());
  ASSERT_TRUE(package_data.conversion_info->package->functions()
                  .front()
                  ->ForeignFunctionData());
  EXPECT_EQ(package_data.conversion_info->package->functions()
                .front()
                ->ForeignFunctionData()
                ->code_template(),
            "extern_foobar {fn} (.out({return}));");
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  result_type { type_enum: BITS bit_count: 32 }
                }
              )pb"));
}

TEST(FunctionConverterTest, ConvertsLastExprAndImplicitTokenWithoutError) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
fn f() {
    let acc: u32 = u32:0;
    for (i, acc): (u32, u32) in u32:0..u32:8 {
        let acc = acc + i;
        trace_fmt!("Do nothing");
        acc
    }(acc);
}
)",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  EXPECT_THAT(package.interface.functions(),
              testing::UnorderedElementsAre(
                  EqualsProto(R"pb(
                    base { top: true name: "__itok__test_module__f" }
                    parameters {
                      name: "__token"
                      type { type_enum: TOKEN }
                    }
                    parameters {
                      name: "__activated"
                      type { type_enum: BITS bit_count: 1 }
                    }
                    result_type {
                      type_enum: TUPLE
                      tuple_elements { type_enum: TOKEN }
                      tuple_elements { type_enum: TUPLE }
                    })pb"),
                  EqualsProto(R"pb(
                    base { name: "____itok__test_module__f_counted_for_0_body" }
                    parameters {
                      name: "i"
                      type { type_enum: BITS bit_count: 32 }
                    }
                    parameters {
                      name: "__token_wrapped"
                      type {
                        type_enum: TUPLE
                        tuple_elements { type_enum: TOKEN }
                        tuple_elements { type_enum: BITS bit_count: 1 }
                        tuple_elements { type_enum: BITS bit_count: 32 }
                      }
                    }
                  )pb"),
                  EqualsProto(R"pb(
                    base { name: "__test_module__f" }
                  )pb")));
}

TEST(FunctionConverterTest,
     ConvertsLastExprAndImplicitTokenWithoutErrorWithProcScopedChannels) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(
fn f() {
    let acc: u32 = u32:0;
    for (i, acc): (u32, u32) in u32:0..u32:8 {
        let acc = acc + i;
        trace_fmt!("Do nothing");
        acc
    }(acc);
}
)",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->extern_verilog_module().has_value());

  const ConvertOptions convert_options = {.lower_to_proc_scoped_channels =
                                              true};
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  EXPECT_THAT(package.interface.functions(),
              testing::UnorderedElementsAre(
                  EqualsProto(R"pb(
                    base { top: true name: "__itok__test_module__f" }
                    parameters {
                      name: "__token"
                      type { type_enum: TOKEN }
                    }
                    parameters {
                      name: "__activated"
                      type { type_enum: BITS bit_count: 1 }
                    }
                    result_type {
                      type_enum: TUPLE
                      tuple_elements { type_enum: TOKEN }
                      tuple_elements { type_enum: TUPLE }
                    })pb"),
                  EqualsProto(R"pb(
                    base { name: "____itok__test_module__f_counted_for_0_body" }
                    parameters {
                      name: "i"
                      type { type_enum: BITS bit_count: 32 }
                    }
                    parameters {
                      name: "__token_wrapped"
                      type {
                        type_enum: TUPLE
                        tuple_elements { type_enum: TOKEN }
                        tuple_elements { type_enum: BITS bit_count: 1 }
                        tuple_elements { type_enum: BITS bit_count: 32 }
                      }
                    }
                  )pb"),
                  EqualsProto(R"pb(
                    base { name: "__test_module__f" }
                  )pb")));
}

TEST(FunctionConverterTest, ConvertsFunctionWithZipBuiltin) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(
          "fn f(x: u32[2], y: u64[2]) -> (u32, u64)[2] { zip(x, y) }",
          "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "x"
                    type {
                      type_enum: ARRAY
                      array_size: 2
                      array_element { type_enum: BITS bit_count: 32 }
                    }
                  }
                  parameters {
                    name: "y"
                    type {
                      type_enum: ARRAY
                      array_size: 2
                      array_element { type_enum: BITS bit_count: 64 }
                    }
                  }
                  result_type {
                    type_enum: ARRAY
                    array_size: 2
                    array_element {
                      type_enum: TUPLE
                      tuple_elements { type_enum: BITS bit_count: 32 }
                      tuple_elements { type_enum: BITS bit_count: 64 }
                    }
                  }
                }
              )pb"));
}

TEST(FunctionConverterTest, ConvertsFunctionWithUpdate2DBuiltin) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("fn f(a: u32[2][3]) -> u32[2][3] { update(a, (u1:1, "
                        "u32:0), u32:42) }",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "a"
                    type {
                      type_enum: ARRAY
                      array_size: 3
                      array_element {
                        type_enum: ARRAY
                        array_size: 2
                        array_element { type_enum: BITS bit_count: 32 }
                      }
                    }
                  }
                  result_type {
                    type_enum: ARRAY
                    array_size: 3
                    array_element {
                      type_enum: ARRAY
                      array_size: 2
                      array_element { type_enum: BITS bit_count: 32 }
                    }
                  }
                }
              )pb"));
}

TEST(FunctionConverterTest, ConvertsFunctionWithUpdate2DBuiltinEmptyTuple) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("fn f(a: u32[2][3]) -> u32[2][3] { update(a, (), a) }",
                        "test_module.x", "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
  ExpectIr(package.DumpIr());
  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "a"
                    type {
                      type_enum: ARRAY
                      array_size: 3
                      array_element {
                        type_enum: ARRAY
                        array_size: 2
                        array_element { type_enum: BITS bit_count: 32 }
                      }
                    }
                  }
                  result_type {
                    type_enum: ARRAY
                    array_size: 3
                    array_element {
                      type_enum: ARRAY
                      array_size: 2
                      array_element { type_enum: BITS bit_count: 32 }
                    }
                  }
                }
              )pb"));
}

TEST(FunctionConverterTest,
     ConvertsImportedSumConstantWithoutConstructorDispatch) {
  constexpr std::string_view kImported = R"(
pub enum Option {
  None,
  Some(u32),
}

pub const SOME: Option = Option::Some(u32:7);
)";
  constexpr std::string_view kProgram = R"(
import imported;

fn f() -> imported::Option {
  imported::SOME
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(
      ParseAndTypecheck(kImported, "imported.x", "imported", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_EQ(package_data.ir_to_dslx.size(), 1);
}

TEST(FunctionConverterTest, LowersSemanticSumAsSharedPayloadSlot) {
  constexpr std::string_view kProgram = R"(
enum Message: u3 {
  Idle = 0,
  Request(u8) = 3,
  Response(u32) = 7,
}

fn f(x: Message) -> Message {
  x
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.interface, EqualsProto(R"pb(
                functions {
                  base { top: true name: "__test_module__f" }
                  parameters {
                    name: "x"
                    type {
                      type_enum: TUPLE
                      tuple_elements { type_enum: BITS bit_count: 3 }
                      tuple_elements {
                        type_enum: TUPLE
                        tuple_elements { type_enum: BITS bit_count: 32 }
                      }
                    }
                  }
                  result_type {
                    type_enum: TUPLE
                    tuple_elements { type_enum: BITS bit_count: 3 }
                    tuple_elements {
                      type_enum: TUPLE
                      tuple_elements { type_enum: BITS bit_count: 32 }
                    }
                  }
                }
              )pb"));
}

TEST(FunctionConverterTest, ConstexprAndRuntimeSumsShare17BitAbiAcrossEngines) {
  constexpr std::string_view kProgram = R"(
enum E { A(u8), B(u16) }

const CONST_A: E = E::A(u8:0xab);
const CONST_B: E = E::B(u16:0xcdef);

fn f(x: u8, y: u16) -> (E, E, E, E, bool, bool, bool, u16, u16, u32) {
  let runtime_a = E::A(x);
  let runtime_b = E::B(y);
  let matched_a = match runtime_a { E::A(v) => v as u16, E::B(v) => v };
  let matched_b = match runtime_b { E::A(v) => v as u16, E::B(v) => v };
  (CONST_A, CONST_B, runtime_a, runtime_b,
   runtime_a == CONST_A, runtime_b == CONST_B, runtime_a != runtime_b,
   matched_a, matched_b, bit_count<E>())
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           tm.module->GetMemberOrError<Function>("f"));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<BytecodeFunction> bytecode,
                           BytecodeEmitter::Emit(&import_data, tm.type_info,
                                                 *function, ParametricEnv{}));

  PackageConversionData package = MakeConversionData("test_module_package");
  XLS_ASSERT_OK(ConvertOneFunctionIntoPackage(function, &import_data,
                                              /*parametric_env=*/nullptr,
                                              ConvertOptions(), &package));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(ir_function));

  // The inherited separate u8/u16 payload slots would use 25 bits. The two
  // constants and two runtime results must each use one shared 16-bit slot.
  ASSERT_TRUE(ir_function->return_type()->IsTuple());
  const xls::TupleType* result_type =
      ir_function->return_type()->AsTupleOrDie();
  for (int64_t i = 0; i < 4; ++i) {
    const xls::Type* sum_type = result_type->element_type(i);
    EXPECT_EQ(sum_type->ToString(), "(bits[1], (bits[16]))");
    EXPECT_EQ(sum_type->GetFlatBitCount(), 17);
  }
  auto encoded_sum = [](bool tag, uint64_t payload) {
    return Value::Tuple(
        {Value::Bool(tag), Value::Tuple({Value(UBits(payload, 16))})});
  };
  struct Case {
    uint64_t x;
    uint64_t y;
    bool a_equals_constant;
    bool b_equals_constant;
  };
  for (const Case& test_case :
       {Case{0xab, 0xcdef, true, true}, Case{0xac, 0xcdee, false, false},
        Case{0xab, 0xab, true, false}}) {
    SCOPED_TRACE(testing::Message() << test_case.x << ", " << test_case.y);
    const Value expected = Value::Tuple(
        {encoded_sum(false, 0xab), encoded_sum(true, 0xcdef),
         encoded_sum(false, test_case.x), encoded_sum(true, test_case.y),
         Value::Bool(test_case.a_equals_constant),
         Value::Bool(test_case.b_equals_constant), Value::Bool(true),
         Value(UBits(test_case.x, 16)), Value(UBits(test_case.y, 16)),
         Value(UBits(17, 32))});

    XLS_ASSERT_OK_AND_ASSIGN(InterpValue bytecode_result,
                             BytecodeInterpreter::Interpret(
                                 &import_data, bytecode.get(),
                                 {InterpValue::MakeUBits(8, test_case.x),
                                  InterpValue::MakeUBits(16, test_case.y)}));
    EXPECT_THAT(bytecode_result.ConvertToIr(),
                ::absl_testing::IsOkAndHolds(expected));

    const std::vector<Value> args = {Value(UBits(test_case.x, 8)),
                                     Value(UBits(test_case.y, 16))};
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> ir_result,
                             InterpretFunction(ir_function, args));
    EXPECT_EQ(ir_result.value, expected);
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jit_result,
                             jit->Run(args));
    EXPECT_EQ(jit_result.value, expected);
  }
}

TEST(FunctionConverterTest, UsesSemanticDiscriminantsForSparseSumLowering) {
  constexpr std::string_view kProgram = R"(
enum Message: u3 {
  Idle = 0,
  Request(u8) = 3,
  Response(u32) = 7,
}

fn f(x: Message) -> Message {
  match x {
    Message::Request(v) => Message::Request(v),
    Message::Response(v) => Message::Response(v),
    Message::Idle => Message::Idle,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::HasSubstr("literal(value=3"));
  EXPECT_THAT(package.DumpIr(), testing::HasSubstr("literal(value=7"));
}

// Verifies: Function boundaries preserve dirty and malformed sum images.
// Catches: Implicit padding normalization on parameters or returns.
TEST(FunctionConverterTest, PreservesSumBoundaryPaddingAndMalformedImages) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 {
  Small(u4) = 0,
  Big(u8) = 1,
}

fn f(x: Message) -> Message {
  x
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value noncanonical = Value::Tuple(
      {Value(UBits(/*value=*/0, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xfa, /*bit_count=*/8))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> valid_result,
                           InterpretFunction(ir_function, {noncanonical}));
  EXPECT_EQ(valid_result.value, noncanonical);

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xfa, /*bit_count=*/8))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> malformed_result,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(malformed_result.value, malformed);
}

TEST(FunctionConverterTest, PreservesMalformedExplicitSingleVariantSumImage) {
  constexpr std::string_view kProgram = R"(
enum Inner {
  Small(u1),
  Big(u2),
}

enum Outer: u1 {
  Wrap(Inner) = 0,
}

fn f(x: Outer) -> u4 {
  match x {
    Outer::Wrap(_) => u4:0,
    invalid!(raw) => raw,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/1, /*bit_count=*/1)),
       Value::Tuple({Value(UBits(/*value=*/0b011, /*bit_count=*/3))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(result.value, Value(UBits(/*value=*/0b1011, /*bit_count=*/4)));
}

TEST(FunctionConverterTest, InvalidRawPatternBindsTagThenPayloadBits) {
  constexpr std::string_view kProgram = R"(
enum Option: u2 {
  None = 0,
  Some(u32) = 1,
}

fn f(x: Option) -> u34 {
  match x {
    Option::None => u34:0,
    Option::Some(_) => u34:0,
    invalid!(raw) => raw,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  bool has_tag_then_payload_concat = false;
  for (xls::Node* node : ir_function->nodes()) {
    if (node->op() != xls::Op::kConcat || node->operand_count() != 2) {
      continue;
    }
    xls::Node* tag = node->operand(0);
    xls::Node* payload = node->operand(1);
    if (tag->op() == xls::Op::kTupleIndex &&
        payload->op() == xls::Op::kTupleIndex &&
        payload->operand(0)->op() == xls::Op::kTupleIndex) {
      has_tag_then_payload_concat = true;
    }
  }
  EXPECT_TRUE(has_tag_then_payload_concat) << package.DumpIr();

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0x12345678, /*bit_count=*/32))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(result.value,
            Value(UBits(/*value=*/0x312345678, /*bit_count=*/34)));
}

TEST(FunctionConverterTest,
     MalformedSumEqualityUsesLastConstructorPayloadAndTag) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 {
  Wide(u8) = 0,
  Narrow(u4) = 1,
}

fn f(x: Message, y: Message) -> bool {
  x == y
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xfa, /*bit_count=*/8))})});
  const Value same_tag_and_active_payload = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0x0a, /*bit_count=*/8))})});
  const Value other_invalid_tag = Value::Tuple(
      {Value(UBits(/*value=*/2, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xfa, /*bit_count=*/8))})});

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpreterResult<Value> equal,
      InterpretFunction(ir_function, {malformed, same_tag_and_active_payload}));
  EXPECT_EQ(equal.value, Value(UBits(/*value=*/1, /*bit_count=*/1)));

  XLS_ASSERT_OK_AND_ASSIGN(
      InterpreterResult<Value> unequal,
      InterpretFunction(ir_function, {malformed, other_invalid_tag}));
  EXPECT_EQ(unequal.value, Value(UBits(/*value=*/0, /*bit_count=*/1)));
}

TEST(FunctionConverterTest, MalformedSumUsesFinalConstructorPayloadFallback) {
  constexpr std::string_view kProgram = R"(
enum Option: u2 {
  None = 0,
  Some(u8) = 1,
}

fn f(x: Option) -> u8 {
  match x {
    Option::None => u8:0,
    Option::Some(value) => value,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xa5, /*bit_count=*/8))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(result.value, Value(UBits(/*value=*/0xa5, /*bit_count=*/8)));
}

TEST(FunctionConverterTest, ExpandsSemanticSumEqIntoTagAndPayloadChecks) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
  Pair(u32, u32),
}

fn f(x: Option, y: Option) -> bool {
  x == y
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  int64_t tuple_index_count = 0;
  int64_t eq_count = 0;
  int64_t eq_literal_count = 0;
  int64_t equality_select_count = 0;
  bool has_direct_param_eq = false;
  for (xls::Node* node : ir_function->nodes()) {
    if (node->op() == xls::Op::kTupleIndex) {
      ++tuple_index_count;
    }
    if (node->op() == xls::Op::kSel) {
      const auto* select = node->As<xls::Select>();
      if (SelectCaseContainsOp(*select, xls::Op::kEq)) {
        ++equality_select_count;
      }
    }
    if (node->op() == xls::Op::kEq) {
      ++eq_count;
      if (node->operand(0)->op() == xls::Op::kLiteral ||
          node->operand(1)->op() == xls::Op::kLiteral) {
        ++eq_literal_count;
      }
      if (node->operand(0)->op() == xls::Op::kParam &&
          node->operand(1)->op() == xls::Op::kParam) {
        has_direct_param_eq = true;
      }
    }
  }
  EXPECT_GE(tuple_index_count, 4);
  EXPECT_GT(eq_count, 1);
  EXPECT_GE(equality_select_count, 2);
  EXPECT_GT(eq_literal_count, 0);
  EXPECT_FALSE(has_direct_param_eq);
}

TEST(FunctionConverterTest, SemanticSumMatchUsesPhase2FallbackWithoutToken) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    _ => u32:0,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(tm.type_info->GetRequiresImplicitToken(*f).value_or(false));

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::Not(testing::HasSubstr("assert(")));
  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("__itok__test_module__f")));
  std::string interface_text = package.interface.DebugString();
  EXPECT_THAT(
      interface_text,
      testing::Not(testing::HasSubstr("name: \"__itok__test_module__f\"")));
  EXPECT_THAT(interface_text, testing::HasSubstr("name: \"__test_module__f\""));
}

TEST(FunctionConverterTest,
     ExhaustiveSemanticSumMatchWithoutWildcardUsesPhase2FallbackWithoutToken) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    Option::None => u32:0,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(tm.type_info->GetRequiresImplicitToken(*f).value_or(false));

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::Not(testing::HasSubstr("assert(")));
  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("__itok__test_module__f")));
  std::string interface_text = package.interface.DebugString();
  EXPECT_THAT(
      interface_text,
      testing::Not(testing::HasSubstr("name: \"__itok__test_module__f\"")));
  EXPECT_THAT(interface_text, testing::HasSubstr("name: \"__test_module__f\""));
}

TEST(FunctionConverterTest,
     RejectsBindingInLaterSemanticSumOrPatternBeforeConversion) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::None | Option::Some(v) => v,
    _ => u32:0,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  EXPECT_THAT(
      ParseAndTypecheck(kProgram, "test_module.x", "test_module", &import_data),
      ::absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          testing::AllOf(
              testing::HasSubstr("Cannot bind names in a match arm with "
                                 "multiple patterns"),
              testing::HasSubstr("bound: v"))));
}

TEST(FunctionConverterTest, SemanticSumEqualityDoesNotRequireImplicitToken) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option, y: Option) -> bool {
  x == y
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(tm.type_info->GetRequiresImplicitToken(*f).value_or(false));

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("phase1_sum_equality")));
  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("__itok__test_module__f")));
}

TEST(FunctionConverterTest, SemanticSumInequalityDoesNotRequireImplicitToken) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option, y: Option) -> bool {
  x != y
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(tm.type_info->GetRequiresImplicitToken(*f).value_or(false));

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("phase1_sum_inequality")));
  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("__itok__test_module__f")));
}

TEST(FunctionConverterTest,
     SemanticSumAssertEqOmitsPhase1WellFormednessAssert) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option, y: Option) -> () {
  assert_eq(x, y)
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(tm.type_info->GetRequiresImplicitToken(*f).value_or(false));

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::HasSubstr("assert("));
  EXPECT_THAT(package.DumpIr(),
              testing::Not(testing::HasSubstr("phase1_sum_assert_eq")));
}

TEST(FunctionConverterTest, UsesAggregateEqForNonSumArrayPayloadSubtrees) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: (Option, u32[4]), y: (Option, u32[4])) -> bool {
  x == y
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);

  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetTopAsFunction());
  int64_t array_index_count = 0;
  for (xls::Node* node : ir_function->nodes()) {
    if (node->op() == xls::Op::kArrayIndex) {
      ++array_index_count;
    }
  }
  EXPECT_EQ(array_index_count, 0);
}

TEST(FunctionConverterTest,
     ConvertsSemanticSumConstructorWithInactiveEmptySumPayloadInPhase1) {
  constexpr std::string_view kProgram = R"(
enum Empty {
}

enum Outer {
  Wrapped(Empty),
  Nothing,
}

fn f() -> Outer {
  Outer::Nothing
}
  )";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
}

TEST(
    FunctionConverterTest,
    ConvertsSemanticSumConstructorWithInactiveAnnotatedEmptySumPayloadInPhase1) {
  constexpr std::string_view kProgram = R"(
enum Empty: u2 {
}

enum MaybeImpossible {
  Unit,
  Impossible(Empty),
}

fn f() -> MaybeImpossible {
  MaybeImpossible::Unit
}
  )";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));

  Function* f = tm.module->GetFunction("f").value();
  ASSERT_NE(f, nullptr);
  const ConvertOptions convert_options;
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
}

}  // namespace
}  // namespace xls::dslx
