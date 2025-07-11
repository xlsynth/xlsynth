#include "xls/dslx/resolve_import_dag_contents.h"

#include <openssl/sha.h>

#include <deque>
#include <filesystem>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls::dslx {

namespace {
// Helper that returns the file contents for the given path via the VFS.
absl::StatusOr<std::string> ReadFileContents(const std::filesystem::path& path,
                                           const ImportData& import_data) {
  return import_data.vfs().GetFileContents(path);
}

// Computes a sha256 digest for the given data and returns it in the
// "sha256:<hex>" canonical format.
std::string Sha256Digest(std::string_view data) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest.data());
  return absl::StrCat(
      "sha256:",
      absl::BytesToHexString({reinterpret_cast<const char*>(digest.data()),
                              digest.size()}));
}

// Determines whether the given filesystem path is considered part of the
// standard library according to `import_data`.
bool IsStdlibPath(const std::filesystem::path& path,
                  const ImportData& import_data) {
  // Consider the path stdlib if its string contains the stdlib path as a
  // substring (simple and reliable for our purposes).
  return path.string().find(import_data.stdlib_path().string()) !=
         std::string::npos;
}

// Enqueues all imported modules referenced by `module` into `queue` if they
// have not been visited.
void EnqueueImports(const Module* module, const ImportData& import_data,
                    std::deque<const Module*>* queue,
                    absl::flat_hash_set<const Module*>* visited) {
  // The Module helper returns a mapping of import name -> Import*.
  absl::flat_hash_map<std::string, Import*> imports =
      module->GetImportByName();
  for (auto& [name, import_node] : imports) {
    ImportTokens tokens(import_node->subject());
    if (!import_data.Contains(tokens)) {
      continue;  // Should not happen, but be tolerant.
    }
    absl::StatusOr<ModuleInfo*> info_or = import_data.Get(tokens);
    if (!info_or.ok()) {
      continue;  // Skip on error.
    }
    const Module* imported_mod = &info_or.value()->module();
    if (visited->insert(imported_mod).second) {
      queue->push_back(imported_mod);
    }
  }
}

}  // namespace

absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
ResolveImportDagContents(const TypecheckedModule& tm,
                         const ImportData& import_data) {
  absl::flat_hash_map<std::string, std::string> result;
  absl::flat_hash_set<const Module*> visited;
  std::deque<const Module*> queue;

  // Seed with the root module.
  visited.insert(tm.module);
  queue.push_back(tm.module);

  while (!queue.empty()) {
    const Module* module = queue.front();
    queue.pop_front();

    // Attempt to determine the filesystem path. Modules created from in-memory
    // text may not have one; skip in that case.
    if (!module->fs_path().has_value()) {
      // Still enqueue imports but skip content extraction.
      EnqueueImports(module, import_data, &queue, &visited);
      continue;
    }

    std::filesystem::path path = *module->fs_path();

    // Ensure we work with an absolute filesystem path for user modules so the
    // key matches the specification (and the test expectations).
    if (!path.is_absolute()) {
      if (auto cwd = import_data.vfs().GetCurrentDirectory(); cwd.ok()) {
        path = *cwd / path;
      }
    }

    bool is_stdlib = IsStdlibPath(path, import_data);

    // Determine the key used in the mapping.
    std::string key;
    if (is_stdlib) {
      key = path.filename().string();  // e.g. "std.x"
    } else {
      key = path.string();  // absolute or relative path for user modules.
    }

    // If we've already inserted this key, we don't need to re-process.
    if (!result.contains(key)) {
      XLS_ASSIGN_OR_RETURN(std::string contents,
                           ReadFileContents(path, import_data));
      if (is_stdlib) {
        result[key] = Sha256Digest(contents);
      } else {
        result[key] = contents;
      }
    }

    // Traverse child imports.
    EnqueueImports(module, import_data, &queue, &visited);
  }

  return result;
}

}  // namespace xls::dslx
