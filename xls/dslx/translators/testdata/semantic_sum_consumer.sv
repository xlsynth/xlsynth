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

// Icarus 12 uses the enclosing packed value's width and loses the signed member
// type. The extended simulator checks declared signed semantics without this cast.
`ifdef XLS_ICARUS_NESTED_PACKED_SIGNEDNESS
`define XLS_SIGNED_VIEW(width, value) $signed(width'(value))
`else
`define XLS_SIGNED_VIEW(width, value) value
`endif

module semantic_sum_consumer;
  import semantic_sum::*;

  Message message;
  Message_pair_view_t pair_view;
  MixedChildren mixed;
  Inner inner;
  Outer outer;
  Sparse sparse;
  Payloadless payloadless;
  Singleton singleton;
  ExplicitSingleton explicit_singleton;
  SignedMessage signed_message;
  CodesAgain codes_again;
  ShadowedMessage shadowed_message;
  SignedCode legacy_code;
  UnsignedCode unsigned_code;
  SignedMessage_SignedRecord_value_t signed_record;
  ShadowedMessage_ShadowedRecord_value_t shadowed_record;
  ReusedShadowedRecord reused_shadowed_record;
  Token named_token;
  logic [17:0] raw_message;
  logic [17:0] raw_inner;
  logic signed [15:0] widened;
  logic [1:0] dslx_selector;
  logic [7:0] dslx_hi;
  logic [7:0] dslx_lo;
  wire [17:0] dslx_produced;
  logic [17:0] dslx_input;
  wire [15:0] dslx_matched;
  logic [1:0] dslx_sparse_selector;
  logic [7:0] dslx_sparse_payload;
  logic [17:0] dslx_inner_input;
  wire [10:0] dslx_sparse_produced;
  wire [25:0] dslx_outer_produced;
  logic [10:0] dslx_sparse_input;
  logic [25:0] dslx_outer_input;
  wire [15:0] dslx_sparse_matched;
  wire [17:0] dslx_inner_matched;
  wire [25:0] dslx_outer_transported;

  semantic_sum_producer producer(
      .selector(dslx_selector), .hi(dslx_hi), .lo(dslx_lo),
      .out(dslx_produced));
  semantic_sum_matcher matcher(.message(dslx_input), .out(dslx_matched));
  semantic_sum_sparse_outer_producer sparse_outer_producer(
      .selector(dslx_sparse_selector), .payload(dslx_sparse_payload),
      .inner(dslx_inner_input), .out({dslx_sparse_produced, dslx_outer_produced}));
  semantic_sum_sparse_outer_matcher sparse_outer_matcher(
      .sparse(dslx_sparse_input), .outer(dslx_outer_input),
      .out({dslx_sparse_matched, dslx_inner_matched, dslx_outer_transported}));

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

    // Drive the real DSLX constructor and inspect its raw output through the
    // package. Simultaneously feed package-constructed bits to the DSLX match.
    dslx_selector = 2'd1;
    dslx_hi = 8'he7;
    dslx_lo = 8'h5a;
    dslx_input = Message_make_pair(8'h12, 8'h34);
    #1;
    message = Message'(dslx_produced);
    if (message !== Message_make_byte(8'h5a) ||
        Message_get_tag(message) !== Message_tag_Byte ||
        message.payload.bits !== 16'h005a ||
        message.payload.as_byte.value !== 8'h5a || dslx_matched !== 16'h1234)
      $fatal(1, "DSLX Byte producer or DSLX matcher of an SV Pair disagrees");

    dslx_selector = 2'd2;
    dslx_hi = 8'h12;
    dslx_lo = 8'h34;
    dslx_input = Message_make_byte(8'ha6);
    #1;
    message = Message'(dslx_produced);
    if (message !== Message_make_pair(8'h12, 8'h34) ||
        Message_get_tag(message) !== Message_tag_Pair ||
        message.payload.as_pair.hi !== 8'h12 ||
        message.payload.as_pair.lo !== 8'h34 || dslx_matched !== 16'h00a6)
      $fatal(1, "DSLX Pair producer or DSLX matcher of an SV Byte disagrees");

    dslx_selector = 2'd0;
    dslx_input = Message'({2'd1, 16'hab5a});
    #1;
    message = Message'(dslx_produced);
    if (message !== Message_make_none() ||
        Message_get_tag(message) !== Message_tag_None ||
        dslx_matched !== 16'h005a)
      $fatal(1, "DSLX None must clear its slot; matching Byte must ignore padding");

    dslx_input = Message_make_none();
    #1;
    if (dslx_matched !== 16'h0000)
      $fatal(1, "DSLX must match an SV-constructed None");

    dslx_input = Message'({Message_tag_None, 16'ha55a});
    #1;
    if (dslx_matched !== 16'h0000)
      $fatal(1, "DSLX matching None must ignore its entire unused payload");

    dslx_input = Message'({2'd1, 16'bxxxx_zzzz_0101_1010});
    #1;
    if (dslx_matched !== 16'h005a)
      $fatal(1, "DSLX matching must ignore four-state padding supplied from SV");

    // The compiled producer constructs sparse tags and wraps externally dirty
    // Inner values. In the other direction the compiled matcher reads SV-made
    // sums and returns the original Outer, including both levels of padding.
    dslx_sparse_selector = 2'd1;
    dslx_sparse_payload = 8'h5a;
    dslx_inner_input = Inner'({2'd1, 16'hc35a});
    dslx_sparse_input = Sparse_make_positive(8'ha6);
    dslx_outer_input = Outer_make_wrapped(Inner'({2'd1, 16'he73c}));
    dslx_outer_input[23:18] = 6'b101101;
    #1;
    sparse = Sparse'(dslx_sparse_produced);
    outer = Outer'(dslx_outer_produced);
    if (sparse !== {3'b111, 8'h5a} || sparse !== Sparse_make_negative(8'h5a) ||
        Sparse_get_tag(sparse) !== Sparse_tag_Negative ||
        !(Sparse_get_tag(sparse) < 0) ||
        sparse.payload.as_negative.value !== 8'h5a ||
        dslx_sparse_matched !== 16'h02a6)
      $fatal(1, "DSLX Negative producer or DSLX matcher of an SV Positive disagrees");
    if (outer !== {2'd1, 6'b0, dslx_inner_input} ||
        outer !== Outer_make_wrapped(Inner'(dslx_inner_input)) ||
        Outer_get_tag(outer) !== Outer_tag_Wrapped ||
        outer.payload.as_wrapped.value.payload.bits !== 16'hc35a ||
        dslx_inner_matched !== {2'd1, 16'he73c} ||
        dslx_outer_transported !== {2'd1, 6'b101101, 2'd1, 16'he73c})
      $fatal(1, "DSLX Outer construction or matching/transport rewrote dirty padding");

    dslx_sparse_selector = 2'd2;
    dslx_sparse_payload = 8'ha5;
    dslx_inner_input = Inner'({2'd1, 16'h3ca5});
    dslx_sparse_input = Sparse_make_negative(8'h69);
    dslx_outer_input = Outer_make_wrapped(Inner'({2'd1, 16'h5a96}));
    dslx_outer_input[23:18] = 6'b010010;
    #1;
    sparse = Sparse'(dslx_sparse_produced);
    outer = Outer'(dslx_outer_produced);
    if (sparse !== {3'd2, 8'ha5} || sparse !== Sparse_make_positive(8'ha5) ||
        Sparse_get_tag(sparse) !== Sparse_tag_Positive ||
        sparse.payload.as_positive.value !== 8'ha5 ||
        dslx_sparse_matched !== 16'hff69)
      $fatal(1, "DSLX Positive producer or DSLX matcher of an SV Negative disagrees");
    if (outer !== {2'd1, 6'b0, dslx_inner_input} ||
        outer.payload.as_wrapped.value.payload.bits !== 16'h3ca5 ||
        dslx_inner_matched !== {2'd1, 16'h5a96} ||
        dslx_outer_transported !== {2'd1, 6'b010010, 2'd1, 16'h5a96})
      $fatal(1, "DSLX must preserve changed binary padding patterns in both directions");

    dslx_sparse_selector = 2'd0;
    dslx_sparse_input = Sparse_make_empty();
    dslx_outer_input = Outer_make_wide(24'hfedcba);
    #1;
    sparse = Sparse'(dslx_sparse_produced);
    if (sparse !== {3'd0, 8'h00} || sparse !== Sparse_make_empty() ||
        Sparse_get_tag(sparse) !== Sparse_tag_Empty || dslx_sparse_matched !== 0 ||
        dslx_inner_matched !== Inner_make_none() ||
        dslx_outer_transported !== Outer_make_wide(24'hfedcba))
      $fatal(1, "DSLX must distinguish Sparse Empty and Outer Wide from payload matches");

    // With known tags, genuine X and Z must remain distinguishable across the
    // constructor's raw Inner path and the matcher's unchanged raw Outer path.
    dslx_inner_input = Inner'({2'd1, 8'bxz01_zx10, 8'h69});
    dslx_outer_input = Outer_make_wrapped(Inner'({2'd1, 8'bzx10_xz01, 8'h96}));
    dslx_outer_input[23:18] = 6'bzx01xz;
    #1;
    outer = Outer'(dslx_outer_produced);
    inner = Inner'(dslx_inner_matched);
    if (outer !== {2'd1, 6'b0, 2'd1, 8'bxz01_zx10, 8'h69} ||
        outer.payload.as_wrapped.value.payload.bits !== {8'bxz01_zx10, 8'h69} ||
        dslx_outer_transported !== {2'd1, 6'bzx01xz, 2'd1, 8'bzx10_xz01, 8'h96} ||
        Inner_get_tag(inner) !== Inner_tag_Byte ||
        inner.payload.as_byte.value !== 8'h96)
      $fatal(1, "compiled DSLX raw transport must preserve nested and outer X/Z padding");

    mixed = MixedChildren_make_positional(8'h12, 8'h34);
    if ($bits(MixedChildren) != 18 ||
        $bits(MixedChildren_positional_view_t) != 16 ||
        $bits(MixedChildren_record_view_t) != 16 ||
        mixed !== {2'd1, 16'h1234} ||
        mixed.payload.as_positional.index_1 !== 8'h12 ||
        mixed.payload.as_positional.index_3 !== 8'h34)
      $fatal(1, "omitted zero-width positional children must retain original indices");
    mixed = MixedChildren_make_record(8'hab, 8'hcd);
    if (mixed !== {2'd2, 16'habcd} ||
        mixed.payload.as_record.hi !== 8'hab ||
        mixed.payload.as_record.lo !== 8'hcd)
      $fatal(1, "omitted zero-width record children must not shift visible fields");

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
    widened = `XLS_SIGNED_VIEW(8, signed_message.payload.as_scalar.value);
    if (!(`XLS_SIGNED_VIEW(8, signed_message.payload.as_scalar.value) < 0) ||
        widened !== 16'hffff)
      $fatal(1, "signed scalar payload must compare negative and sign extend");
    signed_message = SignedMessage_make_bit(1'b1);
    widened = `XLS_SIGNED_VIEW(1, signed_message.payload.as_bit.value);
    if (!(`XLS_SIGNED_VIEW(1, signed_message.payload.as_bit.value) < 0) ||
        widened !== 16'hffff)
      $fatal(1, "one-bit signed payload must retain signed interpretation");
    signed_record = '1;
    signed_message = SignedMessage_make_record(signed_record);
    widened = `XLS_SIGNED_VIEW(8, signed_message.payload.as_record.value.item);
    if (!(`XLS_SIGNED_VIEW(8, signed_message.payload.as_record.value.item) < 0) ||
        !(`XLS_SIGNED_VIEW(1, signed_message.payload.as_record.value.flag) < 0) ||
        widened !== 16'hffff)
      $fatal(1, "a named record/alias must preserve nested signed leaves");
    signed_message = SignedMessage_make_tuple(16'hff01);
    widened = `XLS_SIGNED_VIEW(8, signed_message.payload.as_tuple.value.index_0);
    if (!(`XLS_SIGNED_VIEW(8, signed_message.payload.as_tuple.value.index_0) < 0) ||
        signed_message.payload.as_tuple.value.index_1 !== 8'h01 ||
        widened !== 16'hffff)
      $fatal(1, "a tuple must preserve field order and signed interpretation");
    signed_message = SignedMessage_make_array(16'h01ff);
    widened = `XLS_SIGNED_VIEW(8, signed_message.payload.as_array.value[0]);
    if (!(`XLS_SIGNED_VIEW(8, signed_message.payload.as_array.value[0]) < 0) ||
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
    widened = `XLS_SIGNED_VIEW(8, signed_message.payload.as_error.value);
    if (signed_message.payload.as_error.value !==
            SignedMessage_SignedCode_enum_NEG ||
        codes_again.payload.as_error.value !== CodesAgain_SignedCode_enum_NEG ||
        !(`XLS_SIGNED_VIEW(8, signed_message.payload.as_error.value) < 0) ||
        widened !== 16'hffff ||
        SignedCode'(signed_message.payload.as_error.value) !== legacy_code ||
        legacy_code < 0)
      $fatal(1, "sum-scoped enum types must coexist and preserve signed bits");

    // An ordinary unsigned enum already has the correct semantics. It remains
    // the very same SV enum type when passed to or read from a sum.
    unsigned_code = Bad;
    signed_message = SignedMessage_make_status(unsigned_code);
`ifdef XLS_ICARUS_UNSIGNED_ENUM_VIEW_CAST
    // Icarus 12 loses the enum type on this nested packed-union member select.
    unsigned_code = UnsignedCode'(signed_message.payload.as_status.value);
`else
    unsigned_code = signed_message.payload.as_status.value;
`endif
    if (unsigned_code !== Bad)
      $fatal(1, "an ordinary unsigned enum must preserve its value across the sum");

    named_token.x = 8'h42;
    shadowed_message = ShadowedMessage_make_direct(8'ha5, named_token);
    if (shadowed_message.payload.as_direct.Token__1 !== 8'ha5 ||
        shadowed_message.payload.as_direct.data.x !== 8'h42)
      $fatal(1, "a packed view field must not collide with a package typedef");
    shadowed_message = ShadowedMessage_make_type_only_elsewhere(8'h79);
    if (shadowed_message.payload.as_type_only_elsewhere.token_value !== 8'h79)
      $fatal(1, "a separate variant must preserve its own field spelling");
    shadowed_record.Token__1 = 8'hfd;
    shadowed_record.data = named_token;
    shadowed_message = ShadowedMessage_make_named(shadowed_record);
    if (shadowed_message.payload.as_named.value.Token__1 !== 8'hfd ||
        shadowed_message.payload.as_named.value.data.x !== 8'h42)
      $fatal(1, "a semantic named record must avoid a visible package typedef");
    reused_shadowed_record.Token__1 = 8'h35;
    reused_shadowed_record.data = named_token;
    shadowed_message = ShadowedMessage_make_reused(reused_shadowed_record);
    if (shadowed_message.payload.as_reused.value.Token__1 !== 8'h35 ||
        shadowed_message.payload.as_reused.value.data.x !== 8'h42)
      $fatal(1, "an earlier ordinary record reused by a sum must be legal");

    message = Message'({2'b11, 16'h005a});
    message.tag = Message_tag_t'(2'b11);
    if (Message_get_tag(message) !== 2'b11 || known_tag(message))
      $fatal(1, "an undeclared binary tag must be preserved and take default");
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

    $display("semantic_sum_consumer PASS");
    $finish;
  end
endmodule

`undef XLS_SIGNED_VIEW
