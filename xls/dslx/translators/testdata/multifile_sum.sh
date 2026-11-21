#!/usr/bin/env bash
# Copyright 2026 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
translator="${root}/$1"
fixture="${TEST_TMPDIR}/ordinary_imports"
mkdir -p "${fixture}/one" "${fixture}/two"
printf '%s\n' 'pub type B = u8;' >"${fixture}/b.x"
printf '%s\n' 'import b;' 'pub type A = b::B;' >"${fixture}/a.x"

# An explicit root may already be cached because an earlier root imported it.
for order in forward reverse; do
  inputs=("${fixture}/a.x" "${fixture}/b.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/b.x" "${fixture}/a.x")
  fi
  output="${TEST_TMPDIR}/ordinary_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  grep -Eq 'typedef .* A;' "${output}"
  grep -Eq 'typedef .* B;' "${output}"
done

# Overlapping import roots can give one source both `shared` and `pkg.shared`
# spellings. Explicit selection must use the imported nominal declaration.
mkdir -p "${fixture}/pkg"
printf '%s\n' '#![feature(generics)]' \
  'pub enum Box<T: type> { Empty, Item(T) }' \
  'pub type ByteBox = Box<u8>;' >"${fixture}/pkg/shared.x"
printf '%s\n' '#![feature(generics)]' 'import pkg.shared;' \
  'pub type Imported = shared::ByteBox;' >"${fixture}/pkg/client.x"
output="${TEST_TMPDIR}/overlapping_roots.sv"
(cd "${fixture}" && "${translator}" --package_name=multi --namespace=multi \
  --dslx_path=pkg --output_file="${output}" pkg/shared.x pkg/client.x)
direct="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) ByteBox;/\1/p' "${output}")"
imported="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Imported;/\1/p' "${output}")"
[[ -n "${direct}" && "${direct}" == "${imported}" ]]
[[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 1 ]]

# A single explicit root keeps its historical basename in hashed public names.
printf '%s\n' '#![feature(generics)]' \
  'pub enum Hashed<N: uN[256]> { Empty, Item(u8) }' \
  'pub type Concrete = Hashed<uN[256]:1>;' >"${fixture}/pkg/types.x"
output="${TEST_TMPDIR}/single_hashed.sv"
(cd "${fixture}" && "${translator}" --namespace=single \
  --output_file="${output}" pkg/types.x)
grep -Fq 'typedef Hashed__h46a5a0e92adad9a46ead0801aa4490047d635e88f83c3021a840481b172c472e Concrete;' "${output}"

# Different spellings of the same source must select one stable hash owner.
ln -s pkg/types.x "${fixture}/1a.x"
ln -s pkg/types.x "${fixture}/2b.x"
(cd "${fixture}" && "${translator}" --package_name=single --namespace=single \
  --output_file="${TEST_TMPDIR}/duplicate_forward.sv" 1a.x 2b.x)
(cd "${fixture}" && "${translator}" --package_name=single --namespace=single \
  --output_file="${TEST_TMPDIR}/duplicate_reverse.sv" 2b.x 1a.x)
cmp "${TEST_TMPDIR}/duplicate_forward.sv" "${TEST_TMPDIR}/duplicate_reverse.sv"

# A projected explicit owner must stay distinct from an imported-only owner.
mkdir -p "${fixture}/other"
printf '%s\n' 'pub struct Thing { value: u8 }' >"${fixture}/pkg/types.x"
printf '%s\n' 'pub struct Thing { value: u16 }' >"${fixture}/other/types.x"
printf '%s\n' '#![feature(generics)]' 'import pkg.types as pkg_types;' \
  'import types as other_types;' 'enum Outer<T: type> { Item(T) }' \
  'pub type P = Outer<pkg_types::Thing>;' \
  'pub type Q = Outer<other_types::Thing>;' >"${fixture}/client.x"
output="${TEST_TMPDIR}/distinct_nominal_owners.sv"
(cd "${fixture}" && "${translator}" --package_name=multi --dslx_path=other \
  --output_file="${output}" pkg/types.x client.x)
p="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) P;/\1/p' "${output}")"
q="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Q;/\1/p' "${output}")"
[[ -n "${p}" && -n "${q}" && "${p}" != "${q}" ]]

# A cached import and a different explicit file with its basename remain distinct.
printf '%s\n' 'pub type Left = u8;' >"${fixture}/one/shared.x"
printf '%s\n' 'pub type Right = u16;' >"${fixture}/two/shared.x"
printf '%s\n' 'import shared;' 'pub type Imported = shared::Left;' \
  >"${fixture}/client.x"
for order in forward reverse; do
  inputs=("${fixture}/client.x" "${fixture}/two/shared.x" "${fixture}/one/shared.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/one/shared.x" "${fixture}/two/shared.x" "${fixture}/client.x")
  fi
  output="${TEST_TMPDIR}/same_basename_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}/one" --output_file="${output}" "${inputs[@]}"
  grep -Eq 'typedef .* Left;' "${output}"
  grep -Eq 'typedef .* Right;' "${output}"
  grep -Eq 'typedef .* Imported;' "${output}"
done
