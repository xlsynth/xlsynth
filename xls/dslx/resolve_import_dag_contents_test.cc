#include "xls/dslx/resolve_import_dag_contents.h"

#include <openssl/sha.h>
#include <gtest/gtest.h>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"

namespace xls::dslx {
namespace {

// Computes the digest string used by ResolveImportDagContents.
std::string Sha256Digest(std::string_view data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest);
  return absl::StrCat(
      "sha256:",
      absl::BytesToHexString({reinterpret_cast<const char*>(digest),
                              SHA256_DIGEST_LENGTH}));
}

TEST(ResolveImportDagContentsTest, CollectsUserAndStdlibModules) {
  // Build an in-memory file system representing a simple import graph:
  //   foo.x imports bar.x and std.x
  //   bar.x imports std.x
  const std::string kFooPath = "/mem/usr/foo.x";
  const std::string kBarPath = "/mem/usr/bar.x";
  const std::string kStdPath = "/mem/stdlib/std.x";

  const std::string kStdContents = R"(
pub fn add(a: u32, b: u32) -> u32 { a + b }
)";
  const std::string kBarContents = R"(
import std;
pub fn f() -> u32 { std::add(u32:1, u32:41) }
)";
  const std::string kFooContents = R"(
import bar;
import std;
fn main() -> u32 { bar::f() }
)";

  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {kFooPath, kFooContents},
      {kBarPath, kBarContents},
      {kStdPath, kStdContents},
  };

  auto vfs = std::make_unique<FakeFilesystem>(files, "/mem");
  ImportData import_data = CreateImportData(
    "/mem/stdlib",
    /*additional_search_paths=*/{"/mem/usr"},
    kAllWarningsSet,
    std::move(vfs));

  XLS_ASSERT_OK_AND_ASSIGN(auto tm,
                           ParseAndTypecheck(kFooContents, kFooPath, "foo",
                                            &import_data));

  XLS_ASSERT_OK_AND_ASSIGN(auto dag_contents,
                           ResolveImportDagContents(tm, import_data));

  absl::flat_hash_map<std::string, std::string> expected = {
      {kFooPath, kFooContents},
      {kBarPath, kBarContents},
      {"std.x", Sha256Digest(kStdContents)},
  };

  EXPECT_EQ(dag_contents, expected);
}

}  // namespace
}  // namespace xls::dslx
