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

output="$1"
shift

case "$(uname -s)" in
  Darwin)
    # Darwin's libtool flattens both object files and nested archives into one
    # archive; the platform ar implementation does not support MRI scripts.
    /usr/bin/libtool -static -o "${output}" "$@"
    ;;
  Linux)
    # GNU ar's MRI mode can add archive members without first extracting them,
    # so duplicate object basenames from different inputs cannot collide.
    {
      printf 'CREATE %s\n' "${output}"
      for input in "$@"; do
        case "${input}" in
          *.a | *.lo)
            printf 'ADDLIB %s\n' "${input}"
            ;;
          *)
            printf 'ADDMOD %s\n' "${input}"
            ;;
        esac
      done
      printf 'SAVE\nEND\n'
    } | ar -M
    ;;
  *)
    echo "unsupported static-library bundle platform: $(uname -s)" >&2
    exit 1
    ;;
esac
