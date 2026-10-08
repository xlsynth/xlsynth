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

package="${TEST_SRCDIR}/${TEST_WORKSPACE}/$1"
consumer="${TEST_SRCDIR}/${TEST_WORKSPACE}/$2"
producer="${TEST_SRCDIR}/${TEST_WORKSPACE}/$3"
matcher="${TEST_SRCDIR}/${TEST_WORKSPACE}/$4"
sparse_outer_producer="${TEST_SRCDIR}/${TEST_WORKSPACE}/$5"
sparse_outer_matcher="${TEST_SRCDIR}/${TEST_WORKSPACE}/$6"
compiler_options=(-g2012)
if (( $# == 8 )); then
  simulator="${TEST_SRCDIR}/${TEST_WORKSPACE}/$7"
  runtime="${TEST_SRCDIR}/${TEST_WORKSPACE}/$8"
  compiler_options+=(
    -DXLS_ICARUS_UNSIGNED_ENUM_VIEW_CAST
    -DXLS_ICARUS_NESTED_PACKED_SIGNEDNESS
  )
  export RUNFILES_DIR="${TEST_SRCDIR}"
else
  simulator="${XLS_SV_SIMULATOR:-vcs}"
  runtime="${XLS_SV_VVP:-vvp}"
fi
cd "${TEST_TMPDIR}"

case "${simulator##*/}" in
  vcs)
    "${simulator}" -full64 -sverilog -top semantic_sum_consumer \
      -o "${TEST_TMPDIR}/consumer" "${package}" "${consumer}" \
      "${producer}" "${matcher}" "${sparse_outer_producer}" \
      "${sparse_outer_matcher}"
    "${TEST_TMPDIR}/consumer"
    ;;
  iverilog)
    "${simulator}" "${compiler_options[@]}" -s semantic_sum_consumer \
      -o "${TEST_TMPDIR}/consumer" "${package}" "${consumer}" \
      "${producer}" "${matcher}" "${sparse_outer_producer}" \
      "${sparse_outer_matcher}"
    "${runtime}" "${TEST_TMPDIR}/consumer"
    ;;
  *)
    echo "Unsupported four-state SystemVerilog simulator: ${simulator}" >&2
    exit 1
    ;;
esac
