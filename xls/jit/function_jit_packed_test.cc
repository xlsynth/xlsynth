// Copyright 2026 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <array>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/interpreter/evaluator_options.h"
#include "xls/ir/bits.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/ir/value_view.h"
#include "xls/jit/function_jit.h"
#include "xls/jit/jit_evaluator_options.h"

namespace xls {
namespace {

class FunctionJitPackedTest : public testing::TestWithParam<int64_t> {
 protected:
  template <typename InputView>
  void ExpectZeroWidthEquality(const Value& zero) {
    Package package("packed_zero_width");
    FunctionBuilder fb("equals_zero", &package);
    BValue input = fb.Param("input", package.GetTypeForValue(zero));
    XLS_ASSERT_OK_AND_ASSIGN(
        Function * function,
        fb.BuildWithReturnValue(fb.Eq(input, fb.Literal(zero))));
    XLS_ASSERT_OK_AND_ASSIGN(
        auto jit,
        FunctionJit::Create(function, EvaluatorOptions(),
                            JitEvaluatorOptions().set_opt_level(GetParam())));
    const std::vector<Value> args = {zero};
    XLS_ASSERT_OK_AND_ASSIGN(auto ordinary, jit->Run(args));
    EXPECT_EQ(ordinary.value, Value(UBits(1, 1)));

    // Repeated calls exercise native stack storage that used to be left
    // uninitialized for the zero-width subtree. The nonzero control also
    // ensures initializing that subtree does not erase the packed payload.
    for (int iteration = 0; iteration < 32; ++iteration) {
      SCOPED_TRACE(iteration);
      for (uint8_t payload : {0, 1}) {
        SCOPED_TRACE(static_cast<int>(payload));
        std::array<uint8_t, (InputView::kBitCount + 7) / 8> input_data = {};
        input_data[0] = payload;
        uint8_t output_data = 0;
        InputView packed_input(input_data.data(), 0);
        PackedBitsView<1> output(&output_data, 0);
        XLS_ASSERT_OK(jit->RunWithPackedViews(packed_input, output));
        EXPECT_EQ(output_data, payload == 0 ? 1 : 0);
      }
    }
  }
};

TEST_P(FunctionJitPackedTest, ZeroWidthTag) {
  // A DSLX singleton sum with a u4 payload has this tag/payload layout. The tag
  // occupies no packed bits but has native storage that equality reads.
  using InputView =
      PackedTupleView<PackedBitsView<0>, PackedTupleView<PackedBitsView<4>>>;
  ExpectZeroWidthEquality<InputView>(
      Value::Tuple({Value(UBits(0, 0)), Value::Tuple({Value(UBits(0, 4))})}));
}

TEST_P(FunctionJitPackedTest, ByteAlignedZeroWidthTag) {
  using InputView =
      PackedTupleView<PackedBitsView<0>, PackedTupleView<PackedBitsView<8>>>;
  ExpectZeroWidthEquality<InputView>(
      Value::Tuple({Value(UBits(0, 0)), Value::Tuple({Value(UBits(0, 8))})}));
}

TEST_P(FunctionJitPackedTest, ZeroWidthTuple) {
  using InputView =
      PackedTupleView<PackedTupleView<PackedBitsView<0>, PackedBitsView<0>>,
                      PackedBitsView<4>>;
  ExpectZeroWidthEquality<InputView>(
      Value::Tuple({Value::Tuple({Value(UBits(0, 0)), Value(UBits(0, 0))}),
                    Value(UBits(0, 4))}));
}

TEST_P(FunctionJitPackedTest, ZeroWidthArray) {
  using InputView =
      PackedTupleView<PackedArrayView<PackedBitsView<0>, 2>, PackedBitsView<4>>;
  ExpectZeroWidthEquality<InputView>(
      Value::Tuple({Value::ArrayOrDie({Value(UBits(0, 0)), Value(UBits(0, 0))}),
                    Value(UBits(0, 4))}));
}

INSTANTIATE_TEST_SUITE_P(OptLevels, FunctionJitPackedTest,
                         testing::Values(0, 3));

}  // namespace
}  // namespace xls
