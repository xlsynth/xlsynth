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

#include "xls/dslx/import_routines.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/virtualizable_file_system.h"

namespace xls::dslx {
namespace {

// Verifies: Imports found in Bazel runfiles return their physical location.
// Catches: Returning the logical source path, which is unusable from this cwd.
TEST(ImportRoutinesTest, ReturnsFilesystemPathForRunfile) {
  const std::filesystem::path logical_path = "xls/dslx/stdlib/std.x";
  XLS_ASSERT_OK_AND_ASSIGN(std::string runfile,
                           GetXlsRunfilePath(logical_path));
  const std::filesystem::path physical_path(runfile);
  ASSERT_NE(physical_path, logical_path);
  auto vfs = std::make_unique<FakeFilesystem>(
      absl::flat_hash_map<std::filesystem::path, std::string>{
          {physical_path, ""}},
      std::filesystem::temp_directory_path() / "unrelated-dslx-import-cwd");
  ImportData import_data = CreateImportDataForTest(std::move(vfs));
  XLS_ASSERT_OK_AND_ASSIGN(ImportTokens tokens,
                           ImportTokens::FromString("xls.dslx.stdlib.std"));

  XLS_ASSERT_OK_AND_ASSIGN(
      std::filesystem::path actual,
      FindImportFilesystemPath(tokens, "caller.x", import_data));
  EXPECT_EQ(actual, physical_path);
}

}  // namespace
}  // namespace xls::dslx
