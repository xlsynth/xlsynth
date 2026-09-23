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
first="${root}/$2"
second="${root}/$3"

check_package() {
  local output="$1"
  grep -Fq 'typedef Duplicate Left;' "${output}"
  grep -Fq 'typedef Duplicate Shared;' "${output}"
  grep -Fq 'typedef Duplicate__1 Right;' "${output}"
  [[ "$(grep -Fc '} Duplicate_tag_t;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} Duplicate__1_tag_t;' "${output}")" == 1 ]]
  grep -Fq 'function automatic Duplicate Duplicate_make_item (input logic [7:0] value);' "${output}"
  grep -Fq 'function automatic Duplicate__1 Duplicate__1_make_item (input logic [15:0] value);' "${output}"
}

"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="$(dirname "${first}")" --output_file="${TEST_TMPDIR}/forward.sv" \
  "${first}" "${second}"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="$(dirname "${first}")" --output_file="${TEST_TMPDIR}/reverse.sv" \
  "${second}" "${first}"
check_package "${TEST_TMPDIR}/forward.sv"
check_package "${TEST_TMPDIR}/reverse.sv"
