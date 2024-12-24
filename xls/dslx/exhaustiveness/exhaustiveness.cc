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

#include "xls/dslx/exhaustiveness/exhaustiveness.h"

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace xls::dslx {
namespace {

InterpValue MakeMinValue(BitsLikeProperties bits_like) {
  int64_t bit_count = bits_like.size.GetAsInt64().value();
  bool is_signed = bits_like.is_signed.GetAsBool().value();
  return InterpValue::MakeMinValue(is_signed, bit_count);
}

InterpValue MakeMaxValue(BitsLikeProperties bits_like) {
  int64_t bit_count = bits_like.size.GetAsInt64().value();
  bool is_signed = bits_like.is_signed.GetAsBool().value();
  return InterpValue::MakeMaxValue(is_signed, bit_count);
}

InterpValueRange ExhaustiveRangeFor(BitsLikeProperties bits_like) {
  return InterpValueRange(MakeMinValue(bits_like), MakeMaxValue(bits_like));
}

InterpValue MinStart(const std::optional<InterpValueRange>& lhs,
                     const std::optional<InterpValueRange>& rhs) {
  if (lhs.has_value() && rhs.has_value()) {
    if (lhs->min() < rhs->min()) {
      VLOG(5) << "returning lhs min: " << lhs->min().ToString();
      return lhs->min();
    }
    VLOG(5) << "returning rhs min: " << rhs->min().ToString();
    return rhs->min();
  }
  if (lhs.has_value()) {
    VLOG(5) << "returning lhs min: " << lhs->min().ToString();
    return lhs->min();
  }
  if (rhs.has_value()) {
    VLOG(5) << "returning rhs min: " << rhs->min().ToString();
    return rhs->min();
  }
  LOG(FATAL) << "At least one of the ranges must be provided";
}

}  // namespace

InterpValueRange::InterpValueRange(InterpValue min, InterpValue max)
    : min_(std::move(min)), max_(std::move(max)) {
  absl::StatusOr<InterpValue> le = min_.Le(max_);
  CHECK_OK(le.status());
  CHECK(le->IsTrue()) << absl::StreamFormat("need min=%s <= max=%s",
                                            min_.ToString(), max_.ToString());
}

bool InterpValueRange::Contains(InterpValue value) const {
  return min_.Le(value)->IsTrue() && max_.Ge(value)->IsTrue();
}

std::string InterpValueRange::ToString() const {
  return absl::StrFormat("(%s, %s)", min_.ToString(), max_.ToString());
}

void InterpValueRange::ExtendToInclude(InterpValue value) {
  // If the value is already included in the interval we don't have to do
  // anything.
  if (Contains(value)) {
    return;
  }

  absl::StatusOr<InterpValue> le = max_.Le(value);
  CHECK_OK(le.status());
  CHECK(le->IsTrue()) << absl::StreamFormat(
      "ExtendToInclude(%s) but current max is %s", value.ToString(),
      max_.ToString());
  max_ = value;
}

bool InterpValueRange::IsSigned() const {
  CHECK_EQ(min_.IsSigned(), max_.IsSigned());
  return min_.IsSigned();
}

int64_t InterpValueRange::GetBitCount() const {
  CHECK_EQ(min_.GetBitCount(), max_.GetBitCount());
  return min_.GetBitCount().value();
}

bool InterpValueRange::ShouldMergeWith(const InterpValueRange& other) const {
  InterpValue one = InterpValue::MakeOneValue(IsSigned(), GetBitCount());
  InterpValue this_max_plus_one = max_.Add(one).value();
  // Note: we should merge when the ranges look like so:
  // ```
  // min  max
  // |-----|
  //         max+1   c
  //         |-------|
  // ```
  // i.e. if the current range's max+1 is >= the other range's min.
  //
  // However, notably the max+1 calculation can overflow.
  bool did_overflow = this_max_plus_one.Le(max_)->IsTrue();
  bool result = (this_max_plus_one.Ge(other.min_)->IsTrue() && !did_overflow) ||
                max_.Eq(other.min_);
  VLOG(5) << absl::StreamFormat(
      "ShouldMergeWith(%s, %s) this_max_plus_one=%s other.min_=%s result=%s",
      ToString(), other.ToString(), this_max_plus_one.ToString(),
      other.min_.ToString(), result ? "true" : "false");
  return result;
}

BitsValueRange BitsValueRange::MakeSingleRange(BitsLikeProperties bits_like,
                                               InterpValue min,
                                               InterpValue max) {
  return BitsValueRange(
      bits_like, absl::btree_set<InterpValueRange>{InterpValueRange(min, max)});
}

BitsValueRange BitsValueRange::MakeEmpty(BitsLikeProperties bits_like) {
  return BitsValueRange(bits_like, absl::btree_set<InterpValueRange>{});
}

BitsValueRange BitsValueRange::MakeFull(BitsLikeProperties bits_like) {
  return BitsValueRange(bits_like, absl::btree_set<InterpValueRange>{
                                       ExhaustiveRangeFor(bits_like)});
}

int64_t BitsValueRange::GetBitCount() const {
  return bits_like_.size.GetAsInt64().value();
}

bool BitsValueRange::IsSigned() const {
  return bits_like_.is_signed.GetAsInt64().value();
}

bool BitsValueRange::IsExhaustive() const {
  return disjoint_.size() == 1 &&
         *disjoint_.begin() == ExhaustiveRangeFor(bits_like_);
}

BitsValueRange BitsValueRange::Merge(const BitsValueRange& lhs,
                                     const BitsValueRange& rhs) {
  CHECK_EQ(lhs.bits_like_, rhs.bits_like_);

  const auto& lhs_ranges = lhs.disjoint();
  const auto& rhs_ranges = rhs.disjoint();

  if (lhs_ranges.empty() && rhs_ranges.empty()) {
    return BitsValueRange(lhs.bits_like_, absl::btree_set<InterpValueRange>{});
  }

  absl::btree_set<InterpValueRange> result;
  // Walk through the ranges for the LHS and RHS simultaneously, and if there's
  // overlap, merge them. Once we walk to a spot there's no overlap, we can emit
  // the range into the result.
  //
  // We do it this way because we need to make sure we only place results that
  // are maximally contiguous, so we have to ensure there are holes between any
  // ranges we place into the result.
  InterpValue init_start =
      MinStart(lhs_ranges.empty() ? std::nullopt
                                  : std::make_optional(*lhs_ranges.begin()),
               rhs_ranges.empty() ? std::nullopt
                                  : std::make_optional(*rhs_ranges.begin()));
  std::optional<InterpValueRange> current_range =
      InterpValueRange(init_start, init_start);
  VLOG(5) << "current_range init: " << current_range->ToString();
  auto lhs_iter = lhs_ranges.begin();
  auto rhs_iter = rhs_ranges.begin();
  while (true) {
    if (lhs_iter == lhs_ranges.end() && rhs_iter == rhs_ranges.end()) {
      break;
    }
    if (!current_range.has_value()) {
      // Make whichever interval is smaller the current range and let them get
      // popped in subsequent processing steps.
      InterpValue min_start = MinStart(
          lhs_iter == lhs_ranges.end() ? std::nullopt
                                       : std::make_optional(*lhs_iter),
          rhs_iter == rhs_ranges.end() ? std::nullopt
                                       : std::make_optional(*rhs_iter));
      current_range = InterpValueRange(min_start, min_start);
      VLOG(5) << "current_range is now: " << current_range->ToString();
      continue;
    }
    CHECK(current_range.has_value());
    if (lhs_iter != lhs_ranges.end() &&
        current_range->ShouldMergeWith(*lhs_iter)) {
      VLOG(5) << "extending via lhs contiguous range: " << lhs_iter->ToString();
      current_range->ExtendToInclude(lhs_iter->max());
      VLOG(5) << "current_range is now: " << current_range->ToString();
      ++lhs_iter;
      continue;
    }
    if (rhs_iter != rhs_ranges.end() &&
        current_range->ShouldMergeWith(*rhs_iter)) {
      VLOG(5) << "extending via rhs contiguous range: " << rhs_iter->ToString();
      current_range->ExtendToInclude(rhs_iter->max());
      VLOG(5) << "current_range is now: " << current_range->ToString();
      ++rhs_iter;
      continue;
    }
    result.insert(current_range.value());
    VLOG(5) << "emitting current_range: " << current_range->ToString();
    current_range = std::nullopt;
  }

  if (current_range.has_value()) {
    result.insert(current_range.value());
    VLOG(5) << "emitting final current_range: " << current_range->ToString();
  }

  return BitsValueRange(lhs.bits_like_, std::move(result));
}

std::string BitsValueRange::ToString() const {
  std::vector<std::string> pieces;
  for (const auto& range : disjoint_) {
    pieces.push_back(range.ToString());
  }
  return absl::StrFormat("[%s]", absl::StrJoin(pieces, ", "));
}

}  // namespace xls::dslx
