// Package: parameters, ITCH wire layout, shared types.
//
// The layouts here mirror sw/parser/itch_messages.hpp field for field — that's
// the contract that lets the end-to-end diff compare hardware book state
// against the C++ reference. Common 13-byte header:
//   [0] type, [1:2] stock_locate(BE), [3:4] tracking(BE), [5:12] timestamp(BE,8B)
// then type-specific fields, all big-endian. Byte-swap in hardware is just
// wiring the bytes in reverse order.
//
// Notes:
//  * Behavioral memories use asynchronous (combinational) reads — valid
//    synthesizable LUTRAM; a real FPGA build should re-map them to registered
//    BRAM with an extra pipeline stage.
//  * Price levels are direct-indexed by raw price (price = address). One book
//    is instantiated (single symbol); the harness feeds a single-symbol stream
//    so hardware and the reference model agree exactly.
`ifndef OB_PKG_SV
`define OB_PKG_SV

package ob_pkg;

  // ---- widths -------------------------------------------------------------
  localparam int BYTE_W    = 8;
  localparam int REF_W     = 64;
  localparam int PRICE_W   = 32;
  localparam int SHARES_W  = 32;
  localparam int LOCATE_W  = 16;

  // ---- table / book sizing ------------------------------------------------
  localparam int TABLE_SIZE = 1 << 16;   // 64K order-ref hash slots
  localparam int HASH_W     = 16;
  localparam int MAX_PROBE  = 64;        // linear-probe bound

  localparam int PRICE_LEVELS = 1 << 20; // direct-indexed price array
  localparam int LEVEL_AW      = 20;

  localparam int MSG_MAX_BYTES = 48;     // Trade('P') body = 46; pad to 48

  // ---- ITCH message type codes (ASCII) ------------------------------------
  localparam logic [7:0] T_ADD       = "A";
  localparam logic [7:0] T_ADD_MPID  = "F";
  localparam logic [7:0] T_EXEC      = "E";
  localparam logic [7:0] T_EXEC_PR   = "C";
  localparam logic [7:0] T_CANCEL    = "X";
  localparam logic [7:0] T_DELETE    = "D";
  localparam logic [7:0] T_REPLACE   = "U";
  localparam logic [7:0] T_TRADE     = "P";
  localparam logic [7:0] T_SYSTEM    = "S";

  localparam logic [7:0] SIDE_BUY  = "B";
  localparam logic [7:0] SIDE_SELL = "S";

  // ---- per-type body lengths (bytes) — must match blen:: in C++ -----------
  function automatic int unsigned body_len(input logic [7:0] t);
    case (t)
      T_ADD:      body_len = 38;
      T_ADD_MPID: body_len = 42;
      T_EXEC:     body_len = 33;
      T_EXEC_PR:  body_len = 38;
      T_CANCEL:   body_len = 25;
      T_DELETE:   body_len = 21;
      T_REPLACE:  body_len = 37;
      T_TRADE:    body_len = 46;
      T_SYSTEM:   body_len = 14;
      default:    body_len = 0;
    endcase
  endfunction

  // ---- normalized decoded message (engine-facing) -------------------------
  typedef struct packed {
    logic [7:0]            mtype;
    logic                  is_bid;        // side byte == 'B'
    logic                  printable;     // 'C' message printable flag
    logic [LOCATE_W-1:0]   stock_locate;
    logic [PRICE_W-1:0]    price;         // add/exec(C)/replace-new/trade price
    logic [SHARES_W-1:0]   shares;        // add/exec/cancel/replace-new/trade qty
    logic [REF_W-1:0]      order_ref;     // primary ref (orig ref for Replace)
    logic [REF_W-1:0]      new_order_ref; // Replace new ref
    logic [63:0]           timestamp;
  } decoded_t;

  // ---- order-ref hash table entry -----------------------------------------
  // `tomb` marks a slot whose order was deleted: a lookup must probe PAST it (it
  // does not terminate the search) so other keys in the same collision chain
  // stay reachable. Only a slot that is neither valid nor tomb is a true empty
  // that stops a probe. (The first version deleted by writing the slot empty,
  // which orphans colliding keys stored beyond it — see docs/DESIGN.md.)
  typedef struct packed {
    logic                valid;
    logic                tomb;
    logic                is_bid;
    logic [LOCATE_W-1:0] locate;
    logic [SHARES_W-1:0] shares;
    logic [PRICE_W-1:0]  price;
    logic [REF_W-1:0]    oref;
  } order_entry_t;

  // ---- price level entry (total_shares, order_count) ----------------------
  typedef struct packed {
    logic [31:0] total_shares;
    logic [31:0] order_count;
  } level_t;

  // hardware-friendly XOR-fold hash: pure wires.
  function automatic logic [HASH_W-1:0] hash_ref(input logic [REF_W-1:0] r);
    hash_ref = r[63:48] ^ r[47:32] ^ r[31:16] ^ r[15:0];
  endfunction

  // order-ref table operations
  localparam logic [1:0] OP_INSERT = 2'd0;
  localparam logic [1:0] OP_LOOKUP = 2'd1;
  localparam logic [1:0] OP_DELETE = 2'd2;

endpackage

`endif
