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
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/ir_convert/conversion_info.h"
#include "xls/dslx/ir_convert/convert_options.h"
#include "xls/dslx/ir_convert/test_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/clone_package.h"
#include "xls/ir/nodes.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/ir/xls_ir_interface.pb.h"
#include "xls/jit/function_jit.h"
#include "xls/passes/optimization_pass_pipeline.h"
#include "xls/simulation/default_verilog_simulator.h"
#include "xls/simulation/module_simulator.h"

namespace xls::dslx {
namespace {
using ::absl_testing::IsOkAndHolds;
using ::xls::proto_testing::EqualsProto;

void ExpectIr(std::string_view got) {
  return ::xls::dslx::ExpectIr(got, TestName(), "function_converter_test");
}

PackageConversionData MakeConversionData(std::string_view n) {
  return {.package = std::make_unique<Package>(n)};
}

enum class SharedSumShape { kLadder, kDiamond, kShiftedAggregates };

// Each level adds only one possible rendered child, not two simultaneous
// children. The diamond reaches it through distinct nominal wrappers; the
// aggregates also change its packed offset and position in the rendered text.
std::string SharedSumDeclarations(int64_t depth, SharedSumShape shape) {
  std::string program = "enum S0 { Leaf(u1) }\n";
  for (int64_t i = 1; i <= depth; ++i) {
    const std::string suffix = std::to_string(i);
    const std::string child = "S" + std::to_string(i - 1);
    const std::string left = "Left" + suffix;
    const std::string right = "Right" + suffix;
    if (shape == SharedSumShape::kLadder) {
      program +=
          "enum S" + suffix + " { A(" + child + "), B(" + child + ") }\n";
    } else if (shape == SharedSumShape::kDiamond) {
      program += "enum " + left + " { Wrap(" + child + ") }\n";
      program += "enum " + right + " { Wrap(" + child + ") }\n";
      program += "enum S" + suffix + " { A(" + left + "), B(" + right + ") }\n";
    } else {
      program +=
          "struct " + left + " { before: u1, child: " + child + "[1] }\n";
      program +=
          "struct " + right + " { child: (" + child + ",), after: u2 }\n";
      program += "enum S" + suffix + " { A(" + left + "), B(" + right + ") }\n";
    }
  }
  return program;
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::HasSubstr("literal(value=3"));
  EXPECT_THAT(package.DumpIr(), testing::HasSubstr("literal(value=7"));
}

TEST(FunctionConverterTest,
     ExecutesSignedSumDiscriminantsAtGeneratedRtlBoundary) {
  constexpr std::string_view kProgram = R"(
enum Message: s3 {
  Empty = 0,
  Negative(u8) = -1,
}

fn f(x: Message) -> u8 {
  match x {
    Message::Negative(value) => value,
    Message::Empty => u8:0,
    invalid! => u8:255,
  }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* function = tm.module->GetFunction("f").value();

  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              ConvertOptions(), /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(function, tm.type_info,
                                         ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value negative = Value::Tuple(
      {Value(SBits(/*value=*/-1, /*bit_count=*/3)),
       Value::Tuple({Value(UBits(/*value=*/42, /*bit_count=*/8))})});
  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/3, /*bit_count=*/3)),
       Value::Tuple({Value(UBits(/*value=*/17, /*bit_count=*/8))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted_negative,
                           InterpretFunction(ir_function, {negative}));
  EXPECT_EQ(interpreted_negative.value,
            Value(UBits(/*value=*/42, /*bit_count=*/8)));
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted_malformed,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(interpreted_malformed.value,
            Value(UBits(/*value=*/255, /*bit_count=*/8)));

  XLS_ASSERT_OK(package.package->SetTop(ir_function));
  XLS_ASSERT_OK(RunOptimizationPassPipeline(package.package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> simulation_package,
                           ClonePackage(package.package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * simulation_function,
                           simulation_package->GetFunction("__test_module__f"));
  XLS_ASSERT_OK_AND_ASSIGN(
      verilog::CodegenResult system_verilog,
      verilog::GenerateCombinationalModule(
          ir_function, verilog::CodegenOptions().use_system_verilog(true)));
  EXPECT_THAT(system_verilog.verilog_text, testing::HasSubstr("module "));

  // The default open-source simulator does not accept SystemVerilog. Generate
  // the equivalent Verilog module to execute the same lowered hardware.
  XLS_ASSERT_OK_AND_ASSIGN(
      verilog::CodegenResult generated,
      verilog::GenerateCombinationalModule(
          simulation_function,
          verilog::CodegenOptions().use_system_verilog(false)));
  std::unique_ptr<verilog::VerilogSimulator> verilog_simulator =
      verilog::GetDefaultVerilogSimulator();
  verilog::ModuleSimulator simulator(
      generated.signature, generated.verilog_text, verilog::FileType::kVerilog,
      verilog_simulator.get());
  EXPECT_THAT(simulator.RunFunction(
                  absl::flat_hash_map<std::string, Value>{{"x", negative}}),
              IsOkAndHolds(Value(UBits(/*value=*/42, /*bit_count=*/8))));
  EXPECT_THAT(simulator.RunFunction(
                  absl::flat_hash_map<std::string, Value>{{"x", malformed}}),
              IsOkAndHolds(Value(UBits(/*value=*/255, /*bit_count=*/8))));
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
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

// Verifies: Executed RTL preserves transport and constructor padding.
// Catches: Wire rewrites and nested wildcards that inspect hidden tags.
TEST(FunctionConverterTest, SumTransportAndShallowObservationAtRtlBoundary) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 { Small(u4) = 0, Big(u8) = 1 }
enum Outer: u1 { Wrapped(Message) = 0, Wide(u16) = 1 }
fn forward(x: Message) -> Message { x }
fn construct(x: Message) -> Message {
  match x { Message::Small(v) => Message::Small(v), Message::Big(v) => Message::Big(v) }
}
fn wrap(x: Message) -> Outer { Outer::Wrapped(x) }
fn unwrap(x: Outer) -> Message {
  match x { Outer::Wrapped(v) => v, Outer::Wide(_) => Message::Small(u4:0) }
}
fn ignore(x: Outer) -> u8 {
  match x { Outer::Wrapped(_) => u8:42, Outer::Wide(_) => u8:0 }
}
fn named_arm(x: Message) -> u8 {
  match x { _bound => u8:1, invalid! => u8:2 }
}
fn wildcard_arm(x: Message) -> u8 {
  match x { _ => u8:1, invalid! => u8:2 }
}
fn bind_whole(x: Message) -> Message {
  match x { value => value, invalid! => Message::Small(u4:0) }
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  const Value canonical =
      Value::Tuple({Value(UBits(0, 2)), Value::Tuple({Value(UBits(0x0a, 8))})});
  const Value dirty =
      Value::Tuple({Value(UBits(0, 2)), Value::Tuple({Value(UBits(0xfa, 8))})});
  const Value malformed =
      Value::Tuple({Value(UBits(3, 2)), Value::Tuple({Value(UBits(0xfa, 8))})});
  const Value wrapped_dirty = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0xfa, 16))})});
  const Value wrapped_malformed = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0x3fa, 16))})});
  const Value undeclared =
      Value::Tuple({Value(UBits(2, 2)), Value::Tuple({Value(UBits(0xfa, 8))})});
  struct Case {
    std::string_view function;
    Value input;
    Value expected;
  };
  const std::vector<Case> cases = {
      {"forward", canonical, canonical},
      {"forward", dirty, dirty},
      {"forward", malformed, malformed},
      {"construct", dirty, canonical},
      {"wrap", dirty, wrapped_dirty},
      {"wrap", malformed, wrapped_malformed},
      {"unwrap", wrapped_dirty, dirty},
      {"unwrap", wrapped_malformed, malformed},
      {"ignore", wrapped_malformed, Value(UBits(42, 8))},
      {"named_arm", undeclared, Value(UBits(2, 8))},
      {"named_arm", dirty, Value(UBits(1, 8))},
      {"wildcard_arm", undeclared, Value(UBits(2, 8))},
      {"wildcard_arm", dirty, Value(UBits(1, 8))},
      {"bind_whole", dirty, dirty},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.function);
    SCOPED_TRACE(test_case.input.ToString());
    Function* function = tm.module->GetFunction(test_case.function).value();
    PackageConversionData package = MakeConversionData("test_module_package");
    PackageData package_data{.conversion_info = &package};
    FunctionConverter converter(package_data, tm.module, &import_data,
                                ConvertOptions(), /*proc_data=*/nullptr,
                                /*channel_scope=*/nullptr, /*is_top=*/true);
    XLS_ASSERT_OK(converter.HandleFunction(function, tm.type_info,
                                           ParametricEnv{}));
    XLS_ASSERT_OK_AND_ASSIGN(
        xls::Function * ir_function,
        package.package->GetFunction("__test_module__" +
                                     std::string(test_case.function)));
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted,
                             InterpretFunction(ir_function, {test_case.input}));
    EXPECT_EQ(interpreted.value, test_case.expected);
    XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(ir_function));
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jitted,
                             jit->Run({test_case.input}));
    EXPECT_EQ(jitted.value, test_case.expected);
    XLS_ASSERT_OK(package.package->SetTop(ir_function));
    XLS_ASSERT_OK(RunOptimizationPassPipeline(package.package.get()));
    XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> simulation_package,
                             ClonePackage(package.package.get()));
    XLS_ASSERT_OK_AND_ASSIGN(
        xls::Function * simulation_function,
        simulation_package->GetFunction(ir_function->name()));
    XLS_ASSERT_OK_AND_ASSIGN(
        verilog::CodegenResult system_verilog,
        verilog::GenerateCombinationalModule(
            ir_function, verilog::CodegenOptions().use_system_verilog(true)));
    // Sum ports carry the entire tag and slot, including inactive padding.
    EXPECT_THAT(system_verilog.verilog_text,
                testing::HasSubstr(test_case.input.GetFlatBitCount() == 10
                                       ? "[9:0] x"
                                       : "[16:0] x"));
    XLS_ASSERT_OK_AND_ASSIGN(
        verilog::CodegenResult generated,
        verilog::GenerateCombinationalModule(
            simulation_function,
            verilog::CodegenOptions().use_system_verilog(false)));
    std::unique_ptr<verilog::VerilogSimulator> verilog_simulator =
        verilog::GetDefaultVerilogSimulator();
    verilog::ModuleSimulator simulator(
        generated.signature, generated.verilog_text,
        verilog::FileType::kVerilog, verilog_simulator.get());
    EXPECT_THAT(simulator.RunFunction(absl::flat_hash_map<std::string, Value>{
                    {"x", test_case.input}}),
                IsOkAndHolds(test_case.expected));
  }
}

// Both the outer padding and the active inner sum's padding are ignored by
// equality, without normalizing either operand's raw image.
TEST(FunctionConverterTest,
     SemanticSumEqualityIgnoresNestedPaddingAtRtlBoundary) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 { Small(u4) = 0, Big(u8) = 1 }
enum Outer: u1 { Wrapped(Message) = 0, Wide(u16) = 1 }
fn f(x: Outer, y: Outer) -> (bool, bool, Outer, Outer) {
  (x == y, x != y, x, y)
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* function = tm.module->GetFunction("f").value_or(nullptr);
  ASSERT_NE(function, nullptr);
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              ConvertOptions(), /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(function, tm.type_info,
                                         ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  // The slot contains six outer padding bits, the two-bit Message tag, four
  // inner padding bits, and the four meaningful Small payload bits.
  const Value dirty = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0xfcfa, 16))})});
  const Value canonical = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0x000a, 16))})});
  const Value different = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0x000b, 16))})});
  ASSERT_NE(dirty, canonical);

  struct Case {
    Value rhs;
    bool equal;
  };
  const std::vector<Case> cases = {{canonical, true}, {different, false}};
  std::vector<absl::flat_hash_map<std::string, Value>> rtl_inputs;
  std::vector<Value> expected_outputs;
  XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(ir_function));
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.rhs.ToString());
    const Value expected =
        Value::Tuple({Value(UBits(test_case.equal, 1)),
                      Value(UBits(!test_case.equal, 1)), dirty, test_case.rhs});
    const std::vector<Value> arguments = {dirty, test_case.rhs};
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted,
                             InterpretFunction(ir_function, arguments));
    EXPECT_EQ(interpreted.value, expected);
    EXPECT_THAT(interpreted.events.GetAssertMessages(), testing::IsEmpty());
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jitted,
                             jit->Run(arguments));
    EXPECT_EQ(jitted.value, expected);
    EXPECT_THAT(jitted.events.GetAssertMessages(), testing::IsEmpty());
    rtl_inputs.push_back({{"x", dirty}, {"y", test_case.rhs}});
    expected_outputs.push_back(expected);
  }

  XLS_ASSERT_OK(package.package->SetTop(ir_function));
  XLS_ASSERT_OK(RunOptimizationPassPipeline(package.package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      verilog::CodegenResult generated,
      verilog::GenerateCombinationalModule(
          ir_function, verilog::CodegenOptions().use_system_verilog(false)));
  std::unique_ptr<verilog::VerilogSimulator> verilog_simulator =
      verilog::GetDefaultVerilogSimulator();
  verilog::ModuleSimulator simulator(
      generated.signature, generated.verilog_text, verilog::FileType::kVerilog,
      verilog_simulator.get());
  EXPECT_THAT(simulator.RunBatched(rtl_inputs), IsOkAndHolds(expected_outputs));
}

// Padding may be unknown even when every tag and active field is known.
// Case equality catches both lost X bits in transport and Xs leaking into
// semantic matching or equality; the two-state Value API cannot express either.
TEST(FunctionConverterTest, SemanticSumUnknownPaddingAtRtlBoundary) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 { Small(u4) = 0, Big(u8) = 1 }
enum Outer: u1 { Wrapped(Message) = 0, Wide(u16) = 1 }
fn f(x: Message, y: Outer) ->
    (Message, u8, bool, bool, Outer, u8, bool, bool) {
  let matched = match x {
    Message::Small(v) => v as u8,
    Message::Big(v) => v,
  };
  let nested_matched = match y {
    Outer::Wrapped(inner) => match inner {
      Message::Small(v) => v as u8,
      Message::Big(v) => v,
    },
    Outer::Wide(_) => u8:0,
  };
  let small_ten = Message::Small(u4:10);
  let wrapped_ten = Outer::Wrapped(small_ten);
  (x, matched, x == small_ten, x != small_ten,
   y, nested_matched, y == wrapped_ten, y != wrapped_ten)
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  Function* function = tm.module->GetFunction("f").value_or(nullptr);
  ASSERT_NE(function, nullptr);
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              ConvertOptions(), /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(function, tm.type_info,
                                         ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  XLS_ASSERT_OK(package.package->SetTop(ir_function));
  XLS_ASSERT_OK(RunOptimizationPassPipeline(package.package.get()));

  // Run the generated dialect the simulator supports. The Icarus wrapper only
  // accepts Verilog, which still supports literal X bits and case equality.
  auto simulator = verilog::GetDefaultVerilogSimulator();
  const bool use_system_verilog = simulator->DoesSupportSystemVerilog();
  const verilog::FileType file_type = use_system_verilog
                                          ? verilog::FileType::kSystemVerilog
                                          : verilog::FileType::kVerilog;
  XLS_ASSERT_OK_AND_ASSIGN(
      verilog::CodegenResult generated,
      verilog::GenerateCombinationalModule(
          ir_function, verilog::CodegenOptions()
                           .use_system_verilog(use_system_verilog)
                           .module_name("sum_unknown_padding")));
  constexpr std::string_view kTestbench = R"(
module testbench;
  reg [9:0] x;
  reg [16:0] y;
  // Tuple fields are flattened MSB-first in the DSLX return order.
  wire [46:0] result;
  sum_unknown_padding dut(.x(x), .y(y), .out(result));
  initial begin
    // y has six outer padding bits and four inactive bits in its inner Message.
    // Distinct active payloads separate the top-level and nested observations.
    x = 10'b00_xxxx_1010;
    y = 17'b0_xxxxxx_00_xxxx_1011;
    #1;
    $display("ten_ok=%b result=%b", result === {
      10'b00_xxxx_1010, 8'd10, 1'b1, 1'b0,
      17'b0_xxxxxx_00_xxxx_1011, 8'd11, 1'b0, 1'b1
    }, result);
    x = 10'b00_xxxx_1011;
    y = 17'b0_xxxxxx_00_xxxx_1010;
    #1;
    $display("eleven_ok=%b result=%b", result === {
      10'b00_xxxx_1011, 8'd11, 1'b0, 1'b1,
      17'b0_xxxxxx_00_xxxx_1010, 8'd10, 1'b1, 1'b0
    }, result);
    $finish;
  end
endmodule
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      auto stdout_stderr,
      simulator->Run(generated.verilog_text + std::string(kTestbench),
                     file_type));
  EXPECT_THAT(stdout_stderr.first, testing::HasSubstr("ten_ok=1 result="));
  EXPECT_THAT(stdout_stderr.first, testing::HasSubstr("eleven_ok=1 result="));
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value malformed = Value::Tuple(
      {Value(UBits(/*value=*/1, /*bit_count=*/1)),
       Value::Tuple({Value(UBits(/*value=*/0b011, /*bit_count=*/3))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                           InterpretFunction(ir_function, {malformed}));
  EXPECT_EQ(result.value, Value(UBits(/*value=*/0b1011, /*bit_count=*/4)));
}

// Verifies: Aggregate forwarding preserves each nested sum's complete image.
// Catches: Recursive boundary normalization in tuples, structs, or arrays.
TEST(FunctionConverterTest,
     PreservesSemanticSumsNestedInStructsTuplesAndArrays) {
  constexpr std::string_view kProgram = R"(
enum Message: u2 {
  Small(u4) = 0,
  Big(u8) = 1,
}

struct Wrapper {
  first: Message,
  rest: (Message[1],),
}

fn f(x: Wrapper) -> Wrapper {
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));

  const Value noncanonical = Value::Tuple(
      {Value(UBits(/*value=*/0, /*bit_count=*/2)),
       Value::Tuple({Value(UBits(/*value=*/0xfa, /*bit_count=*/8))})});
  const Value aggregate = Value::Tuple(
      {noncanonical, Value::Tuple({Value::ArrayOrDie({noncanonical})})});

  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                           InterpretFunction(ir_function, {aggregate}));
  EXPECT_EQ(result.value, aggregate);
}

TEST(FunctionConverterTest, SharedSumEqualityGrowthTracksPackedSubvalues) {
  for (SharedSumShape shape :
       {SharedSumShape::kLadder, SharedSumShape::kDiamond,
        SharedSumShape::kShiftedAggregates}) {
    SCOPED_TRACE(static_cast<int>(shape));
    std::vector<int64_t> node_counts;
    for (int64_t depth : {4, 8}) {
      SCOPED_TRACE(depth);
      const std::string root = "S" + std::to_string(depth);
      const std::string program = SharedSumDeclarations(depth, shape) +
                                  "fn f(x: " + root + ", y: " + root +
                                  ") -> bool { x == y }";
      ImportData import_data = CreateImportDataForTest();
      XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                               ParseAndTypecheck(program, "test_module.x",
                                                 "test_module", &import_data));
      PackageConversionData package = MakeConversionData("test_module_package");
      PackageData package_data{.conversion_info = &package};
      FunctionConverter converter(package_data, tm.module, &import_data,
                                  ConvertOptions{}, /*proc_data=*/nullptr,
                                  /*channel_scope=*/nullptr, /*is_top=*/true);
      XLS_ASSERT_OK(converter.HandleFunction(
          tm.module->GetFunction("f").value(), tm.type_info, ParametricEnv{}));
      XLS_ASSERT_OK_AND_ASSIGN(
          xls::Function * function,
          package.package->GetFunction("__test_module__f"));
      node_counts.push_back(function->node_count());
      for (xls::Node* node : function->nodes()) {
        EXPECT_FALSE(node->Is<xls::Assert>());
      }
    }
    EXPECT_LT(node_counts.at(1), 6 * node_counts.at(0));
  }
}

TEST(FunctionConverterTest, SharedSumEqualityDirectCallersPreserveFallback) {
  constexpr std::string_view kProgram = R"(
enum R: u2 { Wide(u8) = 0, Narrow(u4) = 1 }
enum Left { Wrap((R, R)) }
enum Right { Wrap(R[2]) }
enum S: u2 { A(Left) = 0, B(Right) = 1 }
const K = S::A(Left::Wrap((R::Narrow(u4:10), R::Wide(u8:37))));
fn f(x: S, y: S) -> (bool, bool, bool) {
  assert_eq(x, y);
  (x == y, x != y, match x { K => true, _ => false })
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              ConvertOptions{}, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  Function* source_function = tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      auto constant_deps,
      GetConstantDepFreevars(source_function->body(), *tm.type_info));
  for (ConstantDef* dependency : constant_deps) {
    converter.AddConstantDep(dependency);
  }
  XLS_ASSERT_OK(
      converter.HandleFunction(source_function, tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Function * function,
      package.package->GetFunction("__itok__test_module__f"));
  int64_t assert_count = 0;
  for (xls::Node* node : function->nodes()) {
    assert_count += node->Is<xls::Assert>();
  }
  EXPECT_EQ(assert_count,
            1);  // Only the source assert_eq, never tag validation.
  XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(function));
  auto sum = [](uint64_t tag, uint64_t payload) {
    return Value::Tuple(
        {Value(UBits(tag, 2)), Value::Tuple({Value(UBits(payload, 20))})});
  };
  struct Case {
    Value lhs;
    Value rhs;
    bool equal;
    bool matches_constant;
  };
  const std::vector<Case> cases = {
      {sum(0, (0x1fa << 10) | 37), sum(0, (0x10a << 10) | 37), true, true},
      // Equal first members cannot substitute for unequal second members.
      {sum(0, (0x1fa << 10) | 37), sum(0, (0x10a << 10) | 38), false, true},
      {sum(1, (37 << 10) | 0x1fa), sum(1, (37 << 10) | 0x10a), true, false},
      // Both outer and inner malformed tags use their final constructor, but
      // still compare their original tag bits and ignore only inactive padding.
      {sum(3, (37 << 10) | 0x3fa), sum(3, (37 << 10) | 0x30a), true, false},
      {sum(3, (37 << 10) | 0x3fa), sum(2, (37 << 10) | 0x30a), false, false},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.lhs.ToString());
    SCOPED_TRACE(test_case.rhs.ToString());
    const std::vector<Value> args = {Value::Token(), Value::Bool(true),
                                     test_case.lhs, test_case.rhs};
    const Value expected =
        Value::Tuple({Value::Token(),
                      Value::Tuple({Value::Bool(test_case.equal),
                                    Value::Bool(!test_case.equal),
                                    Value::Bool(test_case.matches_constant)})});
    XLS_ASSERT_OK_AND_ASSIGN(auto interpreted,
                             InterpretFunction(function, args));
    EXPECT_EQ(interpreted.value, expected);
    EXPECT_EQ(interpreted.events.GetAssertMessages().size(), !test_case.equal);
    XLS_ASSERT_OK_AND_ASSIGN(auto jitted, jit->Run(args));
    EXPECT_EQ(jitted.value, expected);
    EXPECT_EQ(jitted.events.GetAssertMessages().size(), !test_case.equal);
  }
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));
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
  XLS_ASSERT_OK(
      converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

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

TEST(FunctionConverterTest, SingleVariantSemanticSumEqSkipsTagSelect) {
  constexpr std::string_view kProgram = R"(
enum Box {
  Wrap(u32),
}

fn f(x: Box, y: Box) -> bool {
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
  int64_t equality_select_count = 0;
  for (xls::Node* node : ir_function->nodes()) {
    if (node->op() == xls::Op::kSel) {
      const auto* select = node->As<xls::Select>();
      if (SelectCaseContainsOp(*select, xls::Op::kEq)) {
        ++equality_select_count;
      }
    }
  }
  EXPECT_EQ(equality_select_count, 0);
}

TEST(FunctionConverterTest,
     SingleVariantSemanticSumMatchNeedsNoTagSelectOrToken) {
  constexpr std::string_view kProgram = R"(
enum Box {
  Wrap(u32),
}

fn f(x: Box) -> u32 {
  match x {
    Box::Wrap(v) => v,
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

  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  int64_t tag_select_count = 0;
  for (xls::Node* node : ir_function->nodes()) {
    if (node->op() == xls::Op::kSel && node->operand(0)->op() == xls::Op::kEq) {
      ++tag_select_count;
    }
  }
  EXPECT_EQ(tag_select_count, 0);
  EXPECT_THAT(package.DumpIr(), testing::Not(testing::HasSubstr("assert(")));
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

// Verifies: ignored arrays stay packed across tuple/rest and named patterns.
// Catches: array-sized IR growth, wrong offsets, and dropped member predicates.
TEST(FunctionConverterTest, IgnoredSumPayloadDoesNotExpandArrays) {
  struct TestCase {
    std::string variant;
    std::string pattern;
    std::string result;
    int64_t bytes_before;
    int64_t bytes_after;
    int64_t expected;
  };
  const TestCase kCases[] = {
      {"Data(Payload)", "Data(_)", "u8:9", 0, 0, 9},
      {"Data((Payload, u8))", "Data((_, n))", "n", 0, 1, 9},
      {"Data((Payload, u8))", "Data((_, u8:9))", "u8:1", 0, 1, 1},
      {"Data((Payload, u8))", "Data((_, u8:7))", "u8:1", 0, 1, 0},
      {"Data((u8, Payload, u8))", "Data((first, .., last))", "first + last", 1,
       1, 16},
      {"Data((u8, Payload, u8))", "Data((.., last))", "last", 1, 1, 9},
      {"Data((u8, Payload, u8))", "Data((first, ..))", "first", 1, 1, 7},
      {"Data { bytes: Payload, number: u8 }", "Data { bytes: _, number: n }",
       "n", 0, 1, 9},
  };
  for (const TestCase& test_case : kCases) {
    SCOPED_TRACE(test_case.pattern);
    std::vector<int64_t> node_counts;
    for (int64_t size : {4, 1024}) {
      SCOPED_TRACE(size);
      const std::string program =
          "type Payload = u8[" + std::to_string(size) +
          "];\n"
          "enum Message { " +
          test_case.variant +
          ", Empty }\n"
          "fn f(x: Message) -> u8 { match x { Message::" +
          test_case.pattern + " => " + test_case.result + ", _ => u8:0 } }";
      ImportData import_data = CreateImportDataForTest();
      XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                               ParseAndTypecheck(program, "test_module.x",
                                                 "test_module", &import_data));
      PackageConversionData package = MakeConversionData("test_module_package");
      PackageData package_data{.conversion_info = &package};
      FunctionConverter converter(package_data, tm.module, &import_data,
                                  ConvertOptions{}, /*proc_data=*/nullptr,
                                  /*channel_scope=*/nullptr, /*is_top=*/true);
      XLS_ASSERT_OK(converter.HandleFunction(
          tm.module->GetFunction("f").value(), tm.type_info, ParametricEnv{}));
      XLS_ASSERT_OK_AND_ASSIGN(
          xls::Function * function,
          package.package->GetFunction("__test_module__f"));
      node_counts.push_back(function->node_count());
      for (xls::Node* node : function->nodes()) {
        EXPECT_NE(node->op(), xls::Op::kArray);
      }

      // Tuple and struct members are packed MSB-first; BitsRope appends LSbs
      // first. The large array is intentionally surrounded by different bytes.
      BitsRope payload(8 *
                       (test_case.bytes_before + size + test_case.bytes_after));
      if (test_case.bytes_after != 0) {
        payload.push_back(UBits(9, 8));
      }
      payload.push_back(Bits::AllOnes(8 * size));
      if (test_case.bytes_before != 0) {
        payload.push_back(UBits(7, 8));
      }
      const Value input = Value::Tuple(
          {Value(UBits(0, 1)), Value::Tuple({Value(payload.Build())})});
      XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted,
                               InterpretFunction(function, {input}));
      EXPECT_EQ(interpreted.value, Value(UBits(test_case.expected, 8)));
      XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(function));
      XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jitted,
                               jit->Run({input}));
      EXPECT_EQ(jitted.value, interpreted.value);
    }
    EXPECT_EQ(node_counts.at(0), node_counts.at(1));
  }
}

// Verifies: an array bound by a sum pattern is available to the arm.
// Catches: dropping demanded data while skipping ignored payloads.
TEST(FunctionConverterTest, BoundArraySumPayloadRemainsAvailable) {
  constexpr std::string_view kProgram = R"(
enum Message { Data(u8[2]), Empty }
fn f(x: Message) -> u8 {
  match x { Message::Data(a) => a[u32:1], _ => u8:0 }
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kProgram, "test_module.x",
                                             "test_module", &import_data));
  PackageConversionData package = MakeConversionData("test_module_package");
  PackageData package_data{.conversion_info = &package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              ConvertOptions{}, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(tm.module->GetFunction("f").value(),
                                         tm.type_info, ParametricEnv{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           package.package->GetFunction("__test_module__f"));
  const Value input = Value::Tuple(
      {Value(UBits(0, 1)), Value::Tuple({Value(UBits(0x0703, 16))})});
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> interpreted,
                           InterpretFunction(function, {input}));
  EXPECT_EQ(interpreted.value, Value(UBits(7, 8)));
  XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(function));
  XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jitted, jit->Run({input}));
  EXPECT_EQ(jitted.value, interpreted.value);
}

TEST(FunctionConverterTest,
     AggregateContainedSemanticSumMatchUsesPhase2FallbackWithoutToken) {
  constexpr std::string_view kProgram = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: (Option,)) -> u32 {
  match x {
    (Option::Some(v),) => v,
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

TEST(FunctionConverterTest,
     AllowsActiveAnnotatedEmptySumPayloadVariantInPhase2WithoutToken) {
  constexpr std::string_view kProgram = R"(
enum Empty: u2 {
}

enum MaybeImpossible {
  Unit,
  Impossible(Empty),
}

fn f(x: MaybeImpossible) -> u32 {
  match x {
    MaybeImpossible::Unit => u32:0,
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
  PackageData package_data{&package};
  FunctionConverter converter(package_data, tm.module, &import_data,
                              convert_options, /*proc_data=*/nullptr,
                              /*channel_scope=*/nullptr,
                              /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(f, tm.type_info, ParametricEnv{}));

  EXPECT_THAT(package.DumpIr(), testing::Not(testing::HasSubstr("assert(")));
}

TEST(FunctionConverterTest,
     Phase2FallbackDoesNotObserveInactiveEnumPayloadMembers) {
  constexpr std::string_view kProgram = R"(
enum Flavor: u2 {
  Vanilla = u2:1,
  Mint = u2:2,
}

enum Choice {
  Unit,
  Some(Flavor),
}

fn f(x: Choice) -> u32 {
  match x {
    Choice::Unit => u32:0,
    _ => u32:1,
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

  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__test_module__f"));
  auto matches_payload_member = [](xls::Node* node, uint64_t value) {
    if (node->op() != xls::Op::kEq) {
      return false;
    }
    auto has_literal_value = [&](xls::Node* operand) {
      return operand->op() == xls::Op::kLiteral &&
             operand->As<xls::Literal>()->value() ==
                 xls::Value(xls::UBits(value, 2));
    };
    return has_literal_value(node->operand(0)) ||
           has_literal_value(node->operand(1));
  };
  int64_t enum_member_eq_count = 0;
  for (xls::Node* node : ir_function->nodes()) {
    if (matches_payload_member(node, /*value=*/1) ||
        matches_payload_member(node, /*value=*/2)) {
      ++enum_member_eq_count;
    }
  }
  EXPECT_EQ(enum_member_eq_count, 0) << package.DumpIr();
}

}  // namespace
}  // namespace xls::dslx
