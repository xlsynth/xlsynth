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

#ifndef XLS_IR_BITS_TEST_UTILS_H_
#define XLS_IR_BITS_TEST_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

//#include "xls/common/fuzzing/fuzztest.h"
#include "absl/log/check.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/ir/bits.h"

namespace xls {

// Create a Bits of the given bit count with the prime number index bits set to
// one.
inline Bits PrimeBits(int64_t bit_count) {
  auto is_prime = [](int64_t n) {
    if (n < 2) {
      return false;
    }
    for (int64_t i = 2; i * i < n; ++i) {
      if (n % i == 0) {
        return false;
      }
    }
    return true;
  };

  InlineBitmap bitmap(bit_count, /*fill=*/false);
  for (int64_t i = 0; i < bit_count; ++i) {
    if (is_prime(i)) {
      bitmap.Set(i, true);
    }
  }
  return Bits::FromBitmap(bitmap);
}

#if 0
// GoogleFuzzTest "Arbitrary" domain for Bits with known count.
inline auto ArbitraryBits(int64_t bit_count) {
  const int64_t byte_count = (bit_count + 7) / 8;
  return fuzztest::ReversibleMap(
      [bit_count](const std::vector<uint8_t>& bytes) {
        return Bits::FromBytes(bytes, bit_count);
      },
      [](const Bits& bits) {
        return std::make_optional(std::make_tuple(bits.ToBytes()));
      },
      fuzztest::Arbitrary<std::vector<uint8_t>>().WithSize(byte_count));
}

// GoogleFuzzTest "Arbitrary" domain for Bits with arbitrary count.
inline auto ArbitraryBits() {
  return fuzztest::ReversibleMap(
      [](const std::vector<uint8_t>& bytes, uint8_t excess_bits) -> Bits {
        if (bytes.empty()) {
          return Bits();
        }
        const int64_t bit_count =
            static_cast<int64_t>(bytes.size()) * 8 - excess_bits;
        return Bits::FromBytes(bytes, bit_count);
      },
      [](const Bits& bits)
          -> std::optional<std::tuple<std::vector<uint8_t>, uint8_t>> {
        const uint8_t overflow_bits = bits.bit_count() % 8;
        return std::make_optional(std::make_tuple(
            bits.ToBytes(), overflow_bits == 0 ? 0 : (8 - overflow_bits)));
      },
      fuzztest::Arbitrary<std::vector<uint8_t>>(),
      fuzztest::InRange<uint8_t>(0, 7));
}

// GoogleFuzzTest domain for nonempty Bits.
//
// If given max_byte_count is the maximum number of bytes (sets of 8 bits) the
// bits object may take up.
inline auto NonemptyBits(std::optional<size_t> max_byte_count = std::nullopt) {
  auto data = fuzztest::Arbitrary<std::vector<uint8_t>>().WithMinSize(1);
  if (max_byte_count) {
    data = data.WithMaxSize(*max_byte_count);
  }
  return fuzztest::ReversibleMap(
      [](const std::vector<uint8_t>& bytes, uint8_t excess_bits) -> Bits {
        CHECK(!bytes.empty());
        const int64_t bit_count =
            static_cast<int64_t>(bytes.size()) * 8 - excess_bits;
        return Bits::FromBytes(bytes, bit_count);
      },
      [](const Bits& bits)
          -> std::optional<std::tuple<std::vector<uint8_t>, uint8_t>> {
        CHECK_NE(bits.bit_count(), 0);
        const uint8_t overflow_bits = bits.bit_count() % 8;
        return std::make_optional(std::make_tuple(
            bits.ToBytes(), overflow_bits == 0 ? 0 : (8 - overflow_bits)));
      },
      std::move(data), fuzztest::InRange<uint8_t>(0, 7));
}
#endif

}  // namespace xls

#endif  // XLS_IR_BITS_TEST_UTILS_H_
