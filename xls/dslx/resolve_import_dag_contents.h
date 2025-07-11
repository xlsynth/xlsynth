#ifndef XLS_DSLX_RESOLVE_IMPORT_DAG_CONTENTS_H_
#define XLS_DSLX_RESOLVE_IMPORT_DAG_CONTENTS_H_

#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/container/flat_hash_map.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {

struct TypecheckedModule;  // Forward declaration.

// Resolves all of the imports in the import DAG under `tm`.
// For user modules the map key is the absolute filesystem path and value is
// the file contents. For stdlib modules the key is e.g. "std.x" and the value
// is "sha256:<digest>" of the module contents.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
ResolveImportDagContents(const TypecheckedModule& tm,
                         const ImportData& import_data);

}  // namespace xls::dslx

#endif  // XLS_DSLX_RESOLVE_IMPORT_DAG_CONTENTS_H_
