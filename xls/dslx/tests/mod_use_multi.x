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

use xls::dslx::tests::mod_imported::{FALSE, my_lsb, my_lsb_uses_const};

fn main(x: u8) -> (bool, u1, u1) {
    (FALSE, my_lsb(x), my_lsb_uses_const(x))
}

#[test]
fn test_main() {
    let t = main(u8:1);
    assert_eq(t, (false, false, false))
}