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

: "${XLS_SV_SLANG:?Set XLS_SV_SLANG to the Slang SystemVerilog compiler executable}"
package="${TEST_SRCDIR}/${TEST_WORKSPACE}/$1"
consumer="${TEST_SRCDIR}/${TEST_WORKSPACE}/$2"

exec "${XLS_SV_SLANG}" --top semantic_sum_name_scope_consumer \
  "${package}" "${consumer}"
