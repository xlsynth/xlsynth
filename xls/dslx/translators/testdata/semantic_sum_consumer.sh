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
simulator="${XLS_SV_SIMULATOR:-vcs}"
cd "${TEST_TMPDIR}"

case "${simulator##*/}" in
  vcs)
    "${simulator}" -full64 -sverilog -top semantic_sum_consumer \
      -o "${TEST_TMPDIR}/consumer" "${package}" "${consumer}"
    "${TEST_TMPDIR}/consumer"
    ;;
  iverilog)
    "${simulator}" -g2012 -s semantic_sum_consumer \
      -o "${TEST_TMPDIR}/consumer" "${package}" "${consumer}"
    "${XLS_SV_VVP:-vvp}" "${TEST_TMPDIR}/consumer"
    ;;
  *)
    echo "Unsupported four-state SystemVerilog simulator: ${simulator}" >&2
    exit 1
    ;;
esac
