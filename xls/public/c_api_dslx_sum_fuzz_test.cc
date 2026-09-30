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

// Bounded valid-source coverage for C API invocation bindings. Each input
// chooses one of six aggregate shapes, three constructors, two bytes, and the
// last surviving clone. Both invocation-data accessors must preserve literal
// constructor formatting and stable borrowed views. Only formatting through
// surviving clones is observed after source teardown; AST/TypeInfo access
// through a cloned invocation entry still requires the source owner.

#include <cstdint>
#include <initializer_list>
#include <string>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/public/c_api.h"
#include "xls/public/c_api_dslx.h"

namespace xls {
namespace {

enum class ValueShape { kDirect, kTuple, kArray, kNested, kRecord, kMixed };
enum class Constructor { kNone, kSome, kOther };
enum class LastClone { kInvocation, kEnvironment, kValue };

struct ValueCase {
  std::string type;
  std::string expression;
  std::string expected;
};

ValueCase MakeValueCase(ValueShape shape, Constructor constructor,
                        uint32_t payload, uint32_t companion) {
  std::string expression;
  std::string expected;
  switch (constructor) {
    case Constructor::kNone:
      expression = "Choice::None";
      expected = "Choice::None";
      break;
    case Constructor::kSome:
      expression = absl::StrCat("Choice::Some( u8:", payload, " )");
      expected = absl::StrCat("Choice::Some(u8:", payload, ")");
      break;
    case Constructor::kOther:
      expression = absl::StrCat("Choice::Other( u8:", payload, " )");
      expected = absl::StrCat("Choice::Other(u8:", payload, ")");
      break;
  }
  // These are host-computed literal expectations, not output from a DSLX
  // formatter, AST printer, or round trip through the implementation.
  ValueCase result;
  switch (shape) {
    case ValueShape::kDirect:
      result = {"Choice", expression, expected};
      break;
    case ValueShape::kTuple:
      result = {
          "(Choice, u8)",
          absl::StrCat("(", expression, ", u8:", companion, ")"),
          absl::StrCat("(\n    ", expected, ",\n    u8:", companion, "\n)")};
      break;
    case ValueShape::kArray:
      result = {
          "Choice[2]",
          absl::StrCat("[", expression, ", Choice::Other(u8:", companion, ")]"),
          absl::StrCat("[\n    ", expected,
                       ",\n    Choice::Other(u8:", companion, ")\n]")};
      break;
    case ValueShape::kNested:
      result = {"Outer", absl::StrCat("Outer::Wrap( ", expression, " )"),
                absl::StrCat("Outer::Wrap(", expected, ")")};
      break;
    case ValueShape::kRecord:
      result = {"Outer",
                absl::StrCat("Outer::Record { value: ", expression, " }"),
                absl::StrCat("Outer::Record {\n    value: ", expected, "\n}")};
      break;
    case ValueShape::kMixed:
      result = {"(Choice[2], u8)",
                absl::StrCat("([", expression, ", Choice::Other(u8:", companion,
                             ")], u8:", companion, ")"),
                absl::StrCat("(\n    [\n        ", expected,
                             ",\n        Choice::Other(u8:", companion,
                             ")\n    ],\n    u8:", companion, "\n)")};
      break;
  }
  return result;
}

void ExpectOwnedText(char* actual, const std::string& expected) {
  absl::Cleanup cleanup([&] { xls_c_str_free(actual); });
  ASSERT_NE(actual, nullptr);
  EXPECT_STREQ(actual, expected.c_str());
}

void ExpectBindings(const xls_dslx_parametric_env* env,
                    const std::string& expected_value, uint32_t control) {
  ASSERT_NE(env, nullptr);
  ASSERT_EQ(xls_dslx_parametric_env_get_binding_count(env), 2);
  EXPECT_STREQ(xls_dslx_parametric_env_get_binding_identifier(env, 0), "A");
  EXPECT_STREQ(xls_dslx_parametric_env_get_binding_identifier(env, 1), "V");
  auto* ordinary = xls_dslx_parametric_env_get_binding_value(env, 0);
  auto* value = xls_dslx_parametric_env_get_binding_value(env, 1);
  ASSERT_NE(ordinary, nullptr);
  ASSERT_NE(value, nullptr);
  const std::string expected_control = absl::StrCat("u8:", control);
  const std::string expected_env =
      absl::StrCat("{A: ", expected_control, ", V: ", expected_value, "}");
  // Interleave whole-environment and individual formatting. Retrieval must
  // keep the same borrowed handles without assigning the sum format to A.
  for (int repeat = 0; repeat < 2; ++repeat) {
    EXPECT_EQ(value, xls_dslx_parametric_env_get_binding_value(env, 1));
    EXPECT_EQ(ordinary, xls_dslx_parametric_env_get_binding_value(env, 0));
    ExpectOwnedText(xls_dslx_parametric_env_to_string(env), expected_env);
    ExpectOwnedText(xls_dslx_interp_value_to_string(value), expected_value);
    ExpectOwnedText(xls_dslx_interp_value_to_string(ordinary),
                    expected_control);
  }
}

void InvocationBindingsKeepSumFormatting(ValueShape shape,
                                         Constructor constructor,
                                         uint32_t payload, uint32_t companion,
                                         LastClone last_clone) {
  const ValueCase value_case =
      MakeValueCase(shape, constructor, payload, companion);
  // A differs from the ordinary aggregate leaf, and declaration order is
  // reversed from the environment's alphabetical binding order.
  const uint32_t control = 255 - companion;
  const std::string program =
      absl::StrCat("enum Choice { None, Some(u8), Other(u8) }\n",
                   "enum Outer { Wrap(Choice), Record { value: Choice } }\n",
                   "type Value = ", value_case.type, ";\n",
                   "fn f<V: Value, A: u8>() -> Value { V }\n",
                   "pub fn main() -> Value { f<{", value_case.expression,
                   "}, u8:", control, ">() }\n");
  SCOPED_TRACE(program);
  SCOPED_TRACE(static_cast<int>(last_clone));
  for (auto get_data : {xls_dslx_type_info_get_unique_invocation_callee_data,
                        xls_dslx_type_info_get_all_invocation_callee_data}) {
    SCOPED_TRACE(get_data ==
                         xls_dslx_type_info_get_unique_invocation_callee_data
                     ? "unique"
                     : "all");
    auto* owner = xls_dslx_import_data_create(
        std::string(kDefaultDslxStdlibPath).c_str(), nullptr, 0);
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
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(xls_dslx_parse_and_typecheck(program.c_str(), "sum_fuzz.x",
                                             "sum_fuzz", owner, &error, &tm))
        << (error == nullptr ? "no error text" : error);
    auto* module = xls_dslx_typechecked_module_get_module(tm);
    auto* type_info = xls_dslx_typechecked_module_get_type_info(tm);
    // The three declarations above precede f.
    auto* function = xls_dslx_module_member_get_function(
        xls_dslx_module_get_member(module, 3));
    ASSERT_NE(function, nullptr);
    array = get_data(type_info, function);
    ASSERT_NE(array, nullptr);
    ASSERT_EQ(xls_dslx_invocation_callee_data_array_get_count(array), 1);
    auto* data = xls_dslx_invocation_callee_data_array_get(array, 0);
    ASSERT_NE(data, nullptr);
    const auto* env = xls_dslx_invocation_callee_data_get_callee_bindings(data);
    ASSERT_NO_FATAL_FAILURE(ExpectBindings(env, value_case.expected, control));
    EXPECT_EQ(env, xls_dslx_invocation_callee_data_get_callee_bindings(data));
    auto* value = xls_dslx_parametric_env_get_binding_value(env, 1);
    const auto* caller_env =
        xls_dslx_invocation_callee_data_get_caller_bindings(data);
    ASSERT_NE(caller_env, nullptr);
    EXPECT_EQ(xls_dslx_parametric_env_get_binding_count(caller_env), 0);
    ExpectOwnedText(xls_dslx_parametric_env_to_string(caller_env), "{}");

    data_clone = xls_dslx_invocation_callee_data_clone(data);
    env_clone = xls_dslx_parametric_env_clone(env);
    value_clone = xls_dslx_interp_value_clone(value);
    ASSERT_NE(data_clone, nullptr);
    ASSERT_NE(env_clone, nullptr);
    ASSERT_NE(value_clone, nullptr);
    xls_dslx_invocation_callee_data_array_free(array);
    array = nullptr;
    xls_dslx_typechecked_module_free(tm);
    tm = nullptr;
    xls_dslx_import_data_free(owner);
    owner = nullptr;

    // All original borrowed handles are now invalid. Only the owned clones
    // and new views borrowed from those clones may be observed below.
    ASSERT_NO_FATAL_FAILURE(ExpectBindings(
        xls_dslx_invocation_callee_data_get_callee_bindings(data_clone),
        value_case.expected, control));
    ASSERT_NO_FATAL_FAILURE(
        ExpectBindings(env_clone, value_case.expected, control));
    ExpectOwnedText(xls_dslx_interp_value_to_string(value_clone),
                    value_case.expected);
    ExpectOwnedText(
        xls_dslx_parametric_env_to_string(
            xls_dslx_invocation_callee_data_get_caller_bindings(data_clone)),
        "{}");

    // Releasing sibling clones must not change the final owner's formatting.
    switch (last_clone) {
      case LastClone::kInvocation:
        xls_dslx_parametric_env_free(env_clone);
        env_clone = nullptr;
        xls_dslx_interp_value_free(value_clone);
        value_clone = nullptr;
        ASSERT_NO_FATAL_FAILURE(ExpectBindings(
            xls_dslx_invocation_callee_data_get_callee_bindings(data_clone),
            value_case.expected, control));
        break;
      case LastClone::kEnvironment:
        xls_dslx_invocation_callee_data_free(data_clone);
        data_clone = nullptr;
        xls_dslx_interp_value_free(value_clone);
        value_clone = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            ExpectBindings(env_clone, value_case.expected, control));
        break;
      case LastClone::kValue:
        xls_dslx_invocation_callee_data_free(data_clone);
        data_clone = nullptr;
        xls_dslx_parametric_env_free(env_clone);
        env_clone = nullptr;
        ExpectOwnedText(xls_dslx_interp_value_to_string(value_clone),
                        value_case.expected);
        break;
    }
  }
}

TEST(CApiDslxSumFuzzTest, DeterministicWitnesses) {
  struct Witness {
    ValueShape shape;
    Constructor constructor;
    uint32_t payload;
    uint32_t companion;
    LastClone last_clone;
  };
  for (const Witness& witness : {
           Witness{ValueShape::kDirect, Constructor::kNone, 0, 0,
                   LastClone::kInvocation},
           {ValueShape::kDirect, Constructor::kSome, 255, 1,
            LastClone::kEnvironment},
           {ValueShape::kDirect, Constructor::kOther, 128, 127,
            LastClone::kValue},
           {ValueShape::kTuple, Constructor::kSome, 0, 255,
            LastClone::kInvocation},
           {ValueShape::kTuple, Constructor::kOther, 255, 0, LastClone::kValue},
           {ValueShape::kArray, Constructor::kNone, 0, 255,
            LastClone::kEnvironment},
           {ValueShape::kArray, Constructor::kSome, 255, 0, LastClone::kValue},
           {ValueShape::kNested, Constructor::kOther, 127, 128,
            LastClone::kInvocation},
           {ValueShape::kRecord, Constructor::kSome, 1, 255,
            LastClone::kEnvironment},
           {ValueShape::kMixed, Constructor::kOther, 128, 1, LastClone::kValue},
       }) {
    ASSERT_NO_FATAL_FAILURE(InvocationBindingsKeepSumFormatting(
        witness.shape, witness.constructor, witness.payload, witness.companion,
        witness.last_clone));
  }
}

FUZZ_TEST(CApiDslxSumFuzzTest, InvocationBindingsKeepSumFormatting)
    .WithDomains(fuzztest::ElementOf<ValueShape>(
                     {ValueShape::kDirect, ValueShape::kTuple,
                      ValueShape::kArray, ValueShape::kNested,
                      ValueShape::kRecord, ValueShape::kMixed}),
                 fuzztest::ElementOf<Constructor>({Constructor::kNone,
                                                   Constructor::kSome,
                                                   Constructor::kOther}),
                 fuzztest::InRange<uint32_t>(0, 255),
                 fuzztest::InRange<uint32_t>(0, 255),
                 fuzztest::ElementOf<LastClone>({LastClone::kInvocation,
                                                 LastClone::kEnvironment,
                                                 LastClone::kValue}));

}  // namespace
}  // namespace xls
