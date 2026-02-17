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

pub type BinaryOpOpcodeRaw = u8;

pub type MovEtcOpcodeRaw = u8;
pub enum MovEtcOpcode : MovEtcOpcodeRaw {
    MOV = 0,
}

pub type SfuOpOpcodeRaw = u7;
pub enum SfuOpOpcode : SfuOpOpcodeRaw {
    SFU = 0,
}

pub type LargeImmOpcodeRaw = u6;
pub type AtomicOpcodeRaw = u5;
pub type ShflPatternRaw = u3;
pub type BinaryWithCarryOpKindRaw = u4;

pub const EXPECTED_OPCODE_RAW_BITS = u32:8;
