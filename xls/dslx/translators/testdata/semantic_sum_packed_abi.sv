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

module semantic_sum_packed_abi_consumer;
  import semantic_sum_packed_abi::*;

  Packed item;
  PlainCode ordinary_code;
  PlainRecord ordinary_record;
  logic signed [15:0] signed_scalar, signed_array_element, signed_tuple_element;
  logic [3:0] tuple_element;
  logic [7:0] nested_element, ordinary_field;
  logic [15:0] raw_payload;

  localparam bit SCALAR_SIGNED = type(item.payload.as_scalar.value)'(-1) < 0;
  localparam bit ARRAY_SIGNED = type(item.payload.as_array.value[0])'(-1) < 0;
  localparam bit TUPLE_SIGNED =
      type(item.payload.as_tuple.value.index_0)'(-1) < 0;

  if (!SCALAR_SIGNED || !ARRAY_SIGNED || !TUPLE_SIGNED) begin : wrong_sign
    $error("selected scalar, packed-array element, and tuple element must be signed");
  end
  if ($bits(Packed) != 19 || $bits(item.payload.bits) != 16 ||
      $bits(item.payload.as_tuple.value.index_1) != 4 ||
      $bits(item.payload.as_nested.value.tag) != 1) begin : wrong_width
    $error("packed sum, shared payload, tuple, and nested tag widths differ from DSLX");
  end

  // Read one raw pattern through every typed view. The low bits hold a short
  // payload; tuple/array member zero is the most significant member.
  localparam Packed RAW = Packed'(19'h5_a3e7);
  if (RAW.tag != 3'h5 || RAW.payload.bits != 16'ha3e7 ||
      RAW.payload.as_scalar.value != -8'sd25 ||
      RAW.payload.as_array.value[0] != -8'sd25 ||
      RAW.payload.as_array.value[1] != -8'sd93 ||
      RAW.payload.as_tuple.value.index_0 != 8'sd62 ||
      RAW.payload.as_tuple.value.index_1 != 4'h7 ||
      RAW.payload.as_nested.value.tag != 1'b1 ||
      RAW.payload.as_nested.value.payload.as_number.value != 8'he7 ||
      RAW.payload.as_record.value.field != 8'he7) begin : wrong_bit_layout
    $error("typed payload views must preserve the raw DSLX bit positions");
  end

  initial begin
    item.tag = Packed_tag_Array;
    item.payload.as_nested.value.tag = Leaf_tag_Number;
    signed_scalar = item.payload.as_scalar.value;
    signed_array_element = item.payload.as_array.value[0];
    signed_tuple_element = item.payload.as_tuple.value.index_0;
    tuple_element = item.payload.as_tuple.value.index_1;
    nested_element = item.payload.as_nested.value.payload.as_number.value;
    ordinary_code = item.payload.as_code.value;
    ordinary_record = item.payload.as_record.value;
    ordinary_field = ordinary_record.field;
    raw_payload = item.payload.bits;
  end
endmodule
