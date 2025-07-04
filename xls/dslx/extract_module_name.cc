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

#include "xls/dslx/extract_module_name.h"

#include <filesystem>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"

namespace xls::dslx {

absl::StatusOr<std::string> ExtractModuleName(
    const std::filesystem::path& path) {
  if (path.extension() != ".x") {
    return absl::InvalidArgumentError(absl::StrFormat(
        "DSLX module path must end with '.x', got: '%s'", path));
  }
  return path.stem();
}

}  // namespace xls::dslx
