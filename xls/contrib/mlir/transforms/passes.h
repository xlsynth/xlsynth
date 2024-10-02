// Copyright 2024 The XLS Authors
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

#ifndef GDM_HW_MLIR_XLS_TRANSFORMS_PASSES_H_
#define GDM_HW_MLIR_XLS_TRANSFORMS_PASSES_H_

#include "mlir/include/mlir/Pass/Pass.h"  // IWYU pragma: keep

namespace mlir::xls {

#define GEN_PASS_DECL
#include "xls/contrib/mlir/transforms/passes.h.inc"

// Generate the code for registering passes.
#define GEN_PASS_REGISTRATION
#include "xls/contrib/mlir/transforms/passes.h.inc"

}  // namespace mlir::xls

#endif  // GDM_HW_MLIR_XLS_TRANSFORMS_PASSES_H_
