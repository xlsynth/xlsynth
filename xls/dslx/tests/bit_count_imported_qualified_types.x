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

import std;
import xls.dslx.tests.mod_bit_count_type_targets;

const OPCODE_RAW_BITS = std::max(
  bit_count<mod_bit_count_type_targets::BinaryOpOpcodeRaw>(),
  std::max(
    bit_count<mod_bit_count_type_targets::MovEtcOpcode>(),
    std::max(
      bit_count<mod_bit_count_type_targets::SfuOpOpcode>(),
      std::max(
        bit_count<mod_bit_count_type_targets::LargeImmOpcodeRaw>(),
        std::max(
          bit_count<mod_bit_count_type_targets::AtomicOpcodeRaw>(),
          std::max(
            bit_count<mod_bit_count_type_targets::ShflPatternRaw>(),
            bit_count<mod_bit_count_type_targets::BinaryWithCarryOpKindRaw>(),
          ),
        ),
      ),
    ),
  ),
);

fn main() -> u32 {
  OPCODE_RAW_BITS
}

#[test]
fn opcode_raw_bits_matches_expected() {
  assert_eq(OPCODE_RAW_BITS, mod_bit_count_type_targets::EXPECTED_OPCODE_RAW_BITS);
}
