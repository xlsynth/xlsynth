// Copyright 2026 The XLS Authors
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

pub enum PlainCode: u2 { A = 0, B = 1 }
pub struct PlainRecord { field: u8 }

pub enum Leaf { None, Number(u8) }

pub enum Packed {
  None,
  Scalar(s8),
  Array(s8[2]),
  Tuple((s8, u4)),
  Nested(Leaf),
  Code(PlainCode),
  Record(PlainRecord),
}
