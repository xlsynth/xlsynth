// Copyright 2026 The XLS Authors
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

#include "xls/ir/events.h"

#include <string>
#include <string_view>
#include <vector>

#include "xls/ir/evaluator_result.pb.h"

namespace xls {

// Keeps assertion-only users from pulling trace formatting dependencies out of
// `events.cc` while preserving one shared `InterpreterEvents` implementation.
void InterpreterEvents::AddAssertMessage(
    std::string_view msg, std::optional<SourceInfo> source_info,
    std::optional<std::string> source_filename) {
  AssertMessageProto* am = proto_.add_assert_msgs();
  am->set_message(std::string{msg});
  if (source_info.has_value() && !source_info->locations.empty()) {
    const SourceLocation& loc = source_info->locations.front();
    auto* locp = am->mutable_location();
    if (source_filename.has_value()) {
      locp->set_filename(*source_filename);
    }
    locp->set_line(loc.lineno().value());
    locp->set_column(loc.colno().value());
  }
}

std::vector<std::string> InterpreterEvents::GetAssertMessages() const {
  std::vector<std::string> asserts;
  asserts.reserve(proto_.assert_msgs_size());
  for (const AssertMessageProto& a : proto_.assert_msgs()) {
    asserts.push_back(a.message());
  }
  return asserts;
}

int64_t InterpreterEvents::GetAssertMessageCount() const {
  return proto_.assert_msgs_size();
}

const std::string* InterpreterEvents::GetAssertMessage(int64_t index) const {
  if (index < 0 || index >= proto_.assert_msgs_size()) {
    return nullptr;
  }
  return &proto_.assert_msgs(index).message();
}

}  // namespace xls
