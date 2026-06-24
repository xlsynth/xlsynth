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

#![feature(type_inference_v2)]

enum SemanticSumToken {
    Empty,
    Word(u32),
    Pair { lo: u8, hi: u8 },
}

fn payload_or_default(x: SemanticSumToken) -> u32 {
    match x {
        SemanticSumToken::Empty => u32:0,
        SemanticSumToken::Word(value) => value,
        SemanticSumToken::Pair { lo: lo, hi: hi } => lo as u32 + hi as u32,
    }
}

fn tuple_payload_or_default(x: (SemanticSumToken, bool)) -> u32 {
    match x {
        (SemanticSumToken::Word(value), true) => value,
        (SemanticSumToken::Pair { lo: lo, hi: hi }, _) => lo as u32 + hi as u32,
        _ => u32:0,
    }
}
