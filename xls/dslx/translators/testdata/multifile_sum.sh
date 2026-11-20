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
