// Copyright 2022 The XLS Authors
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
#ifndef XLS_DSLX_BYTECODE_BYTECODE_CACHE_INTERFACE_H_
#define XLS_DSLX_BYTECODE_BYTECODE_CACHE_INTERFACE_H_

#include <optional>

#include "absl/status/statusor.h"
#include "xls/dslx/bytecode/bytecode.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {

// Forward decl.
class ImportData;

// Defines the interface a type must provide in order to serve as a bytecode
// cache. In practice, this type exists to avoid attaching too many concrete
// dependencies onto ImportData, which is the primary cache owner.
class BytecodeCacheInterface {
 public:
  virtual ~BytecodeCacheInterface() = default;

  // Returns the BytecodeFunction for the given function, whose types and
  // constants are held inside the given `TypeInfo` - different instances of a
  // parametric function will have different `TypeInfo`s associated with them.
  //
  // Args:
  //   f: The function to get or create the BytecodeFunction for.
  //   type_info: The TypeInfo for the function.
  //   parametric_env: The parametric environment for the function invocation, i.e. the parametric bindings used for this invocation in the callee `f`.
  virtual absl::StatusOr<BytecodeFunction*> GetOrCreateBytecodeFunction(
      ImportData& import_data, const Function& f, const TypeInfo* type_info,
      const std::optional<ParametricEnv>& caller_bindings) = 0;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_BYTECODE_BYTECODE_CACHE_INTERFACE_H_
