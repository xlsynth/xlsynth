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

module semantic_sum_consumer;
  import semantic_sum::*;

  Message message;
  Message_pair_view_t pair_view;
  Inner inner;
  Outer outer;
  Sparse sparse;
  Payloadless payloadless;
  Singleton singleton;
  ExplicitSingleton explicit_singleton;
  SignedMessage signed_message;
  CodesAgain codes_again;
  SignedCode legacy_code;
  UnsignedCode unsigned_code;
  SignedMessage_SignedRecord_value_t signed_record;
  logic [17:0] raw_message;
  logic [17:0] raw_inner;
  logic signed [15:0] widened;

  function automatic logic known_tag(Message value);
    case (Message_get_tag(value))
      Message_tag_None, Message_tag_Byte, Message_tag_Pair: known_tag = 1'b1;
      default: known_tag = 1'b0;
    endcase
  endfunction

  initial begin
    if ($bits(Message) != 18 || $bits(Message_tag_t) != 2 ||
        $bits(Message_payload_t) != 16 || $bits(Message_none_view_t) != 16 ||
        $bits(Message_byte_view_t) != 16 || $bits(Message_pair_view_t) != 16)
      $fatal(1, "Message or one of its overlaid views has the wrong width");

    message = Message_make_none();
    if (message !== {2'd0, 16'h0000} ||
        Message_get_tag(message) !== Message_tag_None || !known_tag(message))
      $fatal(1, "None must initialize the full existing slot to zero");

    message = Message_make_byte(8'h5a);
    if (message !== {2'd1, 16'h005a} ||
        Message_get_tag(message) !== Message_tag_Byte ||
        message.payload.bits !== 16'h005a ||
        message.payload.as_byte.value !== 8'h5a ||
        message.payload.as_pair.lo !== 8'h5a)
      $fatal(1, "Byte must be low aligned and overlaid on the pair's low byte");

    message = Message_make_pair(8'h12, 8'h34);
    pair_view = Message_pair_view_t'(message.payload.bits);
    if (message !== {2'd2, 16'h1234} ||
        Message_get_tag(message) !== Message_tag_Pair ||
        message.payload.as_pair.hi !== 8'h12 || pair_view.hi !== 8'h12 ||
        pair_view.lo !== 8'h34 || !known_tag(message))
      $fatal(1, "named and cast pair views must preserve DSLX field order");

    // Crossing a raw boundary preserves unused bits. A constructor zeroes only
    // the padding that belongs to the sum it is constructing.
    raw_message = {2'd1, 16'hab5a};
    message = Message'(raw_message);
    if (message !== raw_message || message.payload.bits !== 16'hab5a ||
        message.payload.as_byte.value !== 8'h5a)
      $fatal(1, "raw transport must retain dirty high-side Byte padding");

    raw_inner = {2'd1, 16'hc35a};
    inner = Inner'(raw_inner);
    outer = Outer_make_wrapped(inner);
    if ($bits(Outer) != 26 || outer !== {2'd1, 6'b0, raw_inner} ||
        outer.payload.as_wrapped.value !== raw_inner ||
        outer.payload.as_wrapped.value.payload.bits !== 16'hc35a)
      $fatal(1, "Outer must zero its padding without rewriting Inner padding");

    if ($bits(Payloadless) != 3)
      $fatal(1, "all-zero payloads must not introduce a payload or placeholder");
    payloadless = Payloadless_make_empty();
    if (payloadless !== 3'd1 ||
        Payloadless_get_tag(payloadless) !== Payloadless_tag_Empty)
      $fatal(1, "zero-width tuple payload must still have a constructor/tag");
    payloadless = Payloadless_make_empty_record();
    if (payloadless !== 3'd5 ||
        Payloadless_get_tag(payloadless) !== Payloadless_tag_EmptyRecord)
      $fatal(1, "zero-width record payload must still have a constructor/tag");

    singleton = Singleton_make_only(16'hcafe);
    if ($bits(Singleton) != 16 || singleton !== 16'hcafe ||
        Singleton_get_tag(singleton) !== Singleton_tag_Only)
      $fatal(1, "inferred singleton tag must be queryable without adding a bit");
    explicit_singleton = ExplicitSingleton_make_only(8'h5a);
    if ($bits(ExplicitSingleton) != 11 ||
        explicit_singleton !== {3'd5, 8'h5a} ||
        ExplicitSingleton_get_tag(explicit_singleton) !==
            ExplicitSingleton_tag_Only)
      $fatal(1, "an explicit singleton tag remains present on the wire");

    sparse = Sparse_make_negative(8'h5a);
    if ($bits(Sparse) != 11 || sparse !== {3'b111, 8'h5a} ||
        Sparse_get_tag(sparse) !== Sparse_tag_Negative ||
        !(Sparse_get_tag(sparse) < 0))
      $fatal(1, "negative explicit tags must retain bits and signed arithmetic");
    sparse = Sparse_make_positive(8'ha5);
    if (sparse !== {3'd2, 8'ha5} ||
        Sparse_get_tag(sparse) !== Sparse_tag_Positive)
      $fatal(1, "sparse tags must retain evaluated source discriminants");

    signed_message = SignedMessage_make_scalar(8'hff);
    widened = signed_message.payload.as_scalar.value;
    if (!(signed_message.payload.as_scalar.value < 0) || widened !== 16'hffff)
      $fatal(1, "signed scalar payload must compare negative and sign extend");
    signed_message = SignedMessage_make_bit(1'b1);
    widened = signed_message.payload.as_bit.value;
    if (!(signed_message.payload.as_bit.value < 0) || widened !== 16'hffff)
      $fatal(1, "one-bit signed payload must retain signed interpretation");
    signed_record = '1;
    signed_message = SignedMessage_make_record(signed_record);
    widened = signed_message.payload.as_record.value.item;
    if (!(signed_message.payload.as_record.value.item < 0) ||
        !(signed_message.payload.as_record.value.flag < 0) ||
        widened !== 16'hffff)
      $fatal(1, "a named record/alias must preserve nested signed leaves");
    signed_message = SignedMessage_make_tuple(16'hff01);
    widened = signed_message.payload.as_tuple.value.index_0;
    if (!(signed_message.payload.as_tuple.value.index_0 < 0) ||
        signed_message.payload.as_tuple.value.index_1 !== 8'h01 ||
        widened !== 16'hffff)
      $fatal(1, "a tuple must preserve field order and signed interpretation");
    signed_message = SignedMessage_make_array(16'h01ff);
    widened = signed_message.payload.as_array.value[0];
    if (!(signed_message.payload.as_array.value[0] < 0) ||
        signed_message.payload.as_array.value[1] !== 8'h01 ||
        widened !== 16'hffff)
      $fatal(1, "an array element must retain signed interpretation");

    // Ordinary SV enum types are nominal. Legacy and sum-specific enums cross
    // using an explicit cast in each direction, preserving the underlying bits.
    legacy_code = NEG;
    signed_message = SignedMessage_make_error(
        SignedMessage_SignedCode_value_t'(legacy_code));
    codes_again =
        CodesAgain_make_error(CodesAgain_SignedCode_value_t'(legacy_code));
    widened = signed_message.payload.as_error.value;
    if (signed_message.payload.as_error.value !==
            SignedMessage_SignedCode_enum_NEG ||
        codes_again.payload.as_error.value !== CodesAgain_SignedCode_enum_NEG ||
        !(signed_message.payload.as_error.value < 0) || widened !== 16'hffff ||
        SignedCode'(signed_message.payload.as_error.value) !== legacy_code ||
        legacy_code < 0)
      $fatal(1, "sum-scoped enum types must coexist and preserve signed bits");

    // An ordinary unsigned enum already has the correct semantics. It remains
    // the very same SV enum type when passed to or read from a sum.
    unsigned_code = Bad;
    signed_message = SignedMessage_make_status(unsigned_code);
    unsigned_code = signed_message.payload.as_status.value;
    if (unsigned_code !== Bad)
      $fatal(1, "an ordinary unsigned enum should cross without a cast");

    message = Message'({2'b11, 16'h005a});
    message.tag = Message_tag_t'(2'b11);
    if (Message_get_tag(message) !== 2'b11 || known_tag(message))
      $fatal(1, "an undeclared binary tag must be preserved and take default");
`ifndef XLS_DISABLE_FOUR_STATE_CHECKS
    if (!$isunknown(2'bxz))
      $fatal(1, "this test requires a four-state simulator");
    raw_message = {2'bx1, 16'hab5a};
    message = Message'(raw_message);
    if (message !== raw_message || Message_get_tag(message) !== 2'bx1 ||
        known_tag(message))
      $fatal(1, "an X tag must be preserved and take default");
    raw_message = {2'bz0, 16'hab5a};
    message = Message'(raw_message);
    if (message !== raw_message || Message_get_tag(message) !== 2'bz0 ||
        known_tag(message))
      $fatal(1, "a Z tag must be preserved and take default");
`endif

    $display("semantic_sum_consumer PASS");
    $finish;
  end
endmodule
