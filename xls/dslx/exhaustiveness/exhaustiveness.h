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

#ifndef XLS_DSLX_EXHAUSTIVENESS_EXHAUSTIVENESS_H_
#define XLS_DSLX_EXHAUSTIVENESS_EXHAUSTIVENESS_H_

#include "absl/container/btree_set.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {

class InterpValueRange {
 public:
  InterpValueRange(InterpValue min, InterpValue max);

  void ExtendToInclude(InterpValue value);

  bool Contains(InterpValue value) const;

  bool operator==(const InterpValueRange& other) const {
    return min_ == other.min_ && max_ == other.max_;
  }

  bool operator<(const InterpValueRange& other) const {
    return min_ < other.min_ || (min_ == other.min_ && max_ < other.max_);
  }

  bool ShouldMergeWith(const InterpValueRange& other) const;

  const InterpValue& min() const { return min_; }
  const InterpValue& max() const { return max_; }

  std::string ToString() const;

 private:
  bool IsSigned() const;
  int64_t GetBitCount() const;
  InterpValue min_;
  InterpValue max_;
};

class BitsValueRange {
 public:
  static BitsValueRange MakeSingleRange(BitsLikeProperties bits_like,
                                        InterpValue min, InterpValue max);

  // Make an empty range for the given bits type.
  static BitsValueRange MakeEmpty(BitsLikeProperties bits_like);

  static BitsValueRange MakeFull(BitsLikeProperties bits_like);

  static BitsValueRange Merge(const BitsValueRange& lhs,
                              const BitsValueRange& rhs);

  BitsValueRange(const BitsValueRange& other) = default;
  BitsValueRange(BitsValueRange&& other) = default;

  BitsValueRange& operator=(const BitsValueRange& other) {
    bits_like_ = Clone(other.bits_like_);
    disjoint_ = other.disjoint_;
    return *this;
  }

  BitsValueRange& operator=(BitsValueRange&& other) {
    bits_like_ = std::move(other.bits_like_);
    disjoint_ = std::move(other.disjoint_);
    return *this;
  }

  bool IsExhaustive() const;

  const absl::btree_set<InterpValueRange>& disjoint() const {
    return disjoint_;
  }

  std::string ToString() const;

 private:
  BitsValueRange(BitsLikeProperties bits_like,
                 absl::btree_set<InterpValueRange> disjoint)
      : bits_like_(bits_like), disjoint_(disjoint) {}

  int64_t GetBitCount() const;
  bool IsSigned() const;

  BitsLikeProperties bits_like_;

  // Minimal set of disjoint (i.e. non continuous) ranges.
  absl::btree_set<InterpValueRange> disjoint_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_EXHAUSTIVENESS_EXHAUSTIVENESS_H_