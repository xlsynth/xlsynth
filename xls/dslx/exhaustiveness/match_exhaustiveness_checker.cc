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

#include "xls/dslx/exhaustiveness/match_exhaustiveness_checker.h"

#include "absl/log/log.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/visitor.h"

namespace xls::dslx {

absl::StatusOr<MatchExhaustivenessChecker> MatchExhaustivenessChecker::Make(
    const Match& match, const TypeInfo& type_info) {
  const Expr* matched = match.matched();
  std::optional<const Type*> matched_type = type_info.GetItem(matched);
  XLS_RET_CHECK(matched_type.has_value());
  std::optional<BitsLikeProperties> bits_like =
      GetBitsLike(*matched_type.value());
  if (!bits_like.has_value()) {
    return absl::UnimplementedError(
        absl::StrCat("MatchExhaustivenessChecker does not support type ",
                     matched_type.value()->ToString()));
  }
  return MatchExhaustivenessChecker(match, type_info, *bits_like);
}

MatchExhaustivenessChecker::MatchExhaustivenessChecker(
    const Match& match, const TypeInfo& type_info, BitsLikeProperties bits_like)
    : match_(match),
      type_info_(type_info),
      bits_like_(bits_like),
      tested_(BitsValueRange::MakeEmpty(bits_like)) {}

bool MatchExhaustivenessChecker::AddPattern(const NameDefTree& pattern) {
  CHECK(pattern.is_leaf());
  if (pattern.IsIrrefutable()) {
    tested_ = BitsValueRange::MakeFull(bits_like_);
    return true;
  }
  absl::visit(
      Visitor{
          [&](auto* node) {
            std::optional<InterpValue> value =
                type_info_.GetConstExprOption(node);
            CHECK(value.has_value());
            tested_ = BitsValueRange::Merge(
                tested_,
                BitsValueRange::MakeSingleRange(bits_like_, *value, *value));
          },
          [&](Range* range) {
            std::optional<InterpValue> start =
                type_info_.GetConstExprOption(range->start());
            CHECK(start.has_value());
            std::optional<InterpValue> end =
                type_info_.GetConstExprOption(range->end());
            CHECK(end.has_value());
            tested_ = BitsValueRange::Merge(
                tested_,
                BitsValueRange::MakeSingleRange(bits_like_, *start, *end));
          },
          [](RestOfTuple* rest_of_tuple) {
            LOG(FATAL) << "RestOfTuple not valid for boolean value match";
          },
      },
      pattern.leaf());
  return tested_.IsExhaustive();
}

}  // namespace xls::dslx