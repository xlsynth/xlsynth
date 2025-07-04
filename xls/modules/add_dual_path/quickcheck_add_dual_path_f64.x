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

import float64;

import xls.modules.add_dual_path.dual_path;

type F64 = float64::F64;

#[quickcheck(test_count=10000)]
fn quickcheck_add_dual_path_f64(x: F64, y: F64) -> bool {
    dual_path::add_dual_path(x, y) == float64::add(x, y)
}
