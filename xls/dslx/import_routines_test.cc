#include "xls/dslx/parse_and_typecheck.h"

#include <openssl/sha.h>
#include <gtest/gtest.h>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/default_dslx_stdlib_path.h"

namespace xls::dslx {

namespace {

// Helper that computes the sha256 digest string used by ResolveImportDagContents.
std::string Sha256Digest(std::string_view data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  return absl::StrCat("sha256:",
                      absl::BytesToHexString({reinterpret_cast<const char*>(digest),
                                              SHA256_DIGEST_LENGTH}));
}

TEST(ResolveImportDagContentsTest, BasicImportDag) {
  // Set up an in-memory file system with a small import DAG.
  const std::string kFooPath = "/fake/foo.x";
  const std::string kBarPath = "/fake/bar.x";
  const std::string kStdlibDir = std::string(xls::kDefaultDslxStdlibPath);
  const std::string kStdPath = absl::StrCat("/fake/", kStdlibDir, "/std.x");

  const std::string kStdContents = R"(
fn add(a: u32, b: u32) -> u32 { a + b }
)";
  const std::string kBarContents = R"(
import std;
fn f() -> u32 { std::add(u32:1, u32:41) }
)";
  const std::string kFooContents = R"(
import bar;
import std;
fn main() -> u32 { bar::f() }
)";

  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {kFooPath, kFooContents}, {kBarPath, kBarContents}, {kStdPath, kStdContents}};

  auto vfs = std::make_unique<FakeFilesystem>(files, "/fake");
  ImportData import_data = CreateImportDataForTest(std::move(vfs));

  // Parse and typecheck the root module.
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, ParseAndTypecheck(
                                               kFooContents, kFooPath, "foo", &import_data));

  // Resolve the DAG contents.
  XLS_ASSERT_OK_AND_ASSIGN(absl::flat_hash_map<std::string, std::string> result,
                           ResolveImportDagContents(tm, import_data));

  // Expected entries.
  absl::flat_hash_map<std::string, std::string> expected = {
      {kFooPath, kFooContents},
      {kBarPath, kBarContents},
      {"std.x", Sha256Digest(kStdContents)},
  };

  EXPECT_EQ(result, expected);
}

}  // namespace

}  // namespace xls::dslx
