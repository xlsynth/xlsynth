// verilog_lint: waive-start struct-union-name-style
package semantic_sum;
  // DSLX Type: pub enum MaybeWord {
  //     None,
  //     Some(u32),
  //     Pair { lo: u8, hi: u8 },
  // }
  typedef struct packed {
    logic [1:0] tag;
    logic [31:0] payload;
  } MaybeWord;

  // DSLX Type: pub enum ExplicitTagWidth : u5 {
  //     None = 0,
  //     Some(u8) = 1,
  // }
  typedef struct packed {
    logic [4:0] tag;
    logic [7:0] payload;
  } ExplicitTagWidth;

  // DSLX Type: pub enum Singleton {
  //     Only(u16),
  // }
  typedef struct packed {
    logic [15:0] payload;
  } Singleton;
endpackage
// verilog_lint: waive-end struct-union-name-style
