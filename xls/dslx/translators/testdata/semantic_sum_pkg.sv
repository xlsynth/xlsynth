// verilog_lint: waive-start struct-union-name-style
package semantic_sum;
  // DSLX Type: pub enum MaybeWord {
  //     None,
  //     Some(u32),
  //     Pair { lo: u8, hi: u8 },
  // }
  typedef enum logic [1:0] {
    MaybeWord_tag_None = 2'h0,
    MaybeWord_tag_Some = 2'h1,
    MaybeWord_tag_Pair = 2'h2
  } MaybeWord_tag_t;
  typedef struct packed {
    logic [31:0] xls_padding;
  } MaybeWord_none_view_t;
  typedef struct packed {
    logic [31:0] value;
  } MaybeWord_some_view_t;
  typedef struct packed {
    logic [15:0] xls_padding;
    logic [7:0] lo;
    logic [7:0] hi;
  } MaybeWord_pair_view_t;
  typedef union packed {
    logic [31:0] bits;
    MaybeWord_none_view_t as_none;
    MaybeWord_some_view_t as_some;
    MaybeWord_pair_view_t as_pair;
  } MaybeWord_payload_t;
  typedef struct packed {
    MaybeWord_tag_t tag;
    MaybeWord_payload_t payload;
  } MaybeWord;

  function automatic MaybeWord MaybeWord_make_none ();
    begin
      MaybeWord_make_none = MaybeWord'({MaybeWord_tag_None, 32'h0000_0000});
    end
  endfunction

  function automatic MaybeWord MaybeWord_make_some (input logic [31:0] value);
    begin
      MaybeWord_make_some = MaybeWord'({MaybeWord_tag_Some, value});
    end
  endfunction

  function automatic MaybeWord MaybeWord_make_pair (input logic [7:0] lo, input logic [7:0] hi);
    begin
      MaybeWord_make_pair = MaybeWord'({MaybeWord_tag_Pair, 16'h0000, lo, hi});
    end
  endfunction

  function automatic MaybeWord_tag_t MaybeWord_get_tag (input MaybeWord value);
    begin
      MaybeWord_get_tag = MaybeWord_tag_t'(value[33:32]);
    end
  endfunction

  // DSLX Type: pub enum ExplicitTagWidth : u5 {
  //     None = 0,
  //     Some(u8) = 1,
  // }
  typedef enum logic [4:0] {
    ExplicitTagWidth_tag_None = 5'h00,
    ExplicitTagWidth_tag_Some = 5'h01
  } ExplicitTagWidth_tag_t;
  typedef struct packed {
    logic [7:0] xls_padding;
  } ExplicitTagWidth_none_view_t;
  typedef struct packed {
    logic [7:0] value;
  } ExplicitTagWidth_some_view_t;
  typedef union packed {
    logic [7:0] bits;
    ExplicitTagWidth_none_view_t as_none;
    ExplicitTagWidth_some_view_t as_some;
  } ExplicitTagWidth_payload_t;
  typedef struct packed {
    ExplicitTagWidth_tag_t tag;
    ExplicitTagWidth_payload_t payload;
  } ExplicitTagWidth;

  function automatic ExplicitTagWidth ExplicitTagWidth_make_none ();
    begin
      ExplicitTagWidth_make_none = ExplicitTagWidth'({ExplicitTagWidth_tag_None, 8'h00});
    end
  endfunction

  function automatic ExplicitTagWidth ExplicitTagWidth_make_some (input logic [7:0] value);
    begin
      ExplicitTagWidth_make_some = ExplicitTagWidth'({ExplicitTagWidth_tag_Some, value});
    end
  endfunction

  function automatic ExplicitTagWidth_tag_t ExplicitTagWidth_get_tag (input ExplicitTagWidth value);
    begin
      ExplicitTagWidth_get_tag = ExplicitTagWidth_tag_t'(value[12:8]);
    end
  endfunction

  // DSLX Type: pub enum Singleton {
  //     Only(u16),
  // }
  typedef enum logic {
    Singleton_tag_Only = 1'h0
  } Singleton_tag_t;
  typedef struct packed {
    logic [15:0] value;
  } Singleton_only_view_t;
  typedef union packed {
    logic [15:0] bits;
    Singleton_only_view_t as_only;
  } Singleton_payload_t;
  typedef struct packed {
    Singleton_payload_t payload;
  } Singleton;

  function automatic Singleton Singleton_make_only (input logic [15:0] value);
    begin
      Singleton_make_only = Singleton'(value);
    end
  endfunction

  function automatic Singleton_tag_t Singleton_get_tag (input Singleton value);
    begin
      Singleton_get_tag = Singleton_tag_Only;
    end
  endfunction

  // DSLX Type: pub enum Message {
  //     None,
  //     Byte(u8),
  //     Pair { hi: u8, lo: u8 },
  // }
  typedef enum logic [1:0] {
    Message_tag_None = 2'h0,
    Message_tag_Byte = 2'h1,
    Message_tag_Pair = 2'h2
  } Message_tag_t;
  typedef struct packed {
    logic [15:0] xls_padding;
  } Message_none_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    logic [7:0] value;
  } Message_byte_view_t;
  typedef struct packed {
    logic [7:0] hi;
    logic [7:0] lo;
  } Message_pair_view_t;
  typedef union packed {
    logic [15:0] bits;
    Message_none_view_t as_none;
    Message_byte_view_t as_byte;
    Message_pair_view_t as_pair;
  } Message_payload_t;
  typedef struct packed {
    Message_tag_t tag;
    Message_payload_t payload;
  } Message;

  function automatic Message Message_make_none ();
    begin
      Message_make_none = Message'({Message_tag_None, 16'h0000});
    end
  endfunction

  function automatic Message Message_make_byte (input logic [7:0] value);
    begin
      Message_make_byte = Message'({Message_tag_Byte, 8'h00, value});
    end
  endfunction

  function automatic Message Message_make_pair (input logic [7:0] hi, input logic [7:0] lo);
    begin
      Message_make_pair = Message'({Message_tag_Pair, hi, lo});
    end
  endfunction

  function automatic Message_tag_t Message_get_tag (input Message value);
    begin
      Message_get_tag = Message_tag_t'(value[17:16]);
    end
  endfunction

  // DSLX Type: pub enum MixedChildren {
  //     None,
  //     Positional((), u8, (), u8),
  //     Record { before: (), hi: u8, middle: (), lo: u8, after: () },
  // }
  typedef enum logic [1:0] {
    MixedChildren_tag_None = 2'h0,
    MixedChildren_tag_Positional = 2'h1,
    MixedChildren_tag_Record = 2'h2
  } MixedChildren_tag_t;
  typedef struct packed {
    logic [15:0] xls_padding;
  } MixedChildren_none_view_t;
  typedef struct packed {
    logic [7:0] index_1;
    logic [7:0] index_3;
  } MixedChildren_positional_view_t;
  typedef struct packed {
    logic [7:0] hi;
    logic [7:0] lo;
  } MixedChildren_record_view_t;
  typedef union packed {
    logic [15:0] bits;
    MixedChildren_none_view_t as_none;
    MixedChildren_positional_view_t as_positional;
    MixedChildren_record_view_t as_record;
  } MixedChildren_payload_t;
  typedef struct packed {
    MixedChildren_tag_t tag;
    MixedChildren_payload_t payload;
  } MixedChildren;

  function automatic MixedChildren MixedChildren_make_none ();
    begin
      MixedChildren_make_none = MixedChildren'({MixedChildren_tag_None, 16'h0000});
    end
  endfunction

  function automatic MixedChildren MixedChildren_make_positional (input logic [7:0] index_1, input logic [7:0] index_3);
    begin
      MixedChildren_make_positional = MixedChildren'({MixedChildren_tag_Positional, index_1, index_3});
    end
  endfunction

  function automatic MixedChildren MixedChildren_make_record (input logic [7:0] hi, input logic [7:0] lo);
    begin
      MixedChildren_make_record = MixedChildren'({MixedChildren_tag_Record, hi, lo});
    end
  endfunction

  function automatic MixedChildren_tag_t MixedChildren_get_tag (input MixedChildren value);
    begin
      MixedChildren_get_tag = MixedChildren_tag_t'(value[17:16]);
    end
  endfunction

  // DSLX Type: pub enum Inner {
  //     None,
  //     Byte(u8),
  //     Wide(u16),
  // }
  typedef enum logic [1:0] {
    Inner_tag_None = 2'h0,
    Inner_tag_Byte = 2'h1,
    Inner_tag_Wide = 2'h2
  } Inner_tag_t;
  typedef struct packed {
    logic [15:0] xls_padding;
  } Inner_none_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    logic [7:0] value;
  } Inner_byte_view_t;
  typedef struct packed {
    logic [15:0] value;
  } Inner_wide_view_t;
  typedef union packed {
    logic [15:0] bits;
    Inner_none_view_t as_none;
    Inner_byte_view_t as_byte;
    Inner_wide_view_t as_wide;
  } Inner_payload_t;
  typedef struct packed {
    Inner_tag_t tag;
    Inner_payload_t payload;
  } Inner;

  function automatic Inner Inner_make_none ();
    begin
      Inner_make_none = Inner'({Inner_tag_None, 16'h0000});
    end
  endfunction

  function automatic Inner Inner_make_byte (input logic [7:0] value);
    begin
      Inner_make_byte = Inner'({Inner_tag_Byte, 8'h00, value});
    end
  endfunction

  function automatic Inner Inner_make_wide (input logic [15:0] value);
    begin
      Inner_make_wide = Inner'({Inner_tag_Wide, value});
    end
  endfunction

  function automatic Inner_tag_t Inner_get_tag (input Inner value);
    begin
      Inner_get_tag = Inner_tag_t'(value[17:16]);
    end
  endfunction

  // DSLX Type: pub enum Outer {
  //     None,
  //     Wrapped(Inner),
  //     Wide(u24),
  // }
  typedef enum logic [1:0] {
    Outer_tag_None = 2'h0,
    Outer_tag_Wrapped = 2'h1,
    Outer_tag_Wide = 2'h2
  } Outer_tag_t;
  typedef struct packed {
    logic [23:0] xls_padding;
  } Outer_none_view_t;
  typedef struct packed {
    logic [5:0] xls_padding;
    Inner value;
  } Outer_wrapped_view_t;
  typedef struct packed {
    logic [23:0] value;
  } Outer_wide_view_t;
  typedef union packed {
    logic [23:0] bits;
    Outer_none_view_t as_none;
    Outer_wrapped_view_t as_wrapped;
    Outer_wide_view_t as_wide;
  } Outer_payload_t;
  typedef struct packed {
    Outer_tag_t tag;
    Outer_payload_t payload;
  } Outer;

  function automatic Outer Outer_make_none ();
    begin
      Outer_make_none = Outer'({Outer_tag_None, 24'h00_0000});
    end
  endfunction

  function automatic Outer Outer_make_wrapped (input Inner value);
    begin
      Outer_make_wrapped = Outer'({Outer_tag_Wrapped, 6'h00, value});
    end
  endfunction

  function automatic Outer Outer_make_wide (input logic [23:0] value);
    begin
      Outer_make_wide = Outer'({Outer_tag_Wide, value});
    end
  endfunction

  function automatic Outer_tag_t Outer_get_tag (input Outer value);
    begin
      Outer_get_tag = Outer_tag_t'(value[25:24]);
    end
  endfunction

  // DSLX Type: pub enum Sparse : s3 {
  //     Empty = 0,
  //     Negative(u8) = -1,
  //     Positive(u8) = 2,
  // }
  typedef enum logic signed [2:0] {
    Sparse_tag_Empty = 3'h0,
    Sparse_tag_Negative = 3'h7,
    Sparse_tag_Positive = 3'h2
  } Sparse_tag_t;
  typedef struct packed {
    logic [7:0] xls_padding;
  } Sparse_empty_view_t;
  typedef struct packed {
    logic [7:0] value;
  } Sparse_negative_view_t;
  typedef struct packed {
    logic [7:0] value;
  } Sparse_positive_view_t;
  typedef union packed {
    logic [7:0] bits;
    Sparse_empty_view_t as_empty;
    Sparse_negative_view_t as_negative;
    Sparse_positive_view_t as_positive;
  } Sparse_payload_t;
  typedef struct packed {
    Sparse_tag_t tag;
    Sparse_payload_t payload;
  } Sparse;

  function automatic Sparse Sparse_make_empty ();
    begin
      Sparse_make_empty = Sparse'({Sparse_tag_Empty, 8'h00});
    end
  endfunction

  function automatic Sparse Sparse_make_negative (input logic [7:0] value);
    begin
      Sparse_make_negative = Sparse'({Sparse_tag_Negative, value});
    end
  endfunction

  function automatic Sparse Sparse_make_positive (input logic [7:0] value);
    begin
      Sparse_make_positive = Sparse'({Sparse_tag_Positive, value});
    end
  endfunction

  function automatic Sparse_tag_t Sparse_get_tag (input Sparse value);
    begin
      Sparse_get_tag = Sparse_tag_t'(value[10:8]);
    end
  endfunction

  // DSLX Type: pub enum Payloadless : u3 {
  //     Empty() = 1,
  //     EmptyRecord {} = 5,
  // }
  typedef enum logic [2:0] {
    Payloadless_tag_Empty = 3'h1,
    Payloadless_tag_EmptyRecord = 3'h5
  } Payloadless_tag_t;
  typedef struct packed {
    Payloadless_tag_t tag;
  } Payloadless;

  function automatic Payloadless Payloadless_make_empty ();
    begin
      Payloadless_make_empty = Payloadless'(Payloadless_tag_Empty);
    end
  endfunction

  function automatic Payloadless Payloadless_make_empty_record ();
    begin
      Payloadless_make_empty_record = Payloadless'(Payloadless_tag_EmptyRecord);
    end
  endfunction

  function automatic Payloadless_tag_t Payloadless_get_tag (input Payloadless value);
    begin
      Payloadless_get_tag = Payloadless_tag_t'(value[2:0]);
    end
  endfunction

  // DSLX Type: pub enum ExplicitSingleton : u3 {
  //     Only(u8) = 5,
  // }
  typedef enum logic [2:0] {
    ExplicitSingleton_tag_Only = 3'h5
  } ExplicitSingleton_tag_t;
  typedef struct packed {
    logic [7:0] value;
  } ExplicitSingleton_only_view_t;
  typedef union packed {
    logic [7:0] bits;
    ExplicitSingleton_only_view_t as_only;
  } ExplicitSingleton_payload_t;
  typedef struct packed {
    ExplicitSingleton_tag_t tag;
    ExplicitSingleton_payload_t payload;
  } ExplicitSingleton;

  function automatic ExplicitSingleton ExplicitSingleton_make_only (input logic [7:0] value);
    begin
      ExplicitSingleton_make_only = ExplicitSingleton'({ExplicitSingleton_tag_Only, value});
    end
  endfunction

  function automatic ExplicitSingleton_tag_t ExplicitSingleton_get_tag (input ExplicitSingleton value);
    begin
      ExplicitSingleton_get_tag = ExplicitSingleton_tag_t'(value[10:8]);
    end
  endfunction

  // DSLX Type: pub enum SignedCode : s8 {
  //     NEG = -1,
  //     ZERO = 0,
  // }
  typedef enum logic [7:0] {
    NEG = 8'hff,
    ZERO = 8'h00
  } SignedCode;

  // DSLX Type: pub enum UnsignedCode : u8 {
  //     Good = 0,
  //     Bad = 1,
  // }
  typedef enum logic [7:0] {
    Good = 8'h00,
    Bad = 8'h01
  } UnsignedCode;

  // DSLX Type: pub struct SignedRecord {
  //     item: s8,
  //     flag: s1,
  // }
  typedef struct packed {
    logic [7:0] item;
    logic flag;
  } SignedRecord;

  // DSLX Type: pub type SignedAlias = SignedRecord;
  typedef SignedRecord SignedAlias;
  typedef struct packed {
    logic signed [7:0] item;
    logic signed flag;
  } SignedMessage_SignedRecord_value_t;
  typedef logic signed [7:0] SignedMessage_s8_value_t;
  typedef enum logic signed [7:0] {
    SignedMessage_SignedCode_enum_NEG = 8'hff,
    SignedMessage_SignedCode_enum_ZERO = 8'h00
  } SignedMessage_SignedCode_value_t;

  // DSLX Type: pub enum SignedMessage {
  //     None,
  //     Scalar(s8),
  //     Bit(s1),
  //     Record(SignedAlias),
  //     Tuple((s8, u8)),
  //     Array(s8[2]),
  //     Error(SignedCode),
  //     Status(UnsignedCode),
  // }
  typedef enum logic [2:0] {
    SignedMessage_tag_None = 3'h0,
    SignedMessage_tag_Scalar = 3'h1,
    SignedMessage_tag_Bit = 3'h2,
    SignedMessage_tag_Record = 3'h3,
    SignedMessage_tag_Tuple = 3'h4,
    SignedMessage_tag_Array = 3'h5,
    SignedMessage_tag_Error = 3'h6,
    SignedMessage_tag_Status = 3'h7
  } SignedMessage_tag_t;
  typedef struct packed {
    logic [15:0] xls_padding;
  } SignedMessage_none_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    logic signed [7:0] value;
  } SignedMessage_scalar_view_t;
  typedef struct packed {
    logic [14:0] xls_padding;
    logic signed value;
  } SignedMessage_bit_view_t;
  typedef struct packed {
    logic [6:0] xls_padding;
    SignedMessage_SignedRecord_value_t value;
  } SignedMessage_record_view_t;
  typedef struct packed {
    struct packed {
      logic signed [7:0] index_0;
      logic [7:0] index_1;
    } value;
  } SignedMessage_tuple_view_t;
  typedef struct packed {
    SignedMessage_s8_value_t [1:0] value;
  } SignedMessage_array_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    SignedMessage_SignedCode_value_t value;
  } SignedMessage_error_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    UnsignedCode value;
  } SignedMessage_status_view_t;
  typedef union packed {
    logic [15:0] bits;
    SignedMessage_none_view_t as_none;
    SignedMessage_scalar_view_t as_scalar;
    SignedMessage_bit_view_t as_bit;
    SignedMessage_record_view_t as_record;
    SignedMessage_tuple_view_t as_tuple;
    SignedMessage_array_view_t as_array;
    SignedMessage_error_view_t as_error;
    SignedMessage_status_view_t as_status;
  } SignedMessage_payload_t;
  typedef struct packed {
    SignedMessage_tag_t tag;
    SignedMessage_payload_t payload;
  } SignedMessage;

  function automatic SignedMessage SignedMessage_make_none ();
    begin
      SignedMessage_make_none = SignedMessage'({SignedMessage_tag_None, 16'h0000});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_scalar (input logic signed [7:0] value);
    begin
      SignedMessage_make_scalar = SignedMessage'({SignedMessage_tag_Scalar, 8'h00, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_bit (input logic signed value);
    begin
      SignedMessage_make_bit = SignedMessage'({SignedMessage_tag_Bit, 15'h0000, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_record (input SignedMessage_SignedRecord_value_t value);
    begin
      SignedMessage_make_record = SignedMessage'({SignedMessage_tag_Record, 7'h00, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_tuple (input struct packed {
    logic signed [7:0] index_0;
    logic [7:0] index_1;
  } value);
    begin
      SignedMessage_make_tuple = SignedMessage'({SignedMessage_tag_Tuple, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_array (input SignedMessage_s8_value_t [1:0] value);
    begin
      SignedMessage_make_array = SignedMessage'({SignedMessage_tag_Array, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_error (input SignedMessage_SignedCode_value_t value);
    begin
      SignedMessage_make_error = SignedMessage'({SignedMessage_tag_Error, 8'h00, value});
    end
  endfunction

  function automatic SignedMessage SignedMessage_make_status (input UnsignedCode value);
    begin
      SignedMessage_make_status = SignedMessage'({SignedMessage_tag_Status, 8'h00, value});
    end
  endfunction

  function automatic SignedMessage_tag_t SignedMessage_get_tag (input SignedMessage value);
    begin
      SignedMessage_get_tag = SignedMessage_tag_t'(value[18:16]);
    end
  endfunction
  typedef enum logic signed [7:0] {
    CodesAgain_SignedCode_enum_NEG = 8'hff,
    CodesAgain_SignedCode_enum_ZERO = 8'h00
  } CodesAgain_SignedCode_value_t;

  // DSLX Type: pub enum CodesAgain {
  //     None,
  //     Error(SignedCode),
  // }
  typedef enum logic {
    CodesAgain_tag_None = 1'h0,
    CodesAgain_tag_Error = 1'h1
  } CodesAgain_tag_t;
  typedef struct packed {
    logic [7:0] xls_padding;
  } CodesAgain_none_view_t;
  typedef struct packed {
    CodesAgain_SignedCode_value_t value;
  } CodesAgain_error_view_t;
  typedef union packed {
    logic [7:0] bits;
    CodesAgain_none_view_t as_none;
    CodesAgain_error_view_t as_error;
  } CodesAgain_payload_t;
  typedef struct packed {
    CodesAgain_tag_t tag;
    CodesAgain_payload_t payload;
  } CodesAgain;

  function automatic CodesAgain CodesAgain_make_none ();
    begin
      CodesAgain_make_none = CodesAgain'({CodesAgain_tag_None, 8'h00});
    end
  endfunction

  function automatic CodesAgain CodesAgain_make_error (input CodesAgain_SignedCode_value_t value);
    begin
      CodesAgain_make_error = CodesAgain'({CodesAgain_tag_Error, value});
    end
  endfunction

  function automatic CodesAgain_tag_t CodesAgain_get_tag (input CodesAgain value);
    begin
      CodesAgain_get_tag = CodesAgain_tag_t'(value[8:8]);
    end
  endfunction

  // DSLX Type: pub struct Token {
  //     x: u8,
  // }
  typedef struct packed {
    logic [7:0] x;
  } Token;

  // DSLX Type: pub struct ReusedShadowedRecord {
  //     Token: u8,
  //     data: Token,
  // }
  typedef struct packed {
    logic [7:0] Token__1;
    Token data;
  } ReusedShadowedRecord;
  typedef struct packed {
    logic signed [7:0] Token__1;
    Token data;
  } ShadowedMessage_ShadowedRecord_value_t;

  // DSLX Type: pub enum ShadowedMessage {
  //     None,
  //     Direct { Token: u8, data: Token },
  //     TypeOnlyElsewhere { token_value: u8 },
  //     Named(ShadowedRecord),
  //     Reused(ReusedShadowedRecord),
  // }
  typedef enum logic [2:0] {
    ShadowedMessage_tag_None = 3'h0,
    ShadowedMessage_tag_Direct = 3'h1,
    ShadowedMessage_tag_TypeOnlyElsewhere = 3'h2,
    ShadowedMessage_tag_Named = 3'h3,
    ShadowedMessage_tag_Reused = 3'h4
  } ShadowedMessage_tag_t;
  typedef struct packed {
    logic [15:0] xls_padding;
  } ShadowedMessage_none_view_t;
  typedef struct packed {
    logic [7:0] Token__1;
    Token data;
  } ShadowedMessage_direct_view_t;
  typedef struct packed {
    logic [7:0] xls_padding;
    logic [7:0] token_value;
  } ShadowedMessage_type_only_elsewhere_view_t;
  typedef struct packed {
    ShadowedMessage_ShadowedRecord_value_t value;
  } ShadowedMessage_named_view_t;
  typedef struct packed {
    ReusedShadowedRecord value;
  } ShadowedMessage_reused_view_t;
  typedef union packed {
    logic [15:0] bits;
    ShadowedMessage_none_view_t as_none;
    ShadowedMessage_direct_view_t as_direct;
    ShadowedMessage_type_only_elsewhere_view_t as_type_only_elsewhere;
    ShadowedMessage_named_view_t as_named;
    ShadowedMessage_reused_view_t as_reused;
  } ShadowedMessage_payload_t;
  typedef struct packed {
    ShadowedMessage_tag_t tag;
    ShadowedMessage_payload_t payload;
  } ShadowedMessage;

  function automatic ShadowedMessage ShadowedMessage_make_none ();
    begin
      ShadowedMessage_make_none = ShadowedMessage'({ShadowedMessage_tag_None, 16'h0000});
    end
  endfunction

  function automatic ShadowedMessage ShadowedMessage_make_direct (input logic [7:0] Token__1, input Token data);
    begin
      ShadowedMessage_make_direct = ShadowedMessage'({ShadowedMessage_tag_Direct, Token__1, data});
    end
  endfunction

  function automatic ShadowedMessage ShadowedMessage_make_type_only_elsewhere (input logic [7:0] token_value);
    begin
      ShadowedMessage_make_type_only_elsewhere = ShadowedMessage'({ShadowedMessage_tag_TypeOnlyElsewhere, 8'h00, token_value});
    end
  endfunction

  function automatic ShadowedMessage ShadowedMessage_make_named (input ShadowedMessage_ShadowedRecord_value_t value);
    begin
      ShadowedMessage_make_named = ShadowedMessage'({ShadowedMessage_tag_Named, value});
    end
  endfunction

  function automatic ShadowedMessage ShadowedMessage_make_reused (input ReusedShadowedRecord value);
    begin
      ShadowedMessage_make_reused = ShadowedMessage'({ShadowedMessage_tag_Reused, value});
    end
  endfunction

  function automatic ShadowedMessage_tag_t ShadowedMessage_get_tag (input ShadowedMessage value);
    begin
      ShadowedMessage_get_tag = ShadowedMessage_tag_t'(value[18:16]);
    end
  endfunction
endpackage
// verilog_lint: waive-end struct-union-name-style
