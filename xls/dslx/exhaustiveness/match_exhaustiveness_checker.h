// Copyright 2025 The XLS Authors
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
#ifndef XLS_DSLX_EXHAUSTIVENESS_MATCH_EXHAUSTIVENESS_CHECKER_H_
#define XLS_DSLX_EXHAUSTIVENESS_MATCH_EXHAUSTIVENESS_CHECKER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "xls/dslx/exhaustiveness/match_pattern_overlap.h"
#include "xls/dslx/exhaustiveness/nd_region.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {

// Object that we can incrementally feed match arms/patterns to and ask whether
// we've reached a point where the patterns are exhaustive. For sums, this
// tracks source-level constructor coverage for well-formed semantic values
// only; malformed or otherwise raw boundary encodings are outside this
// checker's model. This is useful for flagging a warning right when we've
// reached the point that the arms are exhaustive.
class MatchExhaustivenessChecker {
 public:
  struct PatternAddResult {
    // Numeric enum aliases denote one value; distinct sum constructors do not.
    struct AddsCoverage {};
    // The pattern does not match any inhabited value in the original domain.
    struct Unmatchable {};
    struct Overlap {
      // Equally refutable patterns covering the same inhabited values are
      // exact duplicates; differently spelled catch-alls are merely covered.
      MatchPatternOverlapKind kind;
      // An exact semantic predecessor is preferred over the first pattern
      // that merely intersects collective previous coverage.
      Span previous_pattern_span;
    };

    std::variant<AddsCoverage, Unmatchable, Overlap> outcome;

    bool adds_coverage() const {
      return std::holds_alternative<AddsCoverage>(outcome);
    }
    bool is_unmatchable() const {
      return std::holds_alternative<Unmatchable>(outcome);
    }
    const Overlap* overlap() const { return std::get_if<Overlap>(&outcome); }
  };

  MatchExhaustivenessChecker(const Span& matched_expr_span,
                             const TypeInfo& type_info,
                             const Type& matched_type);
  ~MatchExhaustivenessChecker();

  MatchExhaustivenessChecker(const MatchExhaustivenessChecker&) = delete;
  MatchExhaustivenessChecker& operator=(const MatchExhaustivenessChecker&) =
      delete;

  // Incorporates `pattern` and reports its contribution and the previous
  // source provenance when it is already fully covered.
  PatternAddResult AddPattern(const PatternTree& pattern);

  // Returns whether, based on already-added patterns, we're exhaustive in the
  // checker's model. For sums, that means every declared constructor payload
  // space is covered, not that every raw boundary encoding has been handled.
  bool IsExhaustive() const;

  // Formats a sample from the first uncovered region for user-facing match
  // diagnostics. Enum declarations and semantic-sum constructors retain their
  // source names instead of exposing their numeric storage representations.
  std::optional<std::string> FormatSimplestUncoveredValue() const;

 private:
  struct Impl;
  const FileTable& file_table() const;

  std::unique_ptr<Impl> impl_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_EXHAUSTIVENESS_MATCH_EXHAUSTIVENESS_CHECKER_H_
