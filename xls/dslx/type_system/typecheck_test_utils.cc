// Copyright 2023 The XLS Authors
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

#include "xls/dslx/type_system/typecheck_test_utils.h"

#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type_info_to_proto.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/common/logging/log_lines.h"
#include "xls/dslx/type_system_v2/type_system_test_utils.h"
#include "xls/common/indent.h"

namespace xls::dslx {
namespace {

std::string TypeInfoConstexprToString(const TypeInfo& ti, const FileTable& file_table) {
  std::vector<std::string> items;
  for (const auto& [node, maybe_value] : ti.const_exprs()) {
    if (!maybe_value.has_value()) {
      continue;
    }
    items.push_back(absl::StrFormat("span: %s, node: `%s`, value: `%s`", node->GetSpan()->ToString(file_table), node->ToString(), maybe_value->ToString()));
  }
  return absl::StrJoin(items, "\n");
}

std::string TypeInfoTreeToString(const TypeInfo& ti, const FileTable& file_table) {
  std::string result = "Root:\n" + Indent(TypeInfoToString(ti, file_table).value());
  for (const auto& [invocation, invocation_data] : ti.GetRootInvocations()) {
    absl::StrAppend(&result, "\n-- Invocation: `", invocation->ToString(), "`");
    for (const auto& [env, callee_data] : invocation_data.env_to_callee_data()) {
      absl::StrAppend(&result, "\nCaller Env: ", env.ToString());
      absl::StrAppend(&result, "\nCallee Env: ", callee_data.callee_bindings.ToString());
      if (callee_data.derived_type_info == nullptr) {
        absl::StrAppend(&result, "\nType Info: <null>");
        continue;
      }
      absl::StrAppend(&result, "\nType Info:\n", Indent(TypeInfoToString(*callee_data.derived_type_info, file_table).value()));
      absl::StrAppend(&result, "\nConstexpr:\n", Indent(TypeInfoConstexprToString(*callee_data.derived_type_info, file_table)));
    }
  }
  return result;
}

}  // namespace

absl::StatusOr<TypecheckResult> Typecheck(std::string_view text) {
  auto import_data = CreateImportDataPtrForTest();
  absl::StatusOr<TypecheckedModule> tm =
      ParseAndTypecheck(text, "fake.x", "fake", import_data.get());
  if (!tm.ok()) {
    UniformContentFilesystem vfs(text);
    TryPrintError(tm.status(), import_data->file_table(), vfs);
    return tm.status();
  }

  if (VLOG_IS_ON(0)) {
    std::string type_info_string = TypeInfoTreeToString(*tm->type_info, *tm->module->file_table());
    XLS_VLOG_LINES(0, type_info_string);
  }

  // Ensure that we can convert all the type information in the unit tests into
  // its protobuf form.
  XLS_RETURN_IF_ERROR(TypeInfoToProto(*tm->type_info).status());

  return TypecheckResult{std::move(import_data), std::move(*tm)};
}

}  // namespace xls::dslx
