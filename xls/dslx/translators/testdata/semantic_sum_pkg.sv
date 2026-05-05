// verilog_lint: waive-start struct-union-name-style
package semantic_sum;
  // DSLX Type: pub enum MaybeWord {
  //     None,
  //     Some(u32),
  //     Pair { lo: u8, hi: u8 },
  // }
  typedef struct packed {
    logic [1:0] tag;
    logic [31:0] payload_0;
    logic [7:0] payload_1;
    logic [7:0] payload_2;
  } MaybeWord;
endpackage
// verilog_lint: waive-end struct-union-name-style
