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

import xls.dslx.translators.testdata.semantic_sum_pkg;

type Message = semantic_sum_pkg::Message;
type Inner = semantic_sum_pkg::Inner;
type Outer = semantic_sum_pkg::Outer;
type Sparse = semantic_sum_pkg::Sparse;

// Returning and accepting the nominal type makes both raw RTL ports use the
// compiler's sum lowering, without any hand-written DSLX bit packing.
pub fn make_message(selector: u2, hi: u8, lo: u8) -> Message {
    match selector {
        u2:1 => Message::Byte(lo),
        u2:2 => Message::Pair { hi: hi, lo: lo },
        _ => Message::None,
    }
}

pub fn read_message(message: Message) -> u16 {
    match message {
        Message::None => u16:0,
        Message::Byte(value) => value as u16,
        Message::Pair { hi: hi, lo: lo } => hi ++ lo,
    }
}

pub fn make_sparse_and_outer(selector: u2, payload: u8, inner: Inner) -> (Sparse, Outer) {
    let sparse = match selector {
        u2:1 => Sparse::Negative(payload),
        u2:2 => Sparse::Positive(payload),
        _ => Sparse::Empty,
    };
    (sparse, Outer::Wrapped(inner))
}

pub fn read_sparse_and_outer(sparse: Sparse, outer: Outer) -> (u16, Inner, Outer) {
    let sparse_result = match sparse {
        Sparse::Empty => u16:0,
        Sparse::Negative(value) => u8:0xff ++ value,
        Sparse::Positive(value) => u8:2 ++ value,
    };
    let inner = match outer {
        Outer::Wrapped(value) => value,
        _ => Inner::None,
    };
    // Returning the nominal outer unchanged keeps its otherwise unobserved
    // padding visible to the four-state SV consumer.
    (sparse_result, inner, outer)
}
