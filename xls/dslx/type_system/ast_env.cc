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

#include "xls/dslx/type_system/ast_env.h"

#include "absl/types/variant.h"
#include "xls/common/visitor.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/proc.h"
#include "absl/strings/str_join.h"
namespace xls::dslx {

/* static */ NameDef* AstEnv::GetNameDefForKey(KeyT key) {
  return absl::visit(Visitor{[](const Param* n) { return n->name_def(); },
                             [](const ProcMember* n) { return n->name_def(); }},
                     key);
}

std::string AstEnv::ToString() const {
  return absl::StrJoin(map_, ", ",
      [](std::string* out, const std::pair<KeyT, InterpValue>& p) {
        absl::StrAppend(out, ToAstNode(p.first)->ToString(), ": ", p.second.ToString());
      });
}

}  // namespace xls::dslx
