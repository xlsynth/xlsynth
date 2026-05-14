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
    # Older GNU ar releases also treat `~` specially inside MRI scripts. Bazel
    # module repository paths contain that character, so stage each input behind
    # a sanitized unique symlink before writing the script consumed by `ar -M`.
    staging_dir="$(mktemp -d)"
    trap 'rm -rf "${staging_dir}"' EXIT
    staged_inputs=()
    input_index=0
    for input in "$@"; do
      case "${input}" in
        *.a)
          staged_input="${staging_dir}/input_${input_index}.a"
          ;;
        *.lo)
          staged_input="${staging_dir}/input_${input_index}.lo"
          ;;
        *)
          staged_input="${staging_dir}/input_${input_index}.o"
          ;;
      esac
      ln -s "$(pwd)/${input}" "${staged_input}"
      staged_inputs+=("${staged_input}")
      input_index=$((input_index + 1))
    done
    {
      printf 'CREATE %s\n' "${output}"
      for input in "${staged_inputs[@]}"; do
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
