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

#include "xls/dslx/run_routines/run_routines.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "re2/re2.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/file/temp_file.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_generator.h"
#include "xls/dslx/ir_convert/ir_converter.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/run_routines/ir_test_runner.h"
#include "xls/dslx/run_routines/run_comparator.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/warning_kind.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/events.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/jit/function_jit.h"

namespace xls::dslx {
namespace {

// A fake mangled IR name for use in some direct DoQuickCheck calls (will be
// used as a JIT cache key).
constexpr std::string_view kFakeIrName = "__test__fake";

// Matcher that helps us compare against a TestResultData value and what it
// reflects as the summary result status and counts.
MATCHER_P4(IsTestResult, result, ran_count, skipped_count, failed_count, "") {
  if (result != arg.result()) {
    *result_listener << "got " << arg.result() << " want " << result;
    return false;
  }
  if (ran_count != arg.GetRanCount()) {
    *result_listener << "ran count got " << arg.GetRanCount() << " want "
                     << ran_count;
    return false;
  }
  if (skipped_count != arg.GetSkippedCount()) {
    *result_listener << "skipped count got " << arg.GetSkippedCount()
                     << " want " << skipped_count;
    return false;
  }
  if (failed_count != arg.GetFailedCount()) {
    *result_listener << "skipped count got " << arg.GetFailedCount() << " want "
                     << failed_count;
    return false;
  }
  return true;
}

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

class CountingRunComparator : public RunComparator {
 public:
  explicit CountingRunComparator(CompareMode mode) : RunComparator(mode) {}

  absl::StatusOr<InterpreterResult<xls::Value>> RunIrFunction(
      std::string_view ir_name, xls::Function* ir_function,
      absl::Span<const xls::Value> ir_args) override {
    ++invocation_count_;
    invocation_args_.emplace_back(ir_args.begin(), ir_args.end());
    return RunComparator::RunIrFunction(ir_name, ir_function, ir_args);
  }

  int64_t invocation_count() const { return invocation_count_; }
  const std::vector<std::vector<xls::Value>>& invocation_args() const {
    return invocation_args_;
  }

 private:
  int64_t invocation_count_ = 0;
  std::vector<std::vector<xls::Value>> invocation_args_;
};

enum class RunnerType : int8_t {
  kDslxInterpreter,
  kIrJit,
  kIrInterpreter,
  kIrJitProcScoped,
  kIrInterpreterProcScoped,
};

template <typename Sink>
void AbslStringify(Sink& sink, const RunnerType& v) {
  switch (v) {
    case RunnerType::kDslxInterpreter:
      absl::Format(&sink, "DslxInterpreterTestRunner");
      break;
    case RunnerType::kIrJit:
      absl::Format(&sink, "IrJitTestRunner");
      break;
    case RunnerType::kIrInterpreter:
      absl::Format(&sink, "IrInterpreterTestRunner");
      break;
    case RunnerType::kIrJitProcScoped:
      absl::Format(&sink, "IrJitTestRunnerProcScoped");
      break;
    case RunnerType::kIrInterpreterProcScoped:
      absl::Format(&sink, "IrInterpreterTestRunnerProcScoped");
      break;
  }
}
}  // namespace

class RunRoutinesTest : public testing::TestWithParam<RunnerType> {
 public:
  absl::StatusOr<TestResultData> ParseAndTest(
      std::string_view program, std::string_view module_name,
      std::string_view filename, const ParseAndTestOptions& original_options) {
    DslxInterpreterTestRunner dslx;
    IrInterpreterTestRunner ir;
    IrJitTestRunner jit;
    AbstractTestRunner* runner;
    ParseAndTestOptions options(original_options);
    switch (GetParam()) {
      case RunnerType::kDslxInterpreter:
        runner = &dslx;
        break;
      case RunnerType::kIrJit:
        runner = &jit;
        break;
      case RunnerType::kIrInterpreter:
        runner = &ir;
        break;
      case RunnerType::kIrJitProcScoped:
        runner = &jit;
        options.convert_options.lower_to_proc_scoped_channels = true;
        break;
      case RunnerType::kIrInterpreterProcScoped:
        runner = &ir;
        options.convert_options.lower_to_proc_scoped_channels = true;
        break;
    }
    return runner->ParseAndTest(program, module_name, filename, options);
  }
};

using ParseAndTestTest = RunRoutinesTest;

TEST_P(RunRoutinesTest, TestInvokedFunctionDoesJit) {
  constexpr const char* kProgram = R"(
fn unit() -> () { () }

#[test]
fn test_simple() { unit() }
)";
  if (GetParam() != RunnerType::kDslxInterpreter) {
    GTEST_SKIP()
        << "comparator only supported on dslx interpreter for non-quickchecks";
  }
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.run_comparator = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  ASSERT_EQ(jit_comparator.jit_cache_.size(), 1);
  EXPECT_EQ(jit_comparator.jit_cache_.begin()->first, "__test__unit");
}

TEST_P(RunRoutinesTest, QuickcheckInvokedFunctionDoesJit) {
  constexpr const char* kProgram = R"(
fn id(x: bool) -> bool { x }

#[quickcheck(test_count=1024)]
fn trivial(x: u5) -> bool { id(true) }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{2};
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));

  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  ASSERT_EQ(jit_comparator.jit_cache_.size(), 1);
  EXPECT_EQ(jit_comparator.jit_cache_.begin()->first, "__test__trivial");
}

// A simple exhaustive quickcheck that passes for all values.
TEST_P(RunRoutinesTest, QuickcheckExhaustive) {
  constexpr const char* kProgram = R"(
fn id(x: bool) -> bool { x }

#[quickcheck(exhaustive)]
fn trivial(x: u2) -> bool { id(true) }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));

  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  ASSERT_EQ(jit_comparator.jit_cache_.size(), 1);
  EXPECT_EQ(jit_comparator.jit_cache_.begin()->first, "__test__trivial");
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveIntegerAggregates) {
  constexpr std::string_view kProgram = R"(
struct Packet { data: (sN[2][2], uN[1]) }

#[quickcheck(exhaustive)]
fn qc(x: Packet, flag: bool) -> bool { x == x && flag == flag }
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  // Every combination of the six input bits must reach the IR runner, including
  // the signed array elements nested inside the tuple and struct.
  std::vector<std::vector<Value>> expected;
  for (int64_t a = 0; a < 4; ++a) {
    for (int64_t b = 0; b < 4; ++b) {
      for (bool bit : {false, true}) {
        for (bool flag : {false, true}) {
          XLS_ASSERT_OK_AND_ASSIGN(
              Value array,
              Value::Array({Value(UBits(a, 2)), Value(UBits(b, 2))}));
          expected.push_back(
              {Value::Tuple({Value::Tuple({array, Value::Bool(bit)})}),
               Value::Bool(flag)});
        }
      }
    }
  }
  EXPECT_THAT(jit_comparator.invocation_args(),
              ::testing::UnorderedElementsAreArray(expected));
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveIntegerAggregateCounterexample) {
  constexpr std::string_view kProgram = R"(
struct Packet { data: (sN[2][2], uN[1]) }

#[quickcheck(exhaustive)]
fn qc(x: Packet, other: s2) -> bool {
  !(x.data.0 == [s2:-1, s2:-1] && x.data.1 == u1:1 && other == s2:-1)
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  ASSERT_EQ(result.GetFailureMessages().size(), 1);
  EXPECT_THAT(result.GetFailureMessages().front(),
              HasSubstr("tests: [(([s2:-1, s2:-1], u1:1)), s2:-1]"));
  ASSERT_FALSE(jit_comparator.invocation_args().empty());
  const std::vector<Value>& failing_args =
      jit_comparator.invocation_args().back();
  ASSERT_EQ(failing_args.size(), 2);
  EXPECT_EQ(failing_args.at(1), Value(UBits(3, 2)));
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveEnumWithFail) {
  constexpr const char* kProgram = R"(
enum MyEnum: u2 {
  A = 0,
  B = 1,
  C = 2,
}

#[quickcheck(exhaustive)]
fn qc(x: MyEnum) -> bool {
    match x {
        MyEnum::A | MyEnum::B | MyEnum::C => true,
        _ => fail!("impossible_value", false),
    }
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.parse_and_typecheck_options.warnings =
      DisableWarning(kAllWarningsSet, WarningKind::kAlreadyExhaustiveMatch);
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  ASSERT_EQ(jit_comparator.jit_cache_.size(), 1);
  EXPECT_EQ(jit_comparator.jit_cache_.begin()->first, "__test__qc");
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveRejectsInvalidNestedSumInputs) {
  constexpr const char* kProgram = R"(
enum Inner: u2 {
  A = 0,
  B(u1) = 1,
}

enum Outer {
  Wrap(Inner),
}

#[quickcheck(exhaustive)]
fn qc(x: Outer) -> bool {
  match x {
    Outer::Wrap(inner) => match inner {
      Inner::A => true,
      Inner::B(_) => true,
      invalid! => false,
    },
  }
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 4);
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveRejectsInvalidNestedEnumInputs) {
  constexpr std::string_view kProgram = R"(
enum Sparse: u2 { A = 0, B = 2 }
struct Packet { data: (Sparse[1], u1) }

#[quickcheck(exhaustive)]
fn qc(flag: bool, x: Packet) -> bool {
  match x.data.0[u32:0] {
    Sparse::A | Sparse::B => true,
    _ => false,
  }
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.parse_and_typecheck_options.warnings =
      DisableWarning(kAllWarningsSet, WarningKind::kAlreadyExhaustiveMatch);
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  // Two declared enum values and two ordinary bits yield eight valid inputs,
  // even though the enum is nested and a plain integer parameter comes first.
  EXPECT_EQ(jit_comparator.invocation_count(), 8);
}

// Exhaustive enumeration may visit several raw images for one constructor.
// Every executed image must nevertheless have freshly constructed padding.
TEST_P(RunRoutinesTest, QuickcheckExhaustiveConstructsCanonicalNestedInputs) {
  constexpr const char* kProgram = R"(
enum Inner: u2 { Small(u1) = 0, Big(u2) = 1 }
enum Outer: u1 { Wrapped(Inner) = 0, Wide(u5) = 1 }
struct Inputs { values: (Outer[1],) }

#[quickcheck(exhaustive)]
fn qc(_x: Inputs) -> bool { true }
)";
  CountingRunComparator comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  // Of 64 raw candidates, 16 have an undeclared active Inner tag. Keep the
  // remaining 48 executions; canonical construction is not deduplication.
  EXPECT_EQ(comparator.invocation_count(), 48);
  for (const std::vector<Value>& arguments : comparator.invocation_args()) {
    ASSERT_EQ(arguments.size(), 1);
    const Value& outer = arguments.front().element(0).element(0).element(0);
    if (outer.element(0) == Value(UBits(0, 1))) {
      XLS_ASSERT_OK_AND_ASSIGN(uint64_t payload,
                               outer.element(1).element(0).bits().ToUint64());
      EXPECT_EQ(payload & 0x10, 0);  // Outer::Wrapped's padding bit.
      const uint64_t inner_tag = (payload >> 2) & 3;
      EXPECT_LT(inner_tag, 2);
      if (inner_tag == 0) {
        EXPECT_EQ(payload & 2, 0);  // Inner::Small's padding bit.
      }
    }
  }
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveWideSignedSum) {
  constexpr std::string_view kProgram = R"(
enum Sparse: s40 { Only() = -1 }

#[quickcheck(exhaustive)]
fn qc(_prefix: bool, value: Sparse) -> bool {
  assert_eq(value, Sparse::Only());
  true
}
)";
  CountingRunComparator comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  // Only the last tag of each 40-bit range is declared. Skipping must carry
  // into the preceding ordinary argument and stop past the complete domain.
  // The public wrapper supplies the implicit token/activation itself.
  const Value only = Value::Tuple({Value(UBits((uint64_t{1} << 40) - 1, 40)),
                                   Value::Tuple({Value(UBits(0, 0))})});
  const std::vector<std::vector<Value>> expected = {{Value::Bool(false), only},
                                                    {Value::Bool(true), only}};
  EXPECT_THAT(comparator.invocation_args(),
              ::testing::ElementsAreArray(expected));
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveSignedTagsUseRawOrder) {
  constexpr std::string_view kProgram = R"(
enum Sparse: s3 { Last() = -1, First() = 1 }

#[quickcheck(exhaustive)]
fn qc(_prefix: bool, _value: Sparse) -> bool { true }
)";
  CountingRunComparator comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  const auto sum = [](uint64_t tag) {
    return Value::Tuple(
        {Value(UBits(tag, 3)), Value::Tuple({Value(UBits(0, 0))})});
  };
  // Declaration order is [7, 1], but the next raw tag after zero is 1.
  // Preserve that order on both sides of the carry into the ordinary prefix.
  const std::vector<std::vector<Value>> expected = {
      {Value::Bool(false), sum(1)},
      {Value::Bool(false), sum(7)},
      {Value::Bool(true), sum(1)},
      {Value::Bool(true), sum(7)}};
  EXPECT_THAT(comparator.invocation_args(),
              ::testing::ElementsAreArray(expected));
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveSumArrayPreservesRawOrder) {
  constexpr std::string_view kProgram = R"(
enum Leaf: u2 { Small(u1) = 0, Big(u2) = 2 }

#[quickcheck(exhaustive)]
fn qc(_values: Leaf[2]) -> bool { true }
)";
  CountingRunComparator comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  std::vector<std::vector<Value>> expected;
  // Reference the old raw-index traversal explicitly. Top-level IR arrays
  // put their first element in the more-significant bits. Small's two raw
  // padding images must still produce two equal canonical executions.
  for (uint64_t first = 0; first < 16; ++first) {
    for (uint64_t second = 0; second < 16; ++second) {
      const uint64_t first_tag = first >> 2;
      const uint64_t second_tag = second >> 2;
      if ((first_tag == 0 || first_tag == 2) &&
          (second_tag == 0 || second_tag == 2)) {
        const Value a = Value::Tuple(
            {Value(UBits(first_tag, 2)),
             Value::Tuple(
                 {Value(UBits(first & (first_tag == 0 ? 1 : 3), 2))})});
        const Value b = Value::Tuple(
            {Value(UBits(second_tag, 2)),
             Value::Tuple(
                 {Value(UBits(second & (second_tag == 0 ? 1 : 3), 2))})});
        XLS_ASSERT_OK_AND_ASSIGN(Value values, Value::Array({a, b}));
        expected.push_back({std::move(values)});
      }
    }
  }
  ASSERT_EQ(expected.size(), 64);
  EXPECT_THAT(comparator.invocation_args(),
              ::testing::ElementsAreArray(expected));
}

TEST_P(RunRoutinesTest,
       QuickcheckExhaustivePackedSumArraysPreserveMultiplicity) {
  constexpr std::string_view kProgram = R"(
enum Leaf: u2 { Small(u1) = 0, Big(u2) = 2 }
struct Pair { flag: bool, value: Leaf }
enum Outer: u2 { Wrapped(Pair[2]) = 1, Wide(u10) = 3 }

#[quickcheck(exhaustive)]
fn qc(_prefix: bool, _value: Outer) -> bool { true }
)";
  CountingRunComparator comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  std::vector<std::vector<Value>> expected;
  // Exhaust all 13 raw bits as the old runner did, rejecting only undeclared
  // active tags. Wrapped's array element zero occupies the low five bits;
  // each Pair puts its flag above the Leaf. Wide never inspects those bits
  // as nested tags. Preserve both ordinary prefixes and padding duplicates.
  for (uint64_t raw = 0; raw < 8192; ++raw) {
    const uint64_t outer_tag = (raw >> 10) & 3;
    uint64_t payload = raw & 1023;
    bool valid = false;
    if (outer_tag == 1) {
      const uint64_t low_tag = (payload >> 2) & 3;
      const uint64_t high_tag = (payload >> 7) & 3;
      valid =
          (low_tag == 0 || low_tag == 2) && (high_tag == 0 || high_tag == 2);
      if (low_tag == 0) {
        payload &= ~(uint64_t{1} << 1);
      }
      if (high_tag == 0) {
        payload &= ~(uint64_t{1} << 6);
      }
    } else if (outer_tag == 3) {
      valid = true;
    }
    if (valid) {
      const Value value =
          Value::Tuple({Value(UBits(outer_tag, 2)),
                        Value::Tuple({Value(UBits(payload, 10))})});
      expected.push_back({Value::Bool((raw >> 12) != 0), value});
    }
  }
  ASSERT_EQ(expected.size(), 2560);
  EXPECT_THAT(comparator.invocation_args(),
              ::testing::ElementsAreArray(expected));
}

TEST_P(RunRoutinesTest, SemanticSumRuntimeAggregatePatternsCompareValues) {
  constexpr const char* kProgram = R"(
struct Pair { first: u8, second: u8 }
enum E { Array(u8[2]), Tuple((u8, u8)), Record(Pair), Scalar(u8) }
const EXPECTED = u8[2]:[1, 2];

fn array_pattern(x: E, a: u8[2]) -> u8 {
  match x { E::Array(a) => u8:1, E::Array(_) => u8:2, _ => u8:0 }
}
fn tuple_pattern(x: E, a: (u8, u8)) -> u8 {
  match x { E::Tuple(a) => u8:1, E::Tuple(_) => u8:2, _ => u8:0 }
}
fn record_pattern(x: E, a: Pair) -> u8 {
  match x { E::Record(a) => u8:1, E::Record(_) => u8:2, _ => u8:0 }
}
fn scalar_pattern(x: E, a: u8) -> u8 {
  match x { E::Scalar(a) => u8:1, E::Scalar(_) => u8:2, _ => u8:0 }
}
fn constant_pattern(x: E) -> u8 {
  match x { E::Array(EXPECTED) => u8:1, E::Array(_) => u8:2, _ => u8:0 }
}

#[test]
fn check_patterns() {
  assert_eq(array_pattern(E::Array(EXPECTED), EXPECTED), u8:1);
  assert_eq(array_pattern(E::Array(EXPECTED), u8[2]:[2, 1]), u8:2);
  assert_eq(tuple_pattern(E::Tuple((u8:1, u8:2)), (u8:1, u8:2)), u8:1);
  assert_eq(tuple_pattern(E::Tuple((u8:1, u8:2)), (u8:2, u8:1)), u8:2);
  let pair = Pair { first: u8:1, second: u8:2 };
  assert_eq(record_pattern(E::Record(pair), pair), u8:1);
  assert_eq(record_pattern(E::Record(pair), Pair { first: u8:2, second: u8:1 }), u8:2);
  assert_eq(scalar_pattern(E::Scalar(u8:1), u8:1), u8:1);
  assert_eq(scalar_pattern(E::Scalar(u8:1), u8:2), u8:2);
  assert_eq(constant_pattern(E::Array(EXPECTED)), u8:1);
  assert_eq(constant_pattern(E::Array(u8[2]:[2, 1])), u8:2);
}
)";
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(RunRoutinesTest, QuickcheckCountedGeneratesSparseSemanticSums) {
  constexpr const char* kProgram = R"(
enum Sparse: u16 {
  Only(u1) = 7,
}

#[quickcheck(test_count=8)]
fn qc(x: Sparse) -> bool {
  match x {
    Sparse::Only(_) => true,
    invalid! => false,
  }
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{2};
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 8);
}

TEST_P(RunRoutinesTest, QuickcheckCountedGeneratesOnlyInhabitedArrayVariants) {
  constexpr const char* kProgram = R"(
enum Never {}

enum Inner: u2 {
  A = 2,
  B(u1) = 0,
}

enum Choice: u2 {
  Impossible(Never) = 0,
  Byte(u8) = 2,
  Nested(Inner) = 1,
  Flag = 3,
}

#[quickcheck(test_count=16)]
fn qc(values: Choice[4]) -> bool {
  values[0] == values[0] && values[3] == values[3]
}
)";
  const auto run_with_fixed_seed =
      [this, kProgram](
          CountingRunComparator& comparator) -> absl::StatusOr<TestResultData> {
    ParseAndTestOptions options;
    options.vfs_factory = [kProgram] {
      return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
    };
    options.quickcheck_runner = &comparator;
    options.seed = int64_t{2};
    return ParseAndTest(kProgram, "test", "test.x", options);
  };

  CountingRunComparator jit_comparator(CompareMode::kJit);
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           run_with_fixed_seed(jit_comparator));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 16);

  // Keep the constructor sequence and payload generated before caching.
  EXPECT_THAT(
      jit_comparator.invocation_args().front(),
      ::testing::ElementsAre(xls::Value::ArrayOrDie(
          {xls::Value::Tuple({xls::Value(UBits(2, 2)),
                              xls::Value::Tuple({xls::Value(UBits(69, 8))})}),
           xls::Value::Tuple({xls::Value(UBits(3, 2)),
                              xls::Value::Tuple({xls::Value(UBits(0, 8))})}),
           xls::Value::Tuple({xls::Value(UBits(3, 2)),
                              xls::Value::Tuple({xls::Value(UBits(0, 8))})}),
           xls::Value::Tuple(
               {xls::Value(UBits(1, 2)),
                xls::Value::Tuple({xls::Value(UBits(4, 8))})})})));

  bool saw_byte = false;
  bool saw_nested = false;
  bool saw_flag = false;
  bool saw_inner_a = false;
  bool saw_inner_b = false;
  for (const std::vector<xls::Value>& args : jit_comparator.invocation_args()) {
    ASSERT_EQ(args.size(), 1);
    ASSERT_TRUE(args.front().IsArray());
    for (const xls::Value& value : args.front().elements()) {
      ASSERT_TRUE(value.IsTuple());
      ASSERT_FALSE(value.elements().empty());
      ASSERT_TRUE(value.element(0).IsBits());
      XLS_ASSERT_OK_AND_ASSIGN(uint64_t discriminant,
                               value.element(0).bits().ToUint64());
      if (discriminant == 2) {
        saw_byte = true;
      } else if (discriminant == 1) {
        saw_nested = true;
        ASSERT_TRUE(value.element(1).IsTuple());
        ASSERT_EQ(value.element(1).elements().size(), 1);
        ASSERT_TRUE(value.element(1).element(0).IsBits());
        XLS_ASSERT_OK_AND_ASSIGN(uint64_t payload,
                                 value.element(1).element(0).bits().ToUint64());
        const uint64_t inner_discriminant = payload >> 1;
        if (inner_discriminant == 2) {
          saw_inner_a = true;
        } else if (inner_discriminant == 0) {
          saw_inner_b = true;
        } else {
          ADD_FAILURE() << "Generated invalid inner discriminant "
                        << inner_discriminant;
        }
      } else if (discriminant == 3) {
        saw_flag = true;
      } else {
        ADD_FAILURE() << "Generated uninhabited constructor discriminant "
                      << discriminant;
      }
    }
  }
  EXPECT_TRUE(saw_byte);
  EXPECT_TRUE(saw_nested);
  EXPECT_TRUE(saw_flag);
  EXPECT_TRUE(saw_inner_a);
  EXPECT_TRUE(saw_inner_b);

  CountingRunComparator repeat_comparator(CompareMode::kJit);
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData repeated_result,
                           run_with_fixed_seed(repeat_comparator));
  EXPECT_THAT(repeated_result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_args(),
            repeat_comparator.invocation_args());
}

TEST_P(RunRoutinesTest, QuickcheckFailureFormatsSemanticSumCounterexample) {
  constexpr const char* kProgram = R"(
enum Maybe: u1 {
  Some(u8) = 1,
}

#[quickcheck(test_count=1)]
fn qc(x: Maybe) -> bool {
  false
}
)";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{2};
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  ASSERT_EQ(result.GetFailureMessages().size(), 1);
  EXPECT_THAT(result.GetFailureMessages().front(),
              HasSubstr("tests: [Maybe::Some(u8:"));
}

TEST_P(RunRoutinesTest, QuickcheckCountedGeneratesSumsInStructArrayAndTuple) {
  constexpr const char* kProgram = R"(
enum Inner: u3 {
  A = 1,
  B(u1) = 6,
}

struct Wrapper {
  values: Inner[2],
  extra: (Inner,),
}

fn is_valid(x: Inner) -> bool {
  match x {
    Inner::A => true,
    Inner::B(_) => true,
    invalid! => false,
  }
}

#[quickcheck(test_count=16)]
fn qc(x: Wrapper) -> bool {
  is_valid(x.values[0]) && is_valid(x.values[1]) && is_valid(x.extra.0)
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{2};
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 16);
}

// Verifies: source tokens work in counted and exhaustive QuickCheck runs.
// Catches: routing zero-bit tokens through the bits-only generator.
TEST_P(RunRoutinesTest, QuickcheckSupportsSourceToken) {
  constexpr std::pair<std::string_view, int64_t> kPrograms[] = {
      {R"(
#![feature(type_inference_v2)]
#[quickcheck(test_count=3)]
fn qc(_t: token) -> bool { true }
)",
       3},
      {R"(
#![feature(type_inference_v2)]
#[quickcheck(exhaustive)]
fn qc(_t: token) -> bool { true }
)",
       1},
  };
  for (const auto& [program, expected_count] : kPrograms) {
    SCOPED_TRACE(program);
    CountingRunComparator comparator(CompareMode::kJit);
    ParseAndTestOptions options;
    options.vfs_factory = [program] {
      return std::make_unique<UniformContentFilesystem>(program, "test.x");
    };
    options.quickcheck_runner = &comparator;
    XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                             ParseAndTest(program, "test", "test.x", options));
    EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
    EXPECT_EQ(comparator.invocation_count(), expected_count);
    for (const std::vector<Value>& arguments : comparator.invocation_args()) {
      EXPECT_THAT(arguments, ::testing::ElementsAre(Value::Token()));
    }
  }
}

// Verifies: source token and sum arguments coexist with implicit IR controls.
// Catches: confusing source tokens with the wrapper's synthetic token.
TEST_P(RunRoutinesTest, QuickcheckCountedSupportsImplicitTokenSumInputs) {
  constexpr const char* kProgram = R"(
enum Sparse: u4 {
  Only(u1) = 7,
}

#[quickcheck(test_count=8)]
fn qc(_t: token, x: Sparse) -> bool {
  trace_fmt!("{}", x);
  match x {
    Sparse::Only(_) => true,
    invalid! => false,
  }
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{2};
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 8);
  for (const std::vector<Value>& arguments : jit_comparator.invocation_args()) {
    // The public wrapper supplies the implicit token and activation itself.
    // The token argument here is the actual source parameter.
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_TRUE(arguments.at(0).IsToken());
    EXPECT_EQ(arguments.at(1).element(0), Value(UBits(7, 4)));
  }
}

TEST_P(RunRoutinesTest, QuickcheckExhaustiveSupportsImplicitTokenSumInputs) {
  constexpr const char* kProgram = R"(
enum Sparse: u4 {
  Only(u1) = 7,
}

#[quickcheck(exhaustive)]
fn qc(x: Sparse) -> bool {
  trace_fmt!("{}", x);
  match x {
    Sparse::Only(_) => true,
    invalid! => false,
  }
}
)";
  CountingRunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
  EXPECT_EQ(jit_comparator.invocation_count(), 2);
  for (const std::vector<Value>& arguments : jit_comparator.invocation_args()) {
    ASSERT_EQ(arguments.size(), 1);
    EXPECT_EQ(arguments.front().element(0), Value(UBits(7, 4)));
  }
}

TEST(QuickcheckTest, ExhaustiveSumInputsWithImplicitToken) {
  constexpr std::string_view kProgram = R"(
enum Sparse: u40 { Only(u1) = 1099511627775 }

#[quickcheck(exhaustive)]
fn qc(x: Sparse) -> bool {
  trace_fmt!("{}", x);
  true
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto package,
      ConvertModuleToPackage(tm.module, &import_data, ConvertOptions{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__itok__test__qc"));
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * fn_type,
                           tm.type_info->GetItemAs<FunctionType>(
                               tm.module->GetQuickChecks().front()->fn()));
  CountingRunComparator comparator(CompareMode::kJit);
  XLS_ASSERT_OK_AND_ASSIGN(
      auto result, DoQuickCheck(/*requires_implicit_token=*/true, fn_type,
                                ir_function, "__itok__test__qc", &comparator,
                                /*seed=*/0, QuickCheckTestCases::Exhaustive()));
  EXPECT_EQ(result.results.size(), 2);
  for (const std::vector<Value>& arguments : comparator.invocation_args()) {
    ASSERT_EQ(arguments.size(), 3);
    EXPECT_TRUE(arguments.at(0).IsToken());
    EXPECT_EQ(arguments.at(1), Value::Bool(true));
    EXPECT_EQ(arguments.at(2).element(0),
              Value(UBits((uint64_t{1} << 40) - 1, 40)));
  }
}

// Verifies: sum traces print correctly in the IR interpreter and LLVM JIT.
// Catches: backend-specific escaping, signedness, or constructor formatting.
TEST(QuickcheckTest, IrBackendsTraceSignedAndStructuredSumPayloads) {
  constexpr const char* kProgram = R"(
enum SignedValue { Value(s8) }
struct Point { a: u8 }
enum Message { Struct { a: u8 }, Tuple(Point) }

#[quickcheck(test_count=1)]
fn qc() -> bool {
  trace_fmt!("{}", SignedValue::Value(s8:-1));
  trace_fmt!("{}", Point { a: u8:1 });
  trace_fmt!("{}", Message::Struct { a: u8:1 });
  trace_fmt!("{}", Message::Tuple(Point { a: u8:1 }));
  true
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto package,
      ConvertModuleToPackage(tm.module, &import_data, ConvertOptions{}));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__itok__test__qc"));
  const std::vector<Value> arguments = {Value::Token(), Value::Bool(true)};
  auto expect_trace = [](const InterpreterResult<Value>& result) {
    EXPECT_EQ(result.value, Value::Tuple({Value::Token(), Value::Bool(true)}));
    EXPECT_THAT(result.events.GetAssertMessages(), ::testing::IsEmpty());
    EXPECT_THAT(result.events.GetTraceMessageStrings(),
                ::testing::ElementsAre("SignedValue::Value(-1)", "Point{a: 1}",
                                       "Message::Struct {a: 1 }",
                                       "Message::Tuple(Point{a: 1})"));
  };
  {
    SCOPED_TRACE("IR interpreter");
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             InterpretFunction(ir_function, arguments));
    expect_trace(result);
  }
  {
    SCOPED_TRACE("LLVM JIT");
    XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<FunctionJit> jit,
                             FunctionJit::Create(ir_function));
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run(arguments));
    expect_trace(result);
  }
}

TEST_P(RunRoutinesTest, FilteredUninhabitedQuickcheckKeepsSharedPackage) {
  constexpr const char* kProgram = R"(
enum Never {}

#[quickcheck(test_count=1)]
fn excluded(value: Never) -> bool { true }

#[quickcheck(test_count=1)]
fn selected_first(value: u1) -> bool { value == value }

#[quickcheck(test_count=1)]
fn selected_second(value: u1) -> bool { value == value }
)";
  class SharedPackageComparator : public CountingRunComparator {
   public:
    SharedPackageComparator() : CountingRunComparator(CompareMode::kJit) {}

    absl::StatusOr<InterpreterResult<Value>> RunIrFunction(
        std::string_view ir_name, xls::Function* ir_function,
        absl::Span<const Value> ir_args) override {
      // A per-property conversion contains only that property's reachable
      // functions; the shared conversion must contain both selected properties.
      EXPECT_TRUE(
          ir_function->package()->GetFunction("__test__selected_first").ok());
      EXPECT_TRUE(
          ir_function->package()->GetFunction("__test__selected_second").ok());
      return CountingRunComparator::RunIrFunction(ir_name, ir_function,
                                                  ir_args);
    }
  } comparator;
  const RE2 test_filter("selected_.*");
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.test_filter = &test_filter;
  options.quickcheck_runner = &comparator;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kProgram, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 3, 1, 0));
  EXPECT_EQ(comparator.invocation_count(), 2);
}

TEST_P(RunRoutinesTest, EmptySemanticSum) {
  constexpr const char* kProgram = R"(
enum EmptyEnum: u2 {
}

#[quickcheck(exhaustive)]
fn qc(x: EmptyEnum) -> bool {
    true
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  ASSERT_EQ(result.GetFailureMessages().size(), 1);
  std::string failure_message = result.GetFailureMessages()[0];
  EXPECT_THAT(failure_message,
              HasSubstr("quickcheck of `qc` rejected all input samples"));
}

TEST_P(RunRoutinesTest, AggregateContainingEmptySemanticSum) {
  constexpr const char* kProgram = R"(
enum Empty {}

struct Wrapper {
  empty: Empty,
}

#[quickcheck(exhaustive)]
fn qc(x: Wrapper) -> bool {
    true
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  ASSERT_EQ(result.GetFailureMessages().size(), 1);
  std::string failure_message = result.GetFailureMessages()[0];
  EXPECT_THAT(failure_message,
              HasSubstr("quickcheck of `qc` rejected all input samples"));
}

TEST_P(RunRoutinesTest, SemanticSumWithOnlyUninhabitedPayloadVariant) {
  constexpr const char* kProgram = R"(
enum Empty {}

enum Only {
  V(Empty),
}

#[quickcheck(exhaustive)]
fn qc(x: Only) -> bool {
    true
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  ASSERT_EQ(result.GetFailureMessages().size(), 1);
  std::string failure_message = result.GetFailureMessages()[0];
  EXPECT_THAT(failure_message,
              HasSubstr("quickcheck of `qc` rejected all input samples"));
}

// Quickcheck function that takes an `xN` based value and returns an `xN` based
// value.
TEST_P(RunRoutinesTest, QuickcheckXn) {
  constexpr const char* kProgram = R"(
const S: bool = false;
type MyBool = xN[false][1];
type MyBool2 = xN[MyBool:0][1];
#[quickcheck(exhaustive)]
fn qc(x: xN[S][4]) -> MyBool2 {
    let lsb = x[0 +: MyBool2];
    if lsb { lsb } else { MyBool2:1 }
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(RunRoutinesTest, QuickcheckImplicitTokenRoutineThatFails) {
  constexpr const char* kProgram = R"(
#[quickcheck]
fn qc_with_implicit_token(x: u2) -> bool {
    trace_fmt!("{}", x);  // make an implicit token calling convention
    false  // fail immediately
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

TEST_P(RunRoutinesTest, GithubIssue1586) {
  constexpr const char* kProgram = R"(
import apfloat;
import bfloat16;
import float32;

const BF16_TOTAL_SZ: u32 = u32:16;

#[quickcheck]
fn bfloat16_bits_to_float32_bits_upcast_is_zero_pad(x: bits[BF16_TOTAL_SZ]) -> bool {
    (x ++ bits[u32:16]:0 ==
    float32::flatten(
        apfloat::upcast_daz<float32::F32_EXP_SZ, float32::F32_FRACTION_SZ>(bfloat16::unflatten(x))))
}
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;

  const std::filesystem::path root("/");
  auto get_stdlib_contents = [](std::string_view filename) -> std::string {
    std::filesystem::path path =
        std::filesystem::path(kDefaultDslxStdlibPath) / filename;
    return GetFileContents(path).value();
  };
  options.parse_and_typecheck_options.dslx_stdlib_path = root / "stdlib";
  options.seed = int64_t{431969656495450};
  options.vfs_factory = [&]() -> std::unique_ptr<VirtualizableFilesystem> {
    return std::make_unique<FakeFilesystem>(
        absl::flat_hash_map<std::filesystem::path, std::string>{
            {"/test.x", kProgram},
            {"/stdlib/std.x", get_stdlib_contents("std.x")},
            {"/stdlib/abs_diff.x", get_stdlib_contents("abs_diff.x")},
            {"/stdlib/lza.x", get_stdlib_contents("lza.x")},
            {"/stdlib/apfloat.x", get_stdlib_contents("apfloat.x")},
            {"/stdlib/bfloat16.x", get_stdlib_contents("bfloat16.x")},
            {"/stdlib/float32.x", get_stdlib_contents("float32.x")},
        },
        /*cwd=*/root);
  };
  options.convert_options.convert_tests = true;
  // Run the quickcheck and inspect that there's a u16 reported in the output.
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  // Look at the failure message to make sure the u16 is reported.
  std::vector<std::string> failures = result.GetFailureMessages();
  ASSERT_EQ(failures.size(), 1);
  EXPECT_THAT(failures[0], HasSubstr("tests: [u16:"));
}

// An exhaustive quickcheck that fails just for one value in a decently large
// space.
TEST_P(RunRoutinesTest, QuickcheckExhaustiveFail) {
  constexpr std::string_view kProgram = R"(
#[quickcheck(exhaustive)]
fn trivial(x: u11) -> bool { x != u11::MAX }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, /*test_cases=*/1,
                                   /*ran_count=*/0, /*failed_count=*/1));
}

// An exhaustive quickcheck that fails just for one value in a decently large
// space using a two tuple of params.
TEST_P(RunRoutinesTest, QuickcheckExhaustive2ParamFail) {
  constexpr std::string_view kProgram = R"(
#[quickcheck(exhaustive)]
fn trivial(x: u5, y: u6) -> bool { !(x == u5::MAX && y == u6::MAX) }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, /*test_cases=*/1,
                                   /*ran_count=*/0, /*failed_count=*/1));
}

TEST_P(RunRoutinesTest, NoSeedStillQuickChecks) {
  constexpr const char* kProgram = R"(
fn id(x: bool) -> bool { x }

#[quickcheck(test_count=1024)]
fn trivial(x: u5) -> bool { id(true) }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));

  ASSERT_EQ(jit_comparator.jit_cache_.size(), 1);
  EXPECT_EQ(jit_comparator.jit_cache_.begin()->first, "__test__trivial");
}

TEST_P(RunRoutinesTest, FallibleFunctionQuickChecks) {
  constexpr const char* kProgram = R"(
fn do_fail(x: bool) -> bool { fail!("oh_no", x) }

#[quickcheck]
fn qc(x: bool) -> bool { do_fail(x) }
)";
  constexpr const char* kModuleName = "test";
  constexpr const char* kFilename = "test.x";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.seed = int64_t{2316476071057580};
  options.quickcheck_runner = &jit_comparator;
  options.vfs_factory = [kProgram] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, kFilename, options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

TEST_P(RunRoutinesTest, FailingQuickCheck) {
  constexpr const char* kProgram = R"(
#[quickcheck(test_count=2)]
fn trivial(x: u5) -> bool { false }
)";
  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  RunComparator jit_comparator(CompareMode::kJit);
  ParseAndTestOptions options;
  options.quickcheck_runner = &jit_comparator;
  options.seed = int64_t{42};
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

TEST_P(RunRoutinesTest, TwoNonParametricProcs) {
  ParseAndTestOptions options;
  // TODO: https://github.com/google/xls/issues/2078 - Delete this test case
  // once proc-scoped channels are turned on everwhere.
  if (options.convert_options.lower_to_proc_scoped_channels ||
      GetParam() == RunnerType::kIrInterpreterProcScoped ||
      GetParam() == RunnerType::kIrJitProcScoped) {
    GTEST_SKIP() << "Skipping test for proc-scoped channels";
  }
  constexpr std::string_view kProgram = R"(
proc FirstProc {
    data_r: chan<u32> in;
    data_s: chan<u32> out;

    init { () }

    config(data_r: chan<u32> in, data_s: chan<u32> out) { (data_r, data_s) }

    next( state: ()) {
        let (tok, data) = recv(join(), data_r);
        let tok = send(tok, data_s, data);
    }
}

proc MyOtherProc {
    data_r: chan<u32> in;
    data_s: chan<u32> out;

    init {()}

    config(data_r: chan<u32> in, data_s: chan<u32> out) {
        (data_r, data_s)
    }

    next( state: ()) {
        let (tok, data) = recv(join(), data_r);
        let tok = send(tok, data_s, data);
    }
})";

  constexpr const char* kModuleName = "test";
  RunComparator jit_comparator(CompareMode::kJit);
  options.run_comparator = &jit_comparator;

  EXPECT_THAT(ParseAndTest(kProgram, kModuleName, "test_module.x", options),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("Consider turning off comparison")));
}

TEST_P(RunRoutinesTest, FailingProc) {
  constexpr std::string_view kProgram = R"(
#[test_proc]
proc doomed {
    terminator: chan<bool> out;

    config(terminator: chan<bool> out) {
        (terminator,)
    }

    next(state: ()) {
      let tok = send(join(), terminator, false);
    }
})";

  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));
  EXPECT_THAT(result,
              IsTestResult(TestResult::kParseOrTypecheckError, 0, 0, 0));
}

TEST_P(RunRoutinesTest, TestProcExpectedToFailOnAssert) {
  if (GetParam() != RunnerType::kDslxInterpreter) {
    GTEST_SKIP() << "expected_fail_label attribute is only supported on dslx "
                    "interpreter for test_procs";
  }

  constexpr std::string_view kProgram = R"(
#[test_proc(expected_fail_label="my_fail")]
proc tester {
    terminator: chan<bool> out;
    config(terminator: chan<bool> out) {
        (terminator,)
    }
    init {}
    next(_: ()) {
        assert!(false, "my_fail");
        send(join(), terminator, true);
    }
})";

  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(RunRoutinesTest, TestProcFailingOnDifferentAssertLabelThanExpected) {
  if (GetParam() != RunnerType::kDslxInterpreter) {
    GTEST_SKIP() << "expected_fail_label attribute is only supported on dslx "
                    "interpreter for test_procs";
  }

  constexpr std::string_view kProgram = R"(
#[test_proc(expected_fail_label="my_fail")]
proc tester {
    terminator: chan<bool> out;
    config(terminator: chan<bool> out) {
        (terminator,)
    }
    init {}
    next(_: ()) {
        assert!(false, "unexpected_fail");
        send(join(), terminator, true);
    }
})";

  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));

  std::vector<std::string> failures = result.GetFailureMessages();
  EXPECT_THAT(
      failures[0],
      HasSubstr("The program being interpreted failed! Proc failed on "
                "'unexpected_fail', but expected to fail on 'my_fail'"));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

TEST_P(RunRoutinesTest, TestProcExpectedToFailOnFail) {
  if (GetParam() != RunnerType::kDslxInterpreter) {
    GTEST_SKIP() << "expected_fail_label attribute is only supported on dslx "
                    "interpreter for test_procs";
  }

  constexpr std::string_view kProgram = R"(
#[test_proc(expected_fail_label="my_fail")]
proc tester {
    terminator: chan<bool> out;
    config(terminator: chan<bool> out) {
        (terminator,)
    }
    init {}
    next(_: ()) {
        fail!("my_fail", ());
        send(join(), terminator, true);
    }
})";

  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(RunRoutinesTest, TestProcFailingOnDifferentFailLabelThanExpected) {
  if (GetParam() != RunnerType::kDslxInterpreter) {
    GTEST_SKIP() << "expected_fail_label attribute is only supported on dslx "
                    "interpreter for test_procs";
  }

  constexpr std::string_view kProgram = R"(
#[test_proc(expected_fail_label="my_fail")]
proc tester {
    terminator: chan<bool> out;
    config(terminator: chan<bool> out) {
        (terminator,)
    }
    init {}
    next(_: ()) {
        fail!("unexpected_fail", ());
        send(join(), terminator, true);
    }
})";

  XLS_ASSERT_OK_AND_ASSIGN(auto temp_file,
                           TempFile::CreateWithContent(kProgram, "_test.x"));
  constexpr const char* kModuleName = "test";
  ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, kModuleName, std::string(temp_file.path()),
                   options));

  std::vector<std::string> failures = result.GetFailureMessages();
  EXPECT_THAT(
      failures[0],
      HasSubstr("The program being interpreted failed! Proc failed on "
                "'unexpected_fail', but expected to fail on 'my_fail'"));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

// Verifies: recursive token leaves preserve complete prior argument values.
// Catches: bit-generated tokens or callbacks seeing partial aggregates.
TEST(QuickcheckTest, ValueGeneratorRejectsSumWidthBeforeGeneratingPayload) {
  constexpr std::string_view kProgram = R"(
enum Generated: u1 { Active(uN[0], u1[2][3]) = 0 }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(SumDef * sum_def,
                           tm.module->GetMemberOrError<SumDef>("Generated"));
  auto make_sum = [&](uint32_t inner_size, uint32_t outer_size) {
    std::vector<std::unique_ptr<Type>> payload;
    payload.push_back(std::make_unique<BitsType>(false, 0));
    payload.push_back(std::make_unique<ArrayType>(
        std::make_unique<ArrayType>(std::make_unique<BitsType>(false, 1),
                                    TypeDim::CreateU32(inner_size)),
        TypeDim::CreateU32(outer_size)));
    std::vector<SumTypeVariant> variants;
    variants.push_back(SumTypeVariant::MakeTuple(*sum_def->variants().front(),
                                                 std::move(payload)));
    return SumType(*sum_def, std::move(variants), TypeDim::CreateU32(1));
  };
  SumType overflow = make_sum(65535, 65537);
  XLS_ASSERT_OK_AND_ASSIGN(TypeDim payload_width,
                           overflow.GetMaxPayloadBitCount());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t payload_bits, payload_width.GetAsInt64());
  ASSERT_EQ(payload_bits, 4294967295);

  int leaf_calls = 0;
  auto stop_before_array =
      [&](absl::BitGenRef, const BitsLikeProperties&,
          absl::Span<const InterpValue>) -> absl::StatusOr<InterpValue> {
    ++leaf_calls;
    return absl::AbortedError("reached the active payload");
  };
  std::mt19937_64 bit_gen{2};
  InterpValueGenerator generator;
  // The first leaf has no bits and safely stops the old traversal before it
  // can enter the enormous second member. Its callback must not run at all.
  EXPECT_THAT(generator.Generate(bit_gen, overflow, {}, stop_before_array),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("shared sum bit count exceeds")));
  EXPECT_EQ(leaf_calls, 0);

  SumType ordinary = make_sum(2, 3);
  EXPECT_THAT(generator.Generate(bit_gen, ordinary, {}, stop_before_array),
              StatusIs(absl::StatusCode::kAborted,
                       HasSubstr("reached the active payload")));
  EXPECT_EQ(leaf_calls, 1);
  XLS_ASSERT_OK(generator.Generate(bit_gen, ordinary, {}));
}

TEST(QuickcheckTest, ValueGeneratorsPreservePriorValuesAcrossRecursiveLeaves) {
  std::vector<std::unique_ptr<Type>> tuple_members;
  tuple_members.push_back(BitsType::MakeU8());
  tuple_members.push_back(std::make_unique<TokenType>());
  tuple_members.push_back(BitsType::MakeU8());

  std::vector<std::unique_ptr<Type>> owned_types;
  owned_types.push_back(BitsType::MakeU8());
  owned_types.push_back(std::make_unique<TupleType>(std::move(tuple_members)));
  owned_types.push_back(std::make_unique<TokenType>());
  owned_types.push_back(BitsType::MakeU8());
  const std::vector<const Type*> types = {
      owned_types[0].get(), owned_types[1].get(), owned_types[2].get(),
      owned_types[3].get()};

  for (bool use_compatibility_wrapper : {false, true}) {
    SCOPED_TRACE(use_compatibility_wrapper ? "compatibility wrapper"
                                           : "reusable generator");
    std::mt19937_64 bit_gen{2};
    std::vector<std::vector<InterpValue>> observed_prior;
    auto generate_bits = [&observed_prior](absl::BitGenRef,
                                           const BitsLikeProperties&,
                                           absl::Span<const InterpValue> prior)
        -> absl::StatusOr<InterpValue> {
      observed_prior.emplace_back(prior.begin(), prior.end());
      return InterpValue::MakeUBits(
          8, static_cast<int64_t>(observed_prior.size()));
    };

    InterpValueGenerator generator;
    absl::StatusOr<std::vector<InterpValue>> generated =
        use_compatibility_wrapper
            ? GenerateInterpValues(bit_gen, types, generate_bits)
            : generator.GenerateValues(bit_gen, types, generate_bits);
    XLS_ASSERT_OK_AND_ASSIGN(std::vector<InterpValue> values,
                             std::move(generated));
    ASSERT_EQ(values.size(), 4);
    EXPECT_TRUE(values[2].IsToken());
    ASSERT_EQ(values[1].GetValuesOrDie().size(), 3);
    EXPECT_TRUE(values[1].GetValuesOrDie().at(1).IsToken());
    EXPECT_THAT(
        observed_prior,
        ::testing::ElementsAre(
            std::vector<InterpValue>{}, std::vector<InterpValue>{values[0]},
            std::vector<InterpValue>{values[0]},
            std::vector<InterpValue>{values[0], values[1], values[2]}));
  }
}

TEST(QuickcheckTest, JitNestedConditionalTraceSectionsEmitOneEvent) {
  Package package("conditional_trace");
  constexpr std::string_view kIr = R"(
fn conditional_trace(tkn: token, cond: bits[1], outer: bits[1], inner: bits[1], value: bits[8]) -> token {
  ret trace.1: token = trace(tkn, cond, format="prefix{?} outer{?} {}{/}{/} suffix", data_operands=[outer, inner, value])
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(kIr, &package));
  RunComparator jit_comparator(CompareMode::kJit);
  auto run = [&](bool enabled, bool outer, bool inner) {
    const std::vector<Value> arguments = {
        Value::Token(), Value(UBits(enabled, 1)), Value(UBits(outer, 1)),
        Value(UBits(inner, 1)), Value(UBits(42, 8))};
    return jit_comparator.RunIrFunction("conditional_trace", function,
                                        arguments);
  };

  XLS_ASSERT_OK_AND_ASSIGN(auto all_active, run(true, true, true));
  EXPECT_THAT(all_active.events.GetTraceMessageStrings(),
              ::testing::ElementsAre("prefix outer 42 suffix"));

  XLS_ASSERT_OK_AND_ASSIGN(auto inner_inactive, run(true, true, false));
  EXPECT_THAT(inner_inactive.events.GetTraceMessageStrings(),
              ::testing::ElementsAre("prefix outer suffix"));

  XLS_ASSERT_OK_AND_ASSIGN(auto outer_inactive, run(true, false, true));
  EXPECT_THAT(outer_inactive.events.GetTraceMessageStrings(),
              ::testing::ElementsAre("prefix suffix"));

  XLS_ASSERT_OK_AND_ASSIGN(auto disabled, run(false, true, true));
  EXPECT_TRUE(disabled.events.GetTraceMessageStrings().empty());
}

// Verifies that the QuickCheck mechanism can find counter-examples for a simple
// erroneous function.
TEST(QuickcheckTest, QuickCheckBits) {
  Package package("bad_bits_property");
  std::string ir_text = R"(
  fn adjacent_bits(x: bits[2]) -> bits[1] {
    first_bit: bits[1] = bit_slice(x, start=0, width=1)
    second_bit: bits[1] = bit_slice(x, start=1, width=1)
    ret eq_value: bits[1] = eq(first_bit, second_bit)
  }
  )";
  int64_t seed = 0;
  QuickCheckTestCases test_cases = QuickCheckTestCases::Counted(1000);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(ir_text, &package));
  RunComparator jit_comparator(CompareMode::kJit);

  std::vector<std::unique_ptr<dslx::Type>> params;
  params.push_back(std::make_unique<dslx::BitsType>(false, 2));
  auto return_type = std::make_unique<dslx::BitsType>(false, 1);
  dslx::FunctionType fn_type(std::move(params), std::move(return_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));
  std::vector<Value> results = quickcheck_info.results;
  // If a counter-example was found, the last result will be 0.
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

TEST(QuickcheckTest, ExhaustiveCounterexampleWithImplicitToken) {
  Package package("implicit_token");
  constexpr std::string_view kIr = R"(
fn qc(tkn: token, activated: bits[1], x: bits[2]) -> (token, bits[1]) {
  all_ones: bits[2] = literal(value=3)
  holds: bits[1] = ne(x, all_ones)
  ret result: (token, bits[1]) = tuple(tkn, holds)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(kIr, &package));
  CountingRunComparator jit_comparator(CompareMode::kJit);
  std::vector<std::unique_ptr<dslx::Type>> params;
  params.push_back(std::make_unique<dslx::BitsType>(true, 2));
  dslx::FunctionType fn_type(std::move(params),
                             std::make_unique<dslx::BitsType>(false, 1));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/true, &fn_type, function,
                   kFakeIrName, &jit_comparator, /*seed=*/0,
                   QuickCheckTestCases::Exhaustive()));
  ASSERT_TRUE(quickcheck_info.falsifying_dslx_arg_set.has_value());
  EXPECT_THAT(*quickcheck_info.falsifying_dslx_arg_set,
              ::testing::ElementsAre(InterpValue::MakeSBits(2, -1)));
  EXPECT_EQ(jit_comparator.invocation_count(), 4);
  for (const std::vector<Value>& arguments : jit_comparator.invocation_args()) {
    ASSERT_EQ(arguments.size(), 3);
    EXPECT_TRUE(arguments.at(0).IsToken());
    EXPECT_EQ(arguments.at(1), Value::Bool(true));
  }
}

TEST(QuickcheckTest, ExhaustiveNoArgumentCounterexample) {
  Package package("no_arguments");
  constexpr std::string_view kIr = R"(
fn qc() -> bits[1] {
  ret result: bits[1] = literal(value=0)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(kIr, &package));
  RunComparator jit_comparator(CompareMode::kJit);
  dslx::FunctionType fn_type({}, std::make_unique<dslx::BitsType>(false, 1));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, /*seed=*/0,
                   QuickCheckTestCases::Exhaustive()));
  EXPECT_THAT(quickcheck_info.results,
              ::testing::ElementsAre(Value::Bool(false)));
  ASSERT_TRUE(quickcheck_info.falsifying_dslx_arg_set.has_value());
  EXPECT_THAT(*quickcheck_info.falsifying_dslx_arg_set, ::testing::IsEmpty());
}

TEST(QuickcheckTest, QuickCheckArray) {
  Package package("bad_array_property");
  std::string ir_text = R"(
  fn adjacent_elements(x: bits[8][5]) -> bits[1] {
    zero: bits[32] = literal(value=0)
    one: bits[32] = literal(value=1)
    first_element: bits[8] = array_index(x, indices=[zero])
    second_element: bits[8] = array_index(x, indices=[one])
    ret eq_value: bits[1] = eq(first_element, second_element)
  }
  )";
  int64_t seed = 0;
  QuickCheckTestCases test_cases = QuickCheckTestCases::Counted(1000);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(ir_text, &package));
  RunComparator jit_comparator(CompareMode::kJit);

  std::vector<std::unique_ptr<dslx::Type>> params;
  params.push_back(std::make_unique<dslx::ArrayType>(
      std::make_unique<dslx::BitsType>(false, 8), TypeDim::CreateU32(5)));
  auto return_type = std::make_unique<dslx::BitsType>(false, 1);
  dslx::FunctionType fn_type(std::move(params), std::move(return_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));
  std::vector<Value> results = quickcheck_info.results;
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

TEST(QuickcheckTest, QuickCheckTuple) {
  Package package("bad_tuple_property");
  std::string ir_text = R"(
  fn adjacent_elements(x: (bits[8], bits[8])) -> bits[1] {
    first_member: bits[8] = tuple_index(x, index=0)
    second_member: bits[8] = tuple_index(x, index=1)
    ret eq_value: bits[1] = eq(first_member, second_member)
  }
  )";
  int64_t seed = 0;
  QuickCheckTestCases test_cases = QuickCheckTestCases::Counted(1000);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(ir_text, &package));
  RunComparator jit_comparator(CompareMode::kJit);

  std::vector<std::unique_ptr<dslx::Type>> tuple_elems;
  tuple_elems.push_back(std::make_unique<dslx::BitsType>(false, 8));
  tuple_elems.push_back(std::make_unique<dslx::BitsType>(false, 8));

  std::vector<std::unique_ptr<dslx::Type>> params;
  params.push_back(std::make_unique<dslx::TupleType>(std::move(tuple_elems)));
  auto return_type = std::make_unique<dslx::BitsType>(false, 1);
  dslx::FunctionType fn_type(std::move(params), std::move(return_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));
  std::vector<Value> results = quickcheck_info.results;
  EXPECT_EQ(results.back(), Value(UBits(0, 1)));
}

// If the QuickCheck mechanism can't find a falsifying example, we expect
// the argsets and results vectors to have lengths of 'num_tests'.
TEST(QuickcheckTest, NumTests) {
  Package package("always_true");
  std::string ir_text = R"(
  fn ret_true(x: bits[32]) -> bits[1] {
    ret eq_value: bits[1] = eq(x, x)
  }
  )";
  int64_t seed = 0;
  QuickCheckTestCases test_cases = QuickCheckTestCases::Counted(5050);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(ir_text, &package));
  RunComparator jit_comparator(CompareMode::kJit);

  std::vector<std::unique_ptr<dslx::Type>> params;
  params.push_back(std::make_unique<dslx::BitsType>(false, 32));
  auto return_type = std::make_unique<dslx::BitsType>(false, 1);
  dslx::FunctionType fn_type(std::move(params), std::move(return_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));

  std::vector<std::vector<Value>> argsets = quickcheck_info.arg_sets;
  std::vector<Value> results = quickcheck_info.results;
  EXPECT_EQ(argsets.size(), 5050);
  EXPECT_EQ(results.size(), 5050);
}

// Given a constant seed, we expect the same argsets and results vectors from
// two runs through the QuickCheck mechanism.
TEST(QuickcheckTest, Seeding) {
  Package package("sometimes_false");
  std::string ir_text = R"(
  fn gt_one(x: bits[8]) -> bits[1] {
    literal.2: bits[8] = literal(value=1)
    ret ugt.3: bits[1] = ugt(x, literal.2)
  }
  )";
  int64_t seed = 12345;
  QuickCheckTestCases test_cases = QuickCheckTestCases::Counted(1000);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * function,
                           Parser::ParseFunction(ir_text, &package));
  RunComparator jit_comparator(CompareMode::kJit);
  std::vector<std::unique_ptr<dslx::Type>> params;

  params.push_back(std::make_unique<dslx::BitsType>(false, 8));
  auto return_type = std::make_unique<dslx::BitsType>(false, 1);
  dslx::FunctionType fn_type(std::move(params), std::move(return_type));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info1,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto quickcheck_info2,
      DoQuickCheck(/*requires_implicit_token=*/false, &fn_type, function,
                   kFakeIrName, &jit_comparator, seed, test_cases));

  EXPECT_EQ(quickcheck_info1.arg_sets, quickcheck_info2.arg_sets);
  EXPECT_EQ(quickcheck_info1.falsifying_dslx_arg_set,
            quickcheck_info2.falsifying_dslx_arg_set);
  EXPECT_EQ(quickcheck_info1.results, quickcheck_info2.results);
}

TEST(QuickcheckTest, ProofFailure) {
  constexpr std::string_view kProgram = R"(
#[quickcheck(exhaustive)]
fn quickcheck_that_fails(x: u1) -> bool {
  x != x
}
)";
  ParseAndProveOptions options;
  options.vfs_factory = [&] {
    return std::make_unique<UniformContentFilesystem>(kProgram, "test.x");
  };
  XLS_ASSERT_OK_AND_ASSIGN(auto result,
                           ParseAndProve(kProgram, "test", "test.x", options));
  EXPECT_THAT(result.test_result_data,
              IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
  EXPECT_THAT(result.counterexamples.size(), 1);
  EXPECT_THAT(result.counterexamples.begin()->second,
              testing::ElementsAre(Value(UBits(0, 1))));
}

TEST_P(ParseAndTestTest, DeadlockedProc) {
  // Test proc never sends to the subproc, so network is deadlocked.
  constexpr std::string_view kProgram = R"(
proc incrementer {
  in_ch: chan<u32> in;
  out_ch: chan<u32> out;

  init { () }

  config(in_ch: chan<u32> in,
         out_ch: chan<u32> out) {
    (in_ch, out_ch)
  }

  next(_: ()) {
    let (tok, i) = recv(join(), in_ch);
    let tok = send(tok, out_ch, i + u32:1);
  }
}

#[test_proc]
proc tester_proc {
  data_out: chan<u32> out;
  data_in: chan<u32> in;
  terminator: chan<bool> out;

  init { () }

  config(terminator: chan<bool> out) {
    let (input_out, input_in) = chan<u32>("input");
    let (output_out, output_in) = chan<u32>("output");
    spawn incrementer(input_in, output_out);
    (input_out, output_in, terminator)
  }

  next(state: ()) {
    let tok = send_if(join(), data_out, false, u32:42);
    let (tok, _result) = recv(tok, data_in);
    let tok = send(tok, terminator, u1:1);
 }
})";
  ParseAndTestOptions options;
  options.max_ticks = 100;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test_module", "test.x", options));

  std::vector<std::string> failures = result.GetFailureMessages();
  EXPECT_EQ(failures.size(), 1);
  if (GetParam() == RunnerType::kDslxInterpreter) {
    EXPECT_THAT(failures[0],
                AllOf(HasSubstr("proc `incrementer` is blocked on receive on "
                                "channel `tester_proc->incrementer#0::in_ch`"),
                      HasSubstr("proc `tester_proc` is blocked on receive on "
                                "channel `tester_proc::data_in`")));
  } else {
    EXPECT_THAT(failures[0], HasSubstr("deadlocked"));
  }
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

TEST_P(ParseAndTestTest, TooManyTicks) {
  // Test proc never receives and spins forever.
  constexpr std::string_view kProgram = R"(
proc incrementer {
  in_ch: chan<u32> in;
  out_ch: chan<u32> out;

  init { () }

  config(in_ch: chan<u32> in,
         out_ch: chan<u32> out) {
    (in_ch, out_ch)
  }
  next(_: ()) {
    let (tok, i) = recv_if(join(), in_ch, false, u32:0);
    let tok = send_if(tok, out_ch, false, i + u32:1);
  }
}

#[test_proc]
proc tester_proc {
  data_out: chan<u32> out;
  data_in: chan<u32> in;
  terminator: chan<bool> out;

  init { () }

  config(terminator: chan<bool> out) {
    let (input_out, input_in) = chan<u32>("input");
    let (output_out, output_in) = chan<u32>("output");
    spawn incrementer(input_in, output_out);
    (input_out, output_in, terminator)
  }

  next(state: ()) {
    let tok = send(join(), data_out, u32:42);
    let (tok, _result) = recv(tok, data_in);
    let tok = send(tok, terminator, true);
 }
})";
  ParseAndTestOptions options;
  options.max_ticks = 100;
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test_module", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kSomeFailed, 1, 0, 1));
}

inline constexpr std::string_view kTwoTests = R"(
#[test] fn test_one() {}
#[test] fn test_two() {}
)";

TEST_P(ParseAndTestTest, TestFilterEmpty) {
  const ParseAndTestOptions options;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kTwoTests, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 2, 0, 0));
}

TEST_P(ParseAndTestTest, TestFilterSelectNone) {
  const RE2 test_filter("doesnotexist");
  ParseAndTestOptions options;
  options.test_filter = &test_filter;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kTwoTests, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 2, 2, 0));
}

TEST_P(ParseAndTestTest, TestFilterSelectOne) {
  const RE2 test_filter(".*_one");
  ParseAndTestOptions options;
  options.test_filter = &test_filter;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kTwoTests, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 2, 1, 0));
}

TEST_P(ParseAndTestTest, TestFilterSelectBoth) {
  const RE2 test_filter("test_.*");
  ParseAndTestOptions options;
  options.test_filter = &test_filter;
  XLS_ASSERT_OK_AND_ASSIGN(TestResultData result,
                           ParseAndTest(kTwoTests, "test", "test.x", options));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 2, 0, 0));
}

// Exercises https://github.com/google/xls/issues/1368
TEST_P(ParseAndTestTest, StructParametricFromProcParametric) {
  constexpr std::string_view kProgram = R"(
struct Data<WIDTH: u32> {struct_field_value: uN[WIDTH]}

proc MyProc<DATA_WIDTH: u32> {
  type MyProcData = Data<DATA_WIDTH>;
  out_s: chan<MyProcData> out;

  config(out_s: chan<MyProcData> out) {(out_s, )}
  init {}

 next(_: ()) {
    // This line was failing in typecheck previously
    send(join(), out_s, MyProcData{struct_field_value: uN[DATA_WIDTH]:42});
  }
}

const TEST_WIDTH = u32:10;

#[test_proc]
proc MyProcTest {
  type MyProcTestData = Data<TEST_WIDTH>;
  terminator: chan<bool> out;
  out_r: chan<MyProcTestData> in;

  config(terminator: chan<bool> out) {
    let(out_s, out_r) = chan<MyProcTestData>("out");
    spawn MyProc<TEST_WIDTH>(out_s);
    (terminator, out_r)
  }

  init {}

  next(_: ()) {
    let(tok, returned_struct) = recv(join(), out_r);
    assert_eq(returned_struct.struct_field_value, uN[TEST_WIDTH]:42);
    send(tok, terminator, true);
  }
})";
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions{}));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(ParseAndTestTest, StructParametricFromFnParametric) {
  constexpr std::string_view kProgram = R"(
struct Data<WIDTH: u32> {value: uN[WIDTH]}

fn myFn<DATA_WIDTH: u32>() -> Data<DATA_WIDTH> {
  type MyFnData = Data<DATA_WIDTH>;
  let data = MyFnData{value: uN[DATA_WIDTH]:42};
  data
}

const TEST_WIDTH = u32:32;

#[test]
fn test_simple() {
  let output = myFn<TEST_WIDTH>();
  assert_eq(output.value, uN[TEST_WIDTH]:42);
  }
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions{}));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(ParseAndTestTest, NewStyleTestProc) {
  if (GetParam() == RunnerType::kIrInterpreter ||
      GetParam() == RunnerType::kIrJit) {
    GTEST_SKIP()
        << "New-style proc tests only supported with proc-scoped channels";
  }
  constexpr std::string_view kProgram = R"(
#[test]
proc MyNewTestProc {
  __test__terminator: chan<bool> out,
}

impl MyNewTestProc {
  fn new(terminator: chan<bool> out) -> Self {
    MyNewTestProc { __test__terminator: terminator }
  }
  fn next(self) {
    send(join(), self.__test__terminator, true);
  }
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions{}));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(ParseAndTestTest, NewStyleTestProcMissingNew) {
  if (GetParam() != RunnerType::kIrInterpreterProcScoped &&
      GetParam() != RunnerType::kIrJitProcScoped) {
    GTEST_SKIP()
        << "Only testing MakeRunner failure modes on proc-scoped IR runners";
  }
  constexpr std::string_view kProgram = R"(
#[test]
proc MyNewTestProc {
  __test__terminator: chan<bool> out,
}

impl MyNewTestProc {
  // Constructor is named 'create', not 'new'.
  fn create(terminator: chan<bool> out) -> Self {
    MyNewTestProc { __test__terminator: terminator }
  }
  fn next(self) {
    send(join(), self.__test__terminator, true);
  }
}
)";
  EXPECT_THAT(ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions()),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr("Could not find 'new' method in proc")));
}

TEST_P(ParseAndTestTest, NewStyleTestProcNoParams) {
  if (GetParam() != RunnerType::kIrInterpreterProcScoped &&
      GetParam() != RunnerType::kIrJitProcScoped) {
    GTEST_SKIP()
        << "Only testing MakeRunner failure modes on proc-scoped IR runners";
  }
  constexpr std::string_view kProgram0 = R"(
#[test]
proc MyNewTestProc {}

impl MyNewTestProc {
  fn new() -> Self {
    MyNewTestProc {}
  }
  fn next(self) {}
}
)";
  EXPECT_THAT(
      ParseAndTest(kProgram0, "test", "test.x", ParseAndTestOptions()),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("expected 1")));
}

TEST_P(ParseAndTestTest, NewStyleTestProcTooManyParams) {
  if (GetParam() != RunnerType::kIrInterpreterProcScoped &&
      GetParam() != RunnerType::kIrJitProcScoped) {
    GTEST_SKIP()
        << "Only testing MakeRunner failure modes on proc-scoped IR runners";
  }
  constexpr std::string_view kProgram = R"(
#[test]
proc MyNewTestProc {
  __test__terminator: chan<bool> out,
  other: chan<u32> in,
}

impl MyNewTestProc {
  fn new(terminator: chan<bool> out, other: chan<u32> in) -> Self {
    MyNewTestProc { __test__terminator: terminator, other: other }
  }
  fn next(self) {
    send(join(), self.__test__terminator, true);
  }
}
)";
  EXPECT_THAT(
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions()),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("expected 1")));
}

TEST_P(ParseAndTestTest, NewStyleTestProcNonProcScoped) {
  if (GetParam() != RunnerType::kIrInterpreter &&
      GetParam() != RunnerType::kIrJit) {
    GTEST_SKIP() << "Only testing non-proc-scoped failure mode on "
                    "non-proc-scoped IR runners";
  }
  constexpr std::string_view kProgram = R"(
#[test]
proc MyNewTestProc {
  __test__terminator: chan<bool> out,
}

impl MyNewTestProc {
  fn new(terminator: chan<bool> out) -> Self {
    MyNewTestProc { __test__terminator: terminator }
  }
  fn next(self) {
    send(join(), self.__test__terminator, true);
  }
}
)";
  ParseAndTestOptions options;
  options.convert_options.lower_to_proc_scoped_channels = false;
  EXPECT_THAT(ParseAndTest(kProgram, "test", "test.x", options),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Impl-style procs can only be compiled with "
                                 "proc-scoped channels")));
}

TEST_P(ParseAndTestTest, NewStyleTestProcWithSpawn) {
  if (GetParam() == RunnerType::kIrInterpreter ||
      GetParam() == RunnerType::kIrJit) {
    GTEST_SKIP()
        << "New-style proc tests only supported with proc-scoped channels";
  }
  constexpr std::string_view kProgram = R"(
proc Loopback {
  c_in: chan<u32> in,
  c_out: chan<u32> out,
}

impl Loopback {
  fn new(c_in: chan<u32> in, c_out: chan<u32> out) -> Self {
    Loopback { c_in, c_out }
  }

  fn next(self) {
    let (t, val) = recv(join(), self.c_in);
    send(t, self.c_out, val);
  }
}

#[test]
proc Main {
  __test__terminator: chan<bool> out,
  c_in_from_loopback: chan<u32> in,
  c_out_to_loopback: chan<u32> out,
}

impl Main {
  fn new(terminator: chan<bool> out) -> Self {
    let (out_to_loopback, loopback_in) = chan<u32>("main_to_loopback");
    let (loopback_out, in_from_loopback) = chan<u32>("loopback_to_main");
    Loopback::new(loopback_in, loopback_out).spawn();

    Main {
      __test__terminator: terminator,
      c_in_from_loopback: in_from_loopback,
      c_out_to_loopback: out_to_loopback,
    }
  }

  fn next(self) {
    let tok = send(join(), self.c_out_to_loopback, u32:42);
    let (tok, loopback_val) = recv(tok, self.c_in_from_loopback);
    assert_eq(loopback_val, u32:42);
    send(tok, self.__test__terminator, true);
  }
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions{}));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

TEST_P(ParseAndTestTest, TransformedTestFunctionRunsAndSucceeds) {
  if (GetParam() == RunnerType::kIrInterpreter ||
      GetParam() == RunnerType::kIrJit) {
    GTEST_SKIP()
        << "New-style proc tests only supported with proc-scoped channels";
  }
  constexpr std::string_view kProgram = R"(
proc PassThrough {
  c_in: chan<u32> in,
  c_out: chan<u32> out,
}

impl PassThrough {
  fn new(c_in: chan<u32> in, c_out: chan<u32> out) -> Self {
    PassThrough { c_in, c_out }
  }
  fn next(self) {
    let (tok, val) = recv(join(), self.c_in);
    send(tok, self.c_out, val);
  }
}

#[test]
fn test_pass_through() {
  let (in_w, in_r) = chan<u32>("in");
  let (out_w, out_r) = chan<u32>("out");
  PassThrough::new(in_r, out_w).spawn();
  let tok = send(join(), in_w, u32:42);
  let (tok, val) = recv(tok, out_r);
  assert_eq(val, u32:42)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(
      TestResultData result,
      ParseAndTest(kProgram, "test", "test.x", ParseAndTestOptions{}));
  EXPECT_THAT(result, IsTestResult(TestResult::kAllPassed, 1, 0, 0));
}

INSTANTIATE_TEST_SUITE_P(RunRoutinesTest, RunRoutinesTest,
                         testing::Values(RunnerType::kDslxInterpreter,
                                         RunnerType::kIrInterpreter,
                                         RunnerType::kIrJit,
                                         RunnerType::kIrInterpreterProcScoped,
                                         RunnerType::kIrJitProcScoped),
                         testing::PrintToStringParamName());
INSTANTIATE_TEST_SUITE_P(ParseAndTestTest, ParseAndTestTest,
                         testing::Values(RunnerType::kDslxInterpreter,
                                         RunnerType::kIrInterpreter,
                                         RunnerType::kIrJit,
                                         RunnerType::kIrInterpreterProcScoped,
                                         RunnerType::kIrJitProcScoped),
                         testing::PrintToStringParamName());

}  // namespace xls::dslx
