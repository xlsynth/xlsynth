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

#ifndef XLS_DSLX_IMPORT_RECORD_H_
#define XLS_DSLX_IMPORT_RECORD_H_

#include <filesystem>

#include "xls/dslx/frontend/pos.h"

namespace xls::dslx {

struct ImportRecord {
  // The path being imported.
  std::filesystem::path imported;
  // The span of the triggering import. Note that when this is the "entry
  // file" the position will be line/col (0, 0) as there is no syntactical
  // import statement driving that file to be imported.
  Span imported_from;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_IMPORT_RECORD_H_
