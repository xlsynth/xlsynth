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

#include "gtest/gtest.h"

#include "xls/dslx/exhaustiveness/exhaustiveness.h"
#include "absl/log/log.h"

namespace xls::dslx {

TEST(InterpValueRangeTest, ShouldMerge) {
  auto range1 = InterpValueRange(InterpValue::MakeUBits(1, 0), InterpValue::MakeUBits(1, 0));
  auto range2 = InterpValueRange(InterpValue::MakeUBits(1, 1), InterpValue::MakeUBits(1, 1));
  EXPECT_TRUE(range1.ShouldMergeWith(range2));
}

TEST(ExhaustivenessTest, ExhaustiveBooleanRange) {
  BitsLikeProperties properties = BitsLikeProperties{TypeDim::CreateBool(false), TypeDim::CreateU32(1)};
  auto range = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(1, 0), InterpValue::MakeUBits(1, 1));
  EXPECT_TRUE(range.IsExhaustive());
}

TEST(ExhaustivenessTest, InexhaustiveBooleanRangeJustFalse) {
  BitsLikeProperties properties = BitsLikeProperties{TypeDim::CreateBool(false), TypeDim::CreateU32(1)};
  auto range = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(1, 0), InterpValue::MakeUBits(1, 0));
  EXPECT_FALSE(range.IsExhaustive());
}

TEST(ExhaustivenessTest, InexhaustiveBooleanRangeJustTrue) {
  BitsLikeProperties properties = BitsLikeProperties{TypeDim::CreateBool(false), TypeDim::CreateU32(1)};
  auto range = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(1, 1), InterpValue::MakeUBits(1, 1));
  EXPECT_FALSE(range.IsExhaustive());
}

TEST(ExhaustivenessTest, MergeTwoInexhaustiveBooleanRangesMakesExhaustive) {
  BitsLikeProperties properties = BitsLikeProperties{TypeDim::CreateBool(false), TypeDim::CreateU32(1)};
  auto range1 = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(1, 0), InterpValue::MakeUBits(1, 0));
  auto range2 = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(1, 1), InterpValue::MakeUBits(1, 1));
  auto merged = BitsValueRange::Merge(range1, range2);
  EXPECT_EQ(merged.disjoint().size(), 1);
  LOG(ERROR) << "merged: " << merged.ToString();
  EXPECT_TRUE(merged.IsExhaustive());
  EXPECT_EQ(merged.disjoint().begin()->ToString(), "(u1:0, u1:1)");
}

// Make a three bit uint range and fill in the middle value last.
TEST(ExhaustivenessTest, ThreeBitRangeFillInMiddleLast) {
  constexpr int64_t kBitCount = 3;
  BitsLikeProperties properties = BitsLikeProperties{TypeDim::CreateBool(false), TypeDim::CreateU32(kBitCount)};
  auto low_02 = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(kBitCount, 0), InterpValue::MakeUBits(kBitCount, 2));
  auto high_47 = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(kBitCount, 4), InterpValue::MakeUBits(kBitCount, 7));
  auto merged = BitsValueRange::Merge(low_02, high_47);
  EXPECT_EQ(merged.disjoint().size(), 2);
  LOG(ERROR) << "merged: " << merged.ToString();
  EXPECT_FALSE(merged.IsExhaustive());

  auto range_with_3 = BitsValueRange::MakeSingleRange(properties, InterpValue::MakeUBits(kBitCount, 3), InterpValue::MakeUBits(kBitCount, 3));
  auto merged_with_3 = BitsValueRange::Merge(merged, range_with_3);
  EXPECT_EQ(merged_with_3.disjoint().size(), 1);
  LOG(ERROR) << "merged_with_3: " << merged_with_3.ToString();
  EXPECT_TRUE(merged_with_3.IsExhaustive());
  EXPECT_EQ(merged_with_3.disjoint().begin()->ToString(), "(u3:0, u3:7)");
}

}  // namespace xls::dslx
