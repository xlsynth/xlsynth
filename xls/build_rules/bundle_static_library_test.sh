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

bundle_tool="${TEST_SRCDIR}/_main/$1"
workdir="${TEST_TMPDIR}/bundle_static_library_test"
mkdir -p "${workdir}"
cd "${workdir}"

cat > member.c <<'EOF'
int bundled_member(void) { return 1; }
EOF

cc -c member.c -o member.o
ar rcs 'libmember~with_tilde.a' member.o

"${bundle_tool}" libbundle.a 'libmember~with_tilde.a'
ar t libbundle.a | grep -q 'member.o'
