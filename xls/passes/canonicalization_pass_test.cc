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

#include "xls/passes/canonicalization_pass.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/fuzzing/fuzztest.h"
#include "xls/common/status/matchers.h"
#include "xls/fuzzer/ir_fuzzer/ir_fuzz_domain.h"
#include "xls/fuzzer/ir_fuzzer/ir_fuzz_test_library.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/events.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

class CanonicalizePassTest : public IrTestBase {
 protected:
  absl::StatusOr<bool> Run(Package* p) {
    PassResults results;
    OptimizationContext context;
    return CanonicalizationPass().Run(p, OptimizationPassOptions(), &results,
                                      context);
  }
};

TEST_F(CanonicalizePassTest, Canonicalize) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn simple_canon(x:bits[2]) -> bits[2] {
        one:bits[2] = literal(value=1)
        ret addval: bits[2] = add(one, x)
     }
  )",
                                                       p.get()));
  ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Add(m::Param(), m::Literal()));
}

TEST_F(CanonicalizePassTest, SubToAddNegate) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn simple_neg(x:bits[2]) -> bits[2] {
        one:bits[2] = literal(value=1)
        ret subval: bits[2] = sub(x, one)
     }
  )",
                                                       p.get()));

  ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Add(m::Param(), m::Literal(3)));
}

TEST_F(CanonicalizePassTest, NopZeroExtend) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn nop_zero_ext(x:bits[16]) -> bits[16] {
        ret zero_ext: bits[16] = zero_ext(x, new_bit_count=16)
     }
  )",
                                                       p.get()));

  ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Param());
}

TEST_F(CanonicalizePassTest, ZeroExtendReplacedWithConcat) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn zero_ext(x:bits[33]) -> bits[42] {
        ret zero_ext: bits[42] = zero_ext(x, new_bit_count=42)
     }
  )",
                                                       p.get()));

  ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Concat(m::Literal(0), m::Param()));
}

TEST_F(CanonicalizePassTest, NopBitwiseReductions) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn nop_bitwise_reductions(x: bits[1]) -> (bits[1], bits[1], bits[1]) {
        and_reduce.1: bits[1] = and_reduce(x)
        or_reduce.2: bits[1] = or_reduce(x)
        xor_reduce.3: bits[1] = xor_reduce(x)
        ret tuple: (bits[1], bits[1], bits[1]) = tuple(and_reduce.1, or_reduce.2, xor_reduce.3)
     }
  )",
                                                       p.get()));

  ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Tuple(m::Param(), m::Param(), m::Param()));
}

TEST_F(CanonicalizePassTest, ComparisonWithLiteralCanonicalization) {
  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.ULt(fb.Literal(UBits(42, 32)), fb.Param("x", p->GetBitsType(32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
    EXPECT_THAT(f->return_value(), m::UGt(m::Param(), m::Literal(42)));
  }

  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.ULe(fb.Param("x", p->GetBitsType(32)), fb.Literal(UBits(42, 32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
    EXPECT_THAT(f->return_value(), m::ULt(m::Param(), m::Literal(43)));
  }

  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.UGe(fb.Literal(UBits(42, 32)), fb.Param("x", p->GetBitsType(32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
    EXPECT_THAT(f->return_value(), m::ULt(m::Param(), m::Literal(43)));
  }

  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.UGe(fb.Param("x", p->GetBitsType(32)), fb.Literal(UBits(0, 32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(false));
    EXPECT_THAT(f->return_value(), m::UGe(m::Param(), m::Literal(0)));
  }

  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.SGt(fb.Literal(UBits(42, 32)), fb.Param("x", p->GetBitsType(32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
    EXPECT_THAT(f->return_value(), m::SLt(m::Param(), m::Literal(42)));
  }

  {
    auto p = CreatePackage();
    FunctionBuilder fb(TestName(), p.get());
    fb.Eq(fb.Literal(UBits(42, 32)), fb.Param("x", p->GetBitsType(32)));
    XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
    ASSERT_THAT(Run(p.get()), IsOkAndHolds(true));
    EXPECT_THAT(f->return_value(), m::Eq(m::Param(), m::Literal(42)));
  }
}

TEST_F(CanonicalizePassTest, ExhaustiveClampTest) {
  // Exhaustively test all combinations of:
  //
  //    x  cmp K0 ? x  : K1
  //    x  cmp K0 ? K1 : x
  //    K0 cmp x  ? x  : K1
  //    K0 cmp x  ? K1 : x
  //
  // where:
  //    'cmp' is in { >, >=, <, <= }
  //    K0 and K1 are 3-bit constants
  //    x is a 3-bit parameter
  //
  // Testing consists of trying all values of 'x' with the interpreter and
  // verifying the results match before and after optimizations.
  const int64_t kBitWidth = 3;
  const int64_t kMaxValue = 1 << kBitWidth;
  for (Op cmp_op : {Op::kUGt, Op::kUGe, Op::kULt, Op::kULe}) {
    for (bool x_on_lhs : {true, false}) {
      for (bool x_on_true : {true, false}) {
        for (int64_t k0_value = 0; k0_value < kMaxValue; ++k0_value) {
          std::string k0_str = absl::StrCat(k0_value);
          for (int64_t k1_value = 0; k1_value < kMaxValue; ++k1_value) {
            std::string k1_str = absl::StrCat(k1_value);
            std::string expr_str = absl::StrFormat(
                "%s %s %s ? %s : %s", x_on_lhs ? "x" : k0_str,
                OpToString(cmp_op), x_on_lhs ? k0_str : "x",
                x_on_true ? "x" : k1_str, x_on_true ? k1_str : "x");
            VLOG(1) << "Testing: " << expr_str;

            auto p = CreatePackage();
            FunctionBuilder fb(TestName(), p.get());
            BValue x = fb.Param("x", p->GetBitsType(kBitWidth));
            BValue k0 = fb.Literal(Value(UBits(k0_value, kBitWidth)));
            BValue cmp = x_on_lhs ? fb.AddCompareOp(cmp_op, x, k0)
                                  : fb.AddCompareOp(cmp_op, k0, x);
            BValue k1 = fb.Literal(Value(UBits(k1_value, kBitWidth)));
            BValue select =
                x_on_true ? fb.Select(cmp, x, k1) : fb.Select(cmp, k1, x);
            XLS_ASSERT_OK_AND_ASSIGN(Function * f,
                                     fb.BuildWithReturnValue(select));

            VLOG(2) << "Before canonicalization: " << f->DumpIr();

            std::vector<Value> expected(kMaxValue);
            for (int64_t x_value = 0; x_value < kMaxValue; ++x_value) {
              XLS_ASSERT_OK_AND_ASSIGN(
                  expected[x_value],
                  DropEvaluatorEvents(InterpretFunction(
                      f, {Value(UBits(x_value, kBitWidth))})));
            }

            XLS_ASSERT_OK_AND_ASSIGN(bool changed, Run(p.get()));

            VLOG(2) << "changed: " << changed;
            VLOG(2) << "After canonicalization: " << f->DumpIr();

            for (int64_t x_value = 0; x_value < kMaxValue; ++x_value) {
              XLS_ASSERT_OK_AND_ASSIGN(
                  Value actual, DropEvaluatorEvents(InterpretFunction(
                                    f, {Value(UBits(x_value, kBitWidth))})));
              EXPECT_EQ(expected[x_value], actual)
                  << absl::StreamFormat("%s for x = %d", expr_str, x_value);
            }
          }
        }
      }
    }
  }
}

TEST_F(CanonicalizePassTest, SelectWithTrivialDefault) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn f(p: bits[1], x: bits[8], y: bits[8]) -> bits[8] {
        ret sel.1: bits[8] = sel(p, cases=[x], default=y)
     }
  )",
                                                       p.get()));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::Select(m::Param("p"),
                        /*cases=*/{m::Param("x"), m::Param("y")}));
}

TEST_F(CanonicalizePassTest, SelectWithInvertedSelector) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn f(p: bits[1], x: bits[8], y: bits[8]) -> bits[8] {
        not.1: bits[1] = not(p)
        ret sel.2: bits[8] = sel(not.1, cases=[x, y])
     }
  )",
                                                       p.get()));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::Select(m::Param("p"),
                        /*cases=*/{m::Param("y"), m::Param("x")}));
}

TEST_F(CanonicalizePassTest, SelectWithGiantSelector) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn f(p: bits[128], x: bits[8], y: bits[8]) -> bits[8] {
        ret sel.2: bits[8] = sel(p, cases=[x], default=y)
     }
  )",
                                                       p.get()));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(false));
  EXPECT_THAT(f->return_value(), m::Select(m::Param("p"),
                                           /*cases=*/{m::Param("x")},
                                           /*default_value=*/m::Param("y")));
}

TEST_F(CanonicalizePassTest, NextValueWithAlwaysTruePredicate) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, ParseProc(R"(
     proc test(st: bits[1], init={0}) {
        literal.1: bits[1] = literal(value=1)
        next_value.2: () = next_value(param=st, value=literal.1, predicate=literal.1)
     }
  )",
                                                  p.get()));
  EXPECT_THAT(proc->next_values(proc->GetStateRead(int64_t{0})),
              ElementsAre(m::Next(m::StateRead("st"), m::Literal(1),
                                  /*predicate=*/m::Literal(1))));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->next_values(proc->GetStateRead(int64_t{0})),
              ElementsAre(m::Next(/*state_read=*/m::StateRead("st"),
                                  /*value=*/m::Literal(1))));
}

TEST_F(CanonicalizePassTest, NextValueWithAlwaysFalsePredicate) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, ParseProc(R"(
     proc test(st: bits[1], init={1}) {
        literal.1: bits[1] = literal(value=0)
        next_value.2: () = next_value(param=st, value=literal.1, predicate=literal.1)
     }
  )",
                                                  p.get()));
  EXPECT_THAT(proc->next_values(proc->GetStateRead(int64_t{0})),
              ElementsAre(m::Next(m::StateRead("st"), m::Literal(0),
                                  /*predicate=*/m::Literal(0))));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->next_values(), IsEmpty());
}

TEST_F(CanonicalizePassTest, StateReadWithAlwaysTruePredicate) {
  auto p = CreatePackage();
  ProcBuilder pb("test", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 32)),
                             /*read_predicate=*/pb.Literal(UBits(1, 1)));
  pb.Next(x, pb.Literal(UBits(1, 32)));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build());
  EXPECT_THAT(proc->GetStateRead(int64_t{0})->predicate(),
              Optional(m::Literal(1)));

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateRead(int64_t{0})->predicate(), std::nullopt);
}

void IrFuzzCanonicalization(FuzzPackageWithArgs fuzz_package_with_args) {
  CanonicalizationPass pass;
  OptimizationPassChangesOutputs(std::move(fuzz_package_with_args), pass);
}
FUZZ_TEST(IrFuzzTest, IrFuzzCanonicalization)
    .WithDomains(IrFuzzDomainWithArgs(/*arg_set_count=*/10));

}  // namespace
}  // namespace xls
