// Copyright 2025 The XLS Authors
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

#include "xls/contrib/mlir/util/identifier.h"

#include <string>

#include "llvm/include/llvm/ADT/StringRef.h"  // IWYU pragma: keep
#include "mlir/include/mlir/Support/LLVM.h"
#include "xls/codegen/vast/vast.h"

namespace mlir::xls {

std::string CleanupIdentifier(StringRef name) {
  return ::xls::verilog::SanitizeVerilogIdentifier(name.str());
}

}  // namespace mlir::xls
