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

# A same-name re-export denotes the original nominal sum. Selecting only the
# client or changing root order must preserve the direct export's public API.
fixture="${TEST_TMPDIR}/event_reexport"
mkdir -p "${fixture}"
printf '%s\n' 'pub enum Event { Empty, Item(u8) }' >"${fixture}/shared.x"
printf '%s\n' 'import shared;' 'pub type Event = shared::Event;' \
  'pub type Forward = shared::Event;' >"${fixture}/consumer.x"
for order in direct consumer forward reverse; do
  inputs=("${fixture}/shared.x")
  if [[ "${order}" == consumer ]]; then
    inputs=("${fixture}/consumer.x")
  elif [[ "${order}" == forward ]]; then
    inputs+=("${fixture}/consumer.x")
  elif [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/consumer.x" "${fixture}/shared.x")
  fi
  output="${TEST_TMPDIR}/event_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  [[ "$(grep -Fc '} Event;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} Event_tag_t;' "${output}")" == 1 ]]
  [[ "$(grep -Fc 'Event_make_item (input logic [7:0] value);' "${output}")" == 1 ]]
  if [[ "${order}" != direct ]]; then
    [[ "$(grep -Fc 'typedef Event Forward;' "${output}")" == 1 ]]
    [[ "$(grep -Ec '^[[:space:]]*typedef [^ ]+ Event;$' "${output}" || true)" == 0 ]]
  fi
done

# A single explicit root keeps its historical basename in hashed public names.
single_fixture="${TEST_TMPDIR}/single_hashed_owner"
mkdir -p "${single_fixture}/pkg"
printf '%s\n' '#![feature(generics)]' \
  'pub enum Hashed<N: uN[256]> { Empty, Item(u8) }' \
  'pub type Concrete = Hashed<uN[256]:1>;' >"${single_fixture}/pkg/types.x"
output="${TEST_TMPDIR}/single_hashed.sv"
(cd "${single_fixture}" && "${translator}" --namespace=single \
  --output_file="${output}" pkg/types.x)
grep -Fq 'typedef Hashed__h46a5a0e92adad9a46ead0801aa4490047d635e88f83c3021a840481b172c472e Concrete;' "${output}"

# Different spellings of the same source must select one stable hash owner.
ln -s pkg/types.x "${single_fixture}/1a.x"
ln -s pkg/types.x "${single_fixture}/2b.x"
(cd "${single_fixture}" && "${translator}" --package_name=single --namespace=single \
  --output_file="${TEST_TMPDIR}/duplicate_forward.sv" 1a.x 2b.x)
(cd "${single_fixture}" && "${translator}" --package_name=single --namespace=single \
  --output_file="${TEST_TMPDIR}/duplicate_reverse.sv" 2b.x 1a.x)
cmp "${TEST_TMPDIR}/duplicate_forward.sv" "${TEST_TMPDIR}/duplicate_reverse.sv"

# A projected explicit owner must stay distinct from an imported-only owner.
mkdir -p "${single_fixture}/other"
printf '%s\n' 'pub struct Thing { value: u8 }' >"${single_fixture}/pkg/types.x"
printf '%s\n' 'pub struct Thing { value: u16 }' >"${single_fixture}/other/types.x"
printf '%s\n' '#![feature(generics)]' 'import pkg.types as pkg_types;' \
  'import types as other_types;' 'enum Outer<T: type> { Item(T) }' \
  'pub type P = Outer<pkg_types::Thing>;' \
  'pub type Q = Outer<other_types::Thing>;' >"${single_fixture}/client.x"
output="${TEST_TMPDIR}/distinct_nominal_owners.sv"
(cd "${single_fixture}" && "${translator}" --package_name=multi --dslx_path=other \
  --output_file="${output}" pkg/types.x client.x)
p="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) P;/\1/p' "${output}")"
q="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Q;/\1/p' "${output}")"
[[ -n "${p}" && -n "${q}" && "${p}" != "${q}" ]]

# Companion record hashes must use the legacy owner during family planning.
printf '%s\n' '#![feature(generics)]' \
  'pub struct Record<N: uN[256]> { value: u8 }' \
  'pub enum Envelope { Item(Record<uN[256]:1>) }' >"${single_fixture}/pkg/types.x"
output="${TEST_TMPDIR}/hashed_companion.sv"
(cd "${single_fixture}" && "${translator}" --namespace=single \
  --output_file="${output}" pkg/types.x)
grep -Fq 'Envelope_Record__h3596e4cd5408969976360bbf9cca0651bb613891f5dc88d6285ed0023cd3cf7a_value_t' "${output}"

# Numeric suffixes are part of the source spelling. A same-name re-export
# preserves the nominal family, and distinct fixed aliases remain distinct.
printf '%s\n' 'pub enum Event__01 { Empty, Item(u8) }' \
  'pub enum First { Empty, Item(u8) }' \
  'pub enum Second { Empty, Item(u16) }' >"${fixture}/numeric_shared.x"
printf '%s\n' 'import numeric_shared;' \
  'pub type Event__01 = numeric_shared::Event__01;' >"${fixture}/numeric_same.x"
printf '%s\n' 'import numeric_shared;' \
  'pub type Slot__01 = numeric_shared::First;' \
  'pub type Slot__1 = numeric_shared::Second;' >"${fixture}/numeric_consumer.x"
for mode in direct consumer; do
  output="${TEST_TMPDIR}/numeric_${mode}.sv"
  input="${fixture}/numeric_shared.x"
  if [[ "${mode}" == consumer ]]; then
    input="${fixture}/numeric_same.x"
  fi
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" \
    "${input}"
  [[ "$(grep -Fc '} Event__1;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} Event__1_tag_t;' "${output}")" == 1 ]]
  grep -Fq 'Event__1_make_item (input logic [7:0] value);' "${output}"
  if [[ "${mode}" == consumer ]]; then
    grep -Fq 'typedef Event__1 Event__01;' "${output}"
  fi
done
output="${TEST_TMPDIR}/numeric_aliases.sv"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${fixture}" --output_file="${output}" \
  "${fixture}/numeric_consumer.x"
grep -Fq 'typedef First Slot__01;' "${output}"
grep -Fq 'typedef Second Slot__1;' "${output}"

# Normalization must still respect ordinary source names in the prepared graph.
printf '%s\n' 'pub enum Event__01 { Empty, Item(u8) }' \
  'type Event__1 = u8;' >"${fixture}/numeric_guard.x"
printf '%s\n' 'import numeric_guard;' \
  'pub type Event__1 = numeric_guard::Event__01;' \
  >"${fixture}/numeric_guard_consumer.x"
for mode in direct consumer; do
  input="${fixture}/numeric_guard.x"
  if [[ "${mode}" == consumer ]]; then
    input="${fixture}/numeric_guard_consumer.x"
  fi
  output="${TEST_TMPDIR}/numeric_guard_${mode}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${input}"
  sed -n 's/^[[:space:]]*function automatic [^ ]* \([^ ]*_make_item\) (.*/\1/p' \
    "${output}" >"${output}.constructor"
  [[ -s "${output}.constructor" ]]
done
cmp "${TEST_TMPDIR}/numeric_guard_direct.sv.constructor" \
  "${TEST_TMPDIR}/numeric_guard_consumer.sv.constructor"
grep -Eq 'typedef [^ ]+ Event__1;' "${TEST_TMPDIR}/numeric_guard_consumer.sv"

# A concrete generic same-name alias must retain the direct specialization's
# family through the CLI's selected-export reservation path.
printf '%s\n' '#![feature(generics)]' \
  'pub enum Event<N: u32> { Empty, Item(uN[N]) }' \
  'pub type Direct = Event<u32:8>;' >"${fixture}/generic_shared.x"
printf '%s\n' '#![feature(generics)]' 'import generic_shared;' \
  'pub type Event = generic_shared::Event<u32:8>;' \
  >"${fixture}/generic_consumer.x"
direct_output="${TEST_TMPDIR}/generic_direct.sv"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${fixture}" --output_file="${direct_output}" \
  "${fixture}/generic_shared.x"
canonical="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Direct;/\1/p' "${direct_output}")"
[[ -n "${canonical}" && "${canonical}" != Event ]]
output="${TEST_TMPDIR}/generic_consumer.sv"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${fixture}" --output_file="${output}" \
  "${fixture}/generic_consumer.x"
[[ "$(grep -Fc "} ${canonical};" "${output}")" == 1 ]]
[[ "$(grep -Fc "} ${canonical}_tag_t;" "${output}")" == 1 ]]
[[ "$(grep -Fc "typedef ${canonical} Event;" "${output}")" == 1 ]]
[[ "$(grep -Fc "${canonical}_make_item (input logic [7:0] value);" "${output}")" == 1 ]]

# Sanitization of the source identifier must agree for the nominal sum and its
# same-name re-export.
printf '%s\n' 'pub enum wire { Empty, Item(u8) }' >"${fixture}/keyword_shared.x"
printf '%s\n' 'import keyword_shared;' \
  'pub type wire = keyword_shared::wire;' >"${fixture}/keyword_consumer.x"
output="${TEST_TMPDIR}/keyword_consumer.sv"
"${translator}" --package_name=multi --namespace=multi \
  --dslx_path="${fixture}" --output_file="${output}" \
  "${fixture}/keyword_consumer.x"
[[ "$(grep -Fc '} wire_;' "${output}")" == 1 ]]
[[ "$(grep -Fc '} wire__tag_t;' "${output}")" == 1 ]]
[[ "$(grep -Fc 'wire__make_item (input logic [7:0] value);' "${output}")" == 1 ]]
[[ "$(grep -Ec '^[[:space:]]*typedef [^ ]+ wire_;$' "${output}" || true)" == 0 ]]

# A genuinely different Event remains a separate family in either root order.
printf '%s\n' 'pub enum Event { Empty, Item(u16) }' >"${fixture}/other.x"
for order in forward reverse; do
  inputs=("${fixture}/shared.x" "${fixture}/other.x" "${fixture}/consumer.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/consumer.x" "${fixture}/other.x" "${fixture}/shared.x")
  fi
  output="${TEST_TMPDIR}/event_distinct_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  [[ "$(grep -Fc '} shared_Event;' "${output}")" == 1 ]]
  [[ "$(grep -Fc '} other_Event;' "${output}")" == 1 ]]
  grep -Fq 'typedef shared_Event Event;' "${output}"
  grep -Fq 'typedef shared_Event Forward;' "${output}"
  grep -Fq 'shared_Event_make_item (input logic [7:0] value);' "${output}"
  grep -Fq 'other_Event_make_item (input logic [15:0] value);' "${output}"
done

# Alias-only clients that share a basename with an owner must not lengthen the
# owner's qualifier. The other Event is the only competing nominal definition.
mkdir -p "${fixture}/one" "${fixture}/two" "${fixture}/client"
cp "${fixture}/shared.x" "${fixture}/one/shared.x"
cp "${fixture}/other.x" "${fixture}/two/shared.x"
printf '%s\n' 'import one.shared;' 'import two.shared as other;' \
  'pub type Event = shared::Event;' 'pub type OtherEvent = other::Event;' \
  >"${fixture}/client/shared.x"
for order in forward reverse; do
  inputs=("${fixture}/one/shared.x" "${fixture}/two/shared.x" "${fixture}/client/shared.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/client/shared.x" "${fixture}/two/shared.x" "${fixture}/one/shared.x")
  fi
  output="${TEST_TMPDIR}/event_same_basename_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  grep -Fq 'typedef one_shared_Event Event;' "${output}"
  grep -Fq 'typedef two_shared_Event OtherEvent;' "${output}"
done

# The same-name alias cannot contribute a competing owner when the two real
# owners' module spellings need escaping after identifier sanitization.
mkdir -p "${fixture}/a"
cp "${fixture}/shared.x" "${fixture}/a/b.x"
cp "${fixture}/other.x" "${fixture}/a_b.x"
printf '%s\n' 'import a.b;' 'import a_b;' 'pub type Event = b::Event;' \
  'pub type OtherEvent = a_b::Event;' >"${fixture}/sanitized.x"
for order in forward reverse; do
  inputs=("${fixture}/a/b.x" "${fixture}/a_b.x" "${fixture}/sanitized.x")
  if [[ "${order}" == reverse ]]; then
    inputs=("${fixture}/sanitized.x" "${fixture}/a_b.x" "${fixture}/a/b.x")
  fi
  output="${TEST_TMPDIR}/event_sanitized_${order}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  grep -Fq 'typedef a_2e_b_Event Event;' "${output}"
  grep -Fq 'typedef a_5f_b_Event OtherEvent;' "${output}"
done

# A fixed alias owns its spelling even when a different qualified sum would
# otherwise choose it. Without that alias, retain the existing allocation.
fixture="${TEST_TMPDIR}/event_owner_collision"
mkdir -p "${fixture}"
printf '%s\n' 'pub enum Event { Empty, Item(u8) }' \
  'pub type Left = Event;' >"${fixture}/a.x"
printf '%s\n' 'pub enum Event { Empty, Item(u16) }' >"${fixture}/b.x"
printf '%s\n' 'pub enum a_Event { Empty, Item(u32) }' \
  'pub type Third = a_Event;' >"${fixture}/c.x"
printf '%s\n' 'import c;' 'pub type a_Event = c::a_Event;' >"${fixture}/alias.x"
for mode in no_alias forward reverse; do
  inputs=("${fixture}/a.x" "${fixture}/b.x" "${fixture}/c.x")
  if [[ "${mode}" == forward ]]; then
    inputs+=("${fixture}/alias.x")
  elif [[ "${mode}" == reverse ]]; then
    inputs=("${fixture}/alias.x" "${fixture}/c.x" "${fixture}/b.x" "${fixture}/a.x")
  fi
  output="${TEST_TMPDIR}/event_owner_${mode}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  left="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Left;/\1/p' "${output}")"
  third="$(sed -n 's/^[[:space:]]*typedef \([^ ]*\) Third;/\1/p' "${output}")"
  if [[ "${mode}" == no_alias ]]; then
    [[ "${left}" == a_Event && "${third}" != a_Event ]]
  else
    [[ "${left}" != a_Event && "${third}" == a_Event ]]
    [[ "$(grep -Fc '} a_Event;' "${output}")" == 1 ]]
    printf '%s\n' "${left}" "${third}" >"${output}.names"
    cmp "${TEST_TMPDIR}/event_owner_forward.sv.names" "${output}.names"
  fi
done

# A selected alias may displace a generated constructor. A private, unselected
# alias must not change the constructor name.
fixture="${TEST_TMPDIR}/event_constructor_collision"
mkdir -p "${fixture}"
printf '%s\n' 'pub enum Event { Empty, Item(u8) }' \
  'pub enum Other { Empty, Item(u16) }' \
  'type Event_make_item = Other;' \
  'pub struct Holder { item: Event_make_item }' >"${fixture}/base.x"
printf '%s\n' 'import base;' 'pub type Event_make_item = base::Other;' \
  >"${fixture}/alias.x"
for mode in no_alias forward reverse; do
  inputs=("${fixture}/base.x")
  if [[ "${mode}" == forward ]]; then
    inputs+=("${fixture}/alias.x")
  elif [[ "${mode}" == reverse ]]; then
    inputs=("${fixture}/alias.x" "${fixture}/base.x")
  fi
  output="${TEST_TMPDIR}/event_constructor_${mode}.sv"
  "${translator}" --package_name=multi --namespace=multi \
    --dslx_path="${fixture}" --output_file="${output}" "${inputs[@]}"
  if [[ "${mode}" == no_alias ]]; then
    grep -Fq 'Event_make_item (input logic [7:0] value);' "${output}"
    grep -Fq 'Other item;' "${output}"
  else
    grep -Fq 'typedef Other Event_make_item;' "${output}"
    grep -Eq 'Event_make_item__[0-9]+ \(input logic \[7:0\] value\);' "${output}"
  fi
done

# Verifies: aliases share families while distinct sum declarations stay distinct.
# Catches: input or search-path ordering changing identity or generated names.
# Existing stdin input remains usable for an ordinary file and a named import.
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

cat >"${TEST_TMPDIR}/displaced_constructor_consumer.sv" <<'EOF'
module displaced_constructor_consumer;
  import multi::*;
  Event value;
  Event_make_item alias_value;
  initial begin
    value = Event_make_item__1(8'h35);
    alias_value = Other_make_item(16'h7135);
    if (value.payload.bits !== 8'h35 ||
        Event_get_tag(value) !== Event_tag_Item ||
        alias_value.payload.bits !== 16'h7135)
      $fatal(1, "displaced constructor");
  end
endmodule
EOF
"${simulator}" -g2012 -s displaced_constructor_consumer \
  -o "${TEST_TMPDIR}/displaced_constructor_consumer" \
  "${TEST_TMPDIR}/event_constructor_forward.sv" \
  "${TEST_TMPDIR}/displaced_constructor_consumer.sv"
"${runtime}" "${TEST_TMPDIR}/displaced_constructor_consumer"

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
