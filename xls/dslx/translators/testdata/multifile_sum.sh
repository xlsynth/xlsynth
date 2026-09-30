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
simulator="${root}/$4"
runtime="${root}/$5"
dslx_search_path="$(dirname "${first}")"

check_imported_package() {
  local output="$1"
  grep -Fq 'typedef sum_input_a_Duplicate Shared;' "${output}"
  grep -Fq 'typedef sum_input_b_Duplicate Right;' "${output}"
  [[ "$(grep -Fc '} sum_input_a_Duplicate_tag_t;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} sum_input_b_Duplicate_tag_t;' "${output}")" == 1 ]]
  grep -Fq 'sum_input_a_Duplicate_make_item (input logic [7:0] value);' "${output}"
  grep -Fq 'sum_input_b_Duplicate_make_item (input logic [15:0] value);' "${output}"
}

# Verifies: aliases share families while distinct sum declarations stay distinct.
# Catches: input or search-path ordering changing identity or generated names.
# Existing stdin input remains usable for an ordinary file and a named import.
printf '%s\n' 'pub type PlainStdin = u8;' |
  "${translator}" --package_name=multi --namespace=multi \
    --output_file="${TEST_TMPDIR}/stdin_plain.sv" -
grep -Fq 'typedef logic [7:0] PlainStdin;' "${TEST_TMPDIR}/stdin_plain.sv"
printf '%s\n' 'pub type DuplicateStdin = u8;' |
  "${translator}" --package_name=multi --namespace=multi \
    --output_file="${TEST_TMPDIR}/stdin_duplicate.sv" - -
[[ "$(grep -Fc 'typedef logic [7:0] DuplicateStdin;' \
  "${TEST_TMPDIR}/stdin_duplicate.sv")" == 1 ]]

printf '%s\n' 'import sum_input_a;' \
  'pub type FromStdin = sum_input_a::Duplicate;' |
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${dslx_search_path}" \
    --output_file="${TEST_TMPDIR}/stdin_import.sv" -
grep -Fq 'typedef Duplicate FromStdin;' "${TEST_TMPDIR}/stdin_import.sv"

# An imported sum and another root's genuinely different same-named sum get
# distinct families; the import and direct alias keep the same first family.
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${dslx_search_path}" --output_file="${TEST_TMPDIR}/imported.sv" \
  "${second}"
check_imported_package "${TEST_TMPDIR}/imported.sv"

"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${dslx_search_path}" --output_file="${TEST_TMPDIR}/forward.sv" \
  "${first}" "${second}"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${dslx_search_path}" --output_file="${TEST_TMPDIR}/reverse.sv" \
  "${second}" "${first}"
for output in "${TEST_TMPDIR}/forward.sv" "${TEST_TMPDIR}/reverse.sv"; do
  check_imported_package "${output}"
  grep -Fq 'typedef sum_input_a_Duplicate Left;' "${output}"
done

# Overlapping search roots offer both a qualified path and the short name used
# by the import. The direct and imported aliases still denote one declaration.
broader_search_path="$(dirname "${dslx_search_path}")"
for search_order in broad_first narrow_first; do
  search_paths="${broader_search_path}:${dslx_search_path}"
  if [[ "${search_order}" == narrow_first ]]; then
    search_paths="${dslx_search_path}:${broader_search_path}"
  fi
  for input_order in forward reverse; do
    inputs=("${first}" "${second}")
    if [[ "${input_order}" == reverse ]]; then
      inputs=("${second}" "${first}")
    fi
    output="${TEST_TMPDIR}/overlap_${search_order}_${input_order}.sv"
    "${translator}" --package_name=multi --namespace=multi \
      --dslx_path="${search_paths}" --output_file="${output}" "${inputs[@]}"
    left="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Left;/\1/p' "${output}")"
    shared="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Shared;/\1/p' "${output}")"
    right="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Right;/\1/p' "${output}")"
    [[ "${left}" =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ && "${left}" == "${shared}" &&
       "${right}" =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ && "${left}" != "${right}" ]]
    [[ "$(grep -Ec '^[[:space:]]*} [^[:space:]]+_tag_t;$' "${output}")" == 2 ]]
    [[ "$(grep -Fc "} ${left}_tag_t;" "${output}")" == 1 ]]
    [[ "$(grep -Fc "} ${right}_tag_t;" "${output}")" == 1 ]]
    grep -Fq "${left}_make_item (input logic [7:0] value);" "${output}"
    grep -Fq "${right}_make_item (input logic [15:0] value);" "${output}"
    {
      printf '%s\n' "${left}" "${shared}" "${right}"
      sed -n 's/^[[:space:]]*\(function automatic.*\)/\1/p' "${output}" |
        LC_ALL=C sort
    } >"${output}.names"
    cmp "${TEST_TMPDIR}/overlap_${search_order}_forward.sv.names" "${output}.names"
  done
done
cmp "${TEST_TMPDIR}/overlap_broad_first_forward.sv.names" \
  "${TEST_TMPDIR}/overlap_narrow_first_forward.sv.names"

# Compile and execute an actual two-root package. Cross-root aliases and both
# widths must be usable by a native SystemVerilog consumer.
cat >"${TEST_TMPDIR}/multi_consumer.sv" <<'EOF'
module multi_consumer;
  import multi::*;
  Left left_value;
  Shared shared_value;
  Right right_value;
  initial begin
    left_value = sum_input_a_Duplicate_make_item(8'h35);
    shared_value = left_value;
    right_value = sum_input_b_Duplicate_make_item(16'h7135);
    if (shared_value.payload.bits !== 8'h35 ||
        right_value.payload.bits !== 16'h7135 ||
        sum_input_a_Duplicate_get_tag(shared_value) !== sum_input_a_Duplicate_tag_Item ||
        sum_input_b_Duplicate_get_tag(right_value) !== sum_input_b_Duplicate_tag_Item)
      $fatal(1, "cross-root sum");
  end
endmodule
EOF
export RUNFILES_DIR="${TEST_SRCDIR}"
"${simulator}" -g2012 -s multi_consumer -o "${TEST_TMPDIR}/multi_consumer" \
  "${TEST_TMPDIR}/forward.sv" "${TEST_TMPDIR}/multi_consumer.sv"
"${runtime}" "${TEST_TMPDIR}/multi_consumer"

# Fully qualified ordinary and member imports distinguish equal basenames while
# aliases to one definition reuse its family, in both command-line root orders.
fixture="${TEST_TMPDIR}/same_basename"
mkdir -p "${fixture}/one" "${fixture}/two"
printf '%s\n' 'pub enum Duplicate { Empty, Item(u8) }' \
  'pub type First = Duplicate;' >"${fixture}/one/shared.x"
printf '%s\n' 'pub enum Duplicate { Empty, Item(u16) }' \
  'pub type Second = Duplicate;' >"${fixture}/two/shared.x"
printf '%s\n' 'import one.shared;' 'import two.shared as other;' \
  'pub type Imported = shared::Duplicate;' \
  'pub type OtherImported = other::Duplicate;' >"${fixture}/owner.x"
printf '%s\n' '#![feature(use_syntax)]' 'use one::shared::First;' \
  'use two::shared::Duplicate;' 'pub type MemberFirst = First;' \
  'pub type Used = Duplicate;' >"${fixture}/use_owner.x"

"${translator}" --package_name=multi --namespace=multi --dslx_path="${fixture}" \
  --output_file="${TEST_TMPDIR}/qualified_forward.sv" \
  "${fixture}/one/shared.x" "${fixture}/two/shared.x" \
  "${fixture}/owner.x" "${fixture}/use_owner.x"
"${translator}" --package_name=multi --namespace=multi --dslx_path="${fixture}" \
  --output_file="${TEST_TMPDIR}/qualified_reverse.sv" \
  "${fixture}/use_owner.x" "${fixture}/owner.x" \
  "${fixture}/two/shared.x" "${fixture}/one/shared.x"
for output in "${TEST_TMPDIR}/qualified_forward.sv" \
              "${TEST_TMPDIR}/qualified_reverse.sv"; do
  grep -Fq 'typedef one_shared_Duplicate First;' "${output}"
  grep -Fq 'typedef one_shared_Duplicate Imported;' "${output}"
  grep -Fq 'typedef two_shared_Duplicate Second;' "${output}"
  grep -Fq 'typedef two_shared_Duplicate OtherImported;' "${output}"
  grep -Fq 'typedef two_shared_Duplicate Used;' "${output}"
  [[ "$(grep -Fc '} one_shared_Duplicate_tag_t;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} two_shared_Duplicate_tag_t;' "${output}")" == 1 ]]
done

# Conversely, the source can use the longer module spelling while an explicit
# path has a shorter spelling through the first search root.
printf '%s\n' 'import one.shared;' 'pub type FromLong = shared::Duplicate;' \
  >"${fixture}/long_only.x"
for order in forward reverse; do
  inputs=("${fixture}/one/shared.x" "${fixture}/long_only.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${inputs[1]}" "${inputs[0]}")
  fi
  output="${TEST_TMPDIR}/long_import_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}/one:${fixture}" --output_file="${output}" \
    "${inputs[@]}"
  first_family="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) First;/\1/p' "${output}")"
  [[ -n "${first_family}" ]]
  grep -Fq "typedef ${first_family} FromLong;" "${output}"
  [[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 1 ]]
done

# Adding only a client that imports an alternate spelling must not rename the
# existing families of either owner, in either explicit order.
for client in without_client with_client; do
  for order in forward reverse; do
    inputs=("${fixture}/one/shared.x" "${fixture}/two/shared.x")
    if [[ "${order}" == reverse ]]; then
      inputs=("${inputs[1]}" "${inputs[0]}")
    fi
    if [[ "${client}" == with_client ]]; then
      inputs+=("${fixture}/long_only.x")
    fi
    output="${TEST_TMPDIR}/alias_identity_${client}_${order}.sv"
    "${translator}" --package_name=multi --namespace=multi \
      --dslx_path="${fixture}/one:${fixture}" --output_file="${output}" \
      "${inputs[@]}"
    first_family="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) First;/\1/p' "${output}")"
    second_family="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Second;/\1/p' "${output}")"
    [[ -n "${first_family}" && -n "${second_family}" &&
       "${first_family}" != "${second_family}" ]]
    if [[ "${client}" == with_client ]]; then
      grep -Fq "typedef ${first_family} FromLong;" "${output}"
    fi
    printf '%s\n' "${first_family}" "${second_family}" >"${output}.names"
    cmp "${TEST_TMPDIR}/alias_identity_without_client_forward.sv.names" \
      "${output}.names"
  done
done

# Hash fallback includes both the defining sum and nested ordinary nominal type
# arguments. A client that changes the cached import spelling cannot change
# either existing hashed family or create a second family for its aliases.
fixture="${TEST_TMPDIR}/hashed_alias_identity"
mkdir -p "${fixture}/one"
printf '%s\n' '#![feature(generics)]' 'pub struct Argument { value: u8 }' \
  'pub enum Mode: u2 { Value = 1 }' \
  'enum Hashed<T: type, N: uN[256]> { Item(u8) }' \
  'pub type DirectRecord = Hashed<Argument, uN[256]:1>;' \
  'pub type DirectEnum = Hashed<Mode, uN[256]:1>;' >"${fixture}/one/shared.x"
printf '%s\n' 'import one.shared;' \
  'pub type ImportedRecord = shared::DirectRecord;' \
  'pub type ImportedEnum = shared::DirectEnum;' >"${fixture}/client.x"
for order in without_client direct_first client_first; do
  inputs=("${fixture}/one/shared.x")
  if [[ "${order}" == direct_first ]]; then
    inputs+=("${fixture}/client.x")
  elif [[ "${order}" == client_first ]]; then
    inputs=("${fixture}/client.x" "${inputs[0]}")
  fi
  output="${TEST_TMPDIR}/hashed_alias_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}/one:${fixture}" --output_file="${output}" \
    "${inputs[@]}"
  record="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) DirectRecord;/\1/p' "${output}")"
  ordinary_enum="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) DirectEnum;/\1/p' "${output}")"
  [[ "${record}" =~ ^Hashed__h[0-9a-f]{64}$ &&
     "${ordinary_enum}" =~ ^Hashed__h[0-9a-f]{64}$ &&
     "${record}" != "${ordinary_enum}" ]]
  [[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 2 ]]
  if [[ "${order}" != without_client ]]; then
    grep -Fq "typedef ${record} ImportedRecord;" "${output}"
    grep -Fq "typedef ${ordinary_enum} ImportedEnum;" "${output}"
  fi
  printf '%s\n' "${record}" "${ordinary_enum}" >"${output}.names"
  cmp "${TEST_TMPDIR}/hashed_alias_without_client.sv.names" "${output}.names"
done

# Filesystem identity follows symlinks before '..': an imported symlink and
# its explicit target are one declaration, while link/../shared.x and the
# lexical neighbor are distinct declarations with different payloads.
fixture="${TEST_TMPDIR}/physical_identity"
mkdir -p "${fixture}/real/child" "${fixture}/logical/search"
printf '%s\n' 'pub enum Packet { Empty, Physical(u16) }' \
  'pub type Physical = Packet;' >"${fixture}/real/shared.x"
printf '%s\n' 'pub enum Packet { Empty, Lexical(u8) }' \
  'pub type Lexical = Packet;' >"${fixture}/logical/shared.x"
ln -s "${fixture}/real/child" "${fixture}/logical/link"
ln -s "${fixture}/real/shared.x" "${fixture}/logical/search/shared.x"
ln -s "${fixture}/real" "${fixture}/directory_link"
printf '%s\n' 'import shared;' 'pub type Imported = shared::Packet;' \
  >"${fixture}/logical/search/client.x"
printf '%s\n' 'pub enum Nested { Empty, Item(u32) }' \
  'pub type NestedDirect = Nested;' >"${fixture}/real/child/nested.x"
printf '%s\n' 'import child.nested;' \
  'pub type NestedImported = nested::Nested;' \
  >"${fixture}/logical/search/nested_client.x"
for order in forward reverse; do
  paths=("${fixture}/real/shared.x" "${fixture}/logical/search/client.x")
  neighbors=("${fixture}/logical/shared.x" "${fixture}/logical/link/../shared.x")
  if [[ "${order}" == reverse ]]; then
    paths=("${paths[1]}" "${paths[0]}")
    neighbors=("${neighbors[1]}" "${neighbors[0]}")
  fi
  output="${TEST_TMPDIR}/physical_symlink_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}/logical/search" --output_file="${output}" \
    "${paths[@]}"
  grep -Fq 'typedef Packet Physical;' "${output}"
  grep -Fq 'typedef Packet Imported;' "${output}"
  [[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 1 ]]

  output="${TEST_TMPDIR}/physical_dotdot_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --output_file="${output}" "${neighbors[@]}"
  physical="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Physical;/\1/p' "${output}")"
  lexical="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Lexical;/\1/p' "${output}")"
  [[ -n "${physical}" && -n "${lexical}" && "${physical}" != "${lexical}" ]]
  grep -Fq "${physical}_make_physical (input logic [15:0] value);" "${output}"
  grep -Fq "${lexical}_make_lexical (input logic [7:0] value);" "${output}"
  [[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 2 ]]
  printf '%s\n' "${physical}" "${lexical}" >"${output}.names"
  cmp "${TEST_TMPDIR}/physical_dotdot_forward.sv.names" "${output}.names"
done

output="${TEST_TMPDIR}/physical_duplicates.sv"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${fixture}/directory_link" --output_file="${output}" \
  "${fixture}/real/child/nested.x" "${fixture}/directory_link/child/nested.x" \
  "${fixture}/logical/search/nested_client.x"
grep -Fq 'typedef Nested NestedDirect;' "${output}"
grep -Fq 'typedef Nested NestedImported;' "${output}"
[[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 1 ]]
for order in forward reverse; do
  inputs=("${fixture}/real/shared.x" "${fixture}/missing.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${inputs[1]}" "${inputs[0]}")
  fi
  if "${translator}" --package_name=multi --namespace=multi \
      --output_file="${TEST_TMPDIR}/missing_${order}.sv" "${inputs[@]}" \
      >"${TEST_TMPDIR}/missing_${order}.log" 2>&1; then
    echo "Missing explicit source unexpectedly succeeded: ${order}" >&2
    exit 1
  fi
  grep -Fq 'missing.x' "${TEST_TMPDIR}/missing_${order}.log"
done

# Two non-importable input spellings can also collide after ordinary cache-name
# encoding; their distinct declarations must not disappear from the output.
printf '%s\n' 'pub enum Odd { First(u8) }' 'pub type OddFirst = Odd;' \
  >"${fixture}/foo"
printf '%s\n' 'pub enum Odd { Second(u16) }' 'pub type OddSecond = Odd;' \
  >"${fixture}/foo..x"
output="${TEST_TMPDIR}/physical_odd_names.sv"
"${translator}" --package_name=multi --namespace=multi --output_file="${output}" \
  "${fixture}/foo" "${fixture}/foo..x"
grep -Eq 'typedef .* OddFirst;' "${output}"
grep -Eq 'typedef .* OddSecond;' "${output}"
[[ "$(grep -Ec '^  } .*_tag_t;$' "${output}")" == 2 ]]

# Unrelated, empty roots with the same filename cannot extend a standalone
# sum owner's public qualifier; only actual colliding declarations matter.
fixture="${TEST_TMPDIR}/empty_identity"
mkdir -p "${fixture}/a/one" "${fixture}/b/two" "${fixture}/c/one"
printf '%s\n' 'pub enum Packet { Empty, Item(u8) }' \
  'pub type First = Packet;' >"${fixture}/a/one/shared.x"
printf '%s\n' 'pub enum Packet { Empty, Item(u16) }' \
  'pub type Second = Packet;' >"${fixture}/b/two/shared.x"
: >"${fixture}/c/one/shared.x"
for added in without_empty with_empty; do
  inputs=("${fixture}/a/one/shared.x" "${fixture}/b/two/shared.x")
  if [[ "${added}" == with_empty ]]; then
    inputs+=("${fixture}/c/one/shared.x")
  fi
  output="${TEST_TMPDIR}/empty_identity_${added}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --output_file="${output}" "${inputs[@]}"
  sed -n 's/^[[:space:]]*typedef \([^ ]*\) \(First\|Second\);/\2=\1/p' \
    "${output}" | LC_ALL=C sort >"${output}.names"
  [[ "$(wc -l <"${output}.names")" -eq 2 ]]
  cmp "${TEST_TMPDIR}/empty_identity_without_empty.sv.names" "${output}.names"
done

# The only explicit exporting root is an alias-only client. Its actual sum
# owners are two imports away, and an empty root must not change their public
# names even when its module name sanitizes to the first owner's qualifier.
fixture="${TEST_TMPDIR}/transitive_empty_identity"
mkdir -p "${fixture}/a"
printf '%s\n' 'pub enum Packet { Empty, Item(u8) }' >"${fixture}/a/b.x"
printf '%s\n' 'pub enum Packet { Empty, Item(u16) }' >"${fixture}/c.x"
printf '%s\n' 'import a.b;' 'import c;' \
  'pub type FromA = b::Packet;' 'pub type FromC = c::Packet;' \
  >"${fixture}/bridge.x"
printf '%s\n' 'import bridge;' 'pub type First = bridge::FromA;' \
  'pub type Second = bridge::FromC;' 'pub type Ordinary = u4;' \
  >"${fixture}/client.x"
: >"${fixture}/a_b.x"
for order in without_empty client_first empty_first; do
  inputs=("${fixture}/client.x")
  if [[ "${order}" == client_first ]]; then
    inputs+=("${fixture}/a_b.x")
  elif [[ "${order}" == empty_first ]]; then
    inputs=("${fixture}/a_b.x" "${inputs[0]}")
  fi
  output="${TEST_TMPDIR}/transitive_empty_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  cmp "${TEST_TMPDIR}/transitive_empty_without_empty.sv" "${output}"
  grep -Fq 'typedef a_b_Packet First;' "${output}"
  grep -Fq 'typedef c_Packet Second;' "${output}"
  grep -Fq 'typedef logic [3:0] Ordinary;' "${output}"
  [[ "$(grep -Fc '} a_b_Packet_tag_t;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} c_Packet_tag_t;' "${output}")" == 1 ]]
  grep -Fq 'a_b_Packet_make_item (input logic [7:0] value);' "${output}"
  grep -Fq 'c_Packet_make_item (input logic [15:0] value);' "${output}"
done

# A real __shared import and the standalone shared.x root must stay distinct,
# even if the standalone root is seen before the importing client. The public
# names must also stay legal and stable when the fixture is moved.
for layout in identity_original identity_relocated; do
  fixture="${TEST_TMPDIR}/${layout}"
  mkdir -p "${fixture}/search" "${fixture}/outside"
  printf '%s\n' 'pub enum Duplicate { Empty, Item(u8) }' \
    'pub type First = Duplicate;' >"${fixture}/search/__shared.x"
  printf '%s\n' 'pub enum Duplicate { Empty, Item(u16) }' \
    'pub type Second = Duplicate;' >"${fixture}/outside/shared.x"
  printf '%s\n' 'import __shared;' 'pub type Imported = __shared::Duplicate;' \
    >"${fixture}/search/client.x"
  for client in without_client with_client; do
    for order in named_first standalone_first; do
      inputs=("${fixture}/search/__shared.x" "${fixture}/outside/shared.x")
      if [[ "${order}" == standalone_first ]]; then
        inputs=("${fixture}/outside/shared.x" "${fixture}/search/__shared.x")
      fi
      if [[ "${client}" == with_client ]]; then
        inputs=("${inputs[0]}" "${fixture}/search/client.x" "${inputs[1]}")
      fi
      output="${TEST_TMPDIR}/${layout}_${client}_${order}.sv"
      "${translator}" --package_name=multi --namespace=multi \
        --dslx_path="${fixture}/search" --output_file="${output}" "${inputs[@]}"
      named="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) First;/\1/p' "${output}")"
      standalone="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Second;/\1/p' "${output}")"
      [[ "${named}" =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ &&
         "${standalone}" =~ ^[a-zA-Z_][a-zA-Z_0-9]*$ &&
         "${named}" != "${standalone}" ]]
      [[ "$(grep -Fc "} ${named}_tag_t;" "${output}")" == 1 ]]
      [[ "$(grep -Fc "} ${standalone}_tag_t;" "${output}")" == 1 ]]
      grep -Fq "${named}_make_item (input logic [7:0] value);" "${output}"
      grep -Fq "${standalone}_make_item (input logic [15:0] value);" "${output}"
      if [[ "${client}" == with_client ]]; then
        grep -Fq "typedef ${named} Imported;" "${output}"
      fi
      printf '%s\n' "${named}" "${standalone}" >"${output}.names"
      cmp "${TEST_TMPDIR}/identity_original_without_client_named_first.sv.names" \
        "${output}.names"
    done
  done
done

# Standard input also has its own cache entry, separate from named and
# standalone files whose names resemble the internal stdin module name.
fixture="${TEST_TMPDIR}/identity_original"
printf '%s\n' 'pub enum NamedInput { Empty, Item(u8) }' \
  >"${fixture}/search/__stdin.x"
printf '%s\n' 'pub enum StandaloneInput { Empty, Item(u16) }' \
  >"${fixture}/outside/stdin.x"
printf '%s\n' 'import __stdin;' 'pub type ImportedInput = __stdin::NamedInput;' \
  'pub enum StreamInput { Empty, Item(u32) }' |
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}/search" --output_file="${TEST_TMPDIR}/identity_stdin.sv" \
    "${fixture}/outside/stdin.x" - "${fixture}/search/__stdin.x"
output="${TEST_TMPDIR}/identity_stdin.sv"
grep -Fq 'NamedInput_make_item (input logic [7:0] value);' "${output}"
grep -Fq 'StandaloneInput_make_item (input logic [15:0] value);' "${output}"
grep -Fq 'StreamInput_make_item (input logic [31:0] value);' "${output}"
grep -Fq 'typedef NamedInput ImportedInput;' "${output}"

# An explicitly supplied shadowed file must not replace the search-path winner
# imported by another file. The generated families must not depend on the
# absolute directory in which this same input layout lives.
for layout in original relocated; do
  fixture="${TEST_TMPDIR}/${layout}"
  mkdir -p "${fixture}/one" "${fixture}/two"
  printf '%s\n' 'pub enum Packet { Empty, Item(u8) }' >"${fixture}/one/same.x"
  printf '%s\n' 'pub enum Packet { Empty, Item(u16) }' \
    'pub type Standalone = Packet;' >"${fixture}/two/same.x"
  printf '%s\n' 'import same;' 'pub type Imported = same::Packet;' \
    >"${fixture}/one/user.x"
  for order in explicit_first import_first; do
    inputs=("${fixture}/two/same.x" "${fixture}/one/user.x")
    if [[ "${order}" == import_first ]]; then
      inputs=("${fixture}/one/user.x" "${fixture}/two/same.x")
    fi
    output="${TEST_TMPDIR}/${layout}_${order}.sv"
    "${translator}" --package_name=multi --namespace=multi \
      --dslx_path="${fixture}/one:${fixture}/two" \
      --output_file="${output}" "${inputs[@]}"
    imported="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Imported;/\1/p' "${output}")"
    standalone="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Standalone;/\1/p' "${output}")"
    [[ -n "${imported}" && -n "${standalone}" && "${imported}" != "${standalone}" ]]
    grep -Fq "${imported}_make_item (input logic [7:0] value);" "${output}"
    grep -Fq "${standalone}_make_item (input logic [15:0] value);" "${output}"
    printf '%s\n' "${imported}" "${standalone}" >"${output}.names"
    cmp "${TEST_TMPDIR}/original_explicit_first.sv.names" "${output}.names"
  done
done
