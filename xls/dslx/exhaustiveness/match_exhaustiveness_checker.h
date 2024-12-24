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
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {

// Object that we can incrementally feed match arms/patterns to and ask whether we've reached
// a point where the patterns are exhaustive. This is useful for flagging a warning right when
// we've reached the point that the arms are exhaustive.
class MatchExhaustivenessChecker {
 public:
  static absl::StatusOr<MatchExhaustivenessChecker> Make(const Match& match, const TypeInfo& type_info);

  // Returns whether we've reached a point of exhaustiveness.
  bool AddPattern(const NameDefTree& pattern);

 private:
  MatchExhaustivenessChecker(const Match& match, const TypeInfo& type_info, BitsLikeProperties bits_like);

  const Match& match_;
  const TypeInfo& type_info_;
  BitsLikeProperties bits_like_;
  BitsValueRange tested_;
};

}  // namespace xls::dslx