// Copyright 2026 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module semantic_sum_name_scope_consumer;
  import semantic_sum_name_scope::*;

  Message item;
  as_x union_shadow;
  as_reverse union_reverse;
  logic [7:0] named_member, ordinary_member, direct_member, unrelated_member;
  logic [7:0] one_member, pair_member;
  logic [1:0] shadow_member, reverse_member;
  logic signed [7:0] signed_member;

  // Verifies: generated members resolve in their legal struct and union scopes.
  // Catches: missing or unnecessary renaming and qualification under Slang.
  initial begin
    // A later anonymous packed struct has its own member scope. The three
    // packed views retain the DSLX name.
    named_member = item.payload.as_named.Token;
    ordinary_member = item.payload.as_ordinary.value.Token;
    signed_member = item.payload.as_signed.value.Token;

    // Direct name hiding needs a suffix; an unrelated package type does not.
    direct_member = item.payload.as_direct.Token__1;
    unrelated_member = item.payload.as_unrelated.Token;

    // Positional names remain fixed even when a later type is named index_0.
    one_member = item.payload.as_one.value;
    pair_member = item.payload.as_pair.index_0;

    // Fixed union member spellings survive when they hide a later view type.
    shadow_member = union_shadow.payload.as_y.value;
    reverse_member = union_reverse.payload.as_y.value;
  end
endmodule
