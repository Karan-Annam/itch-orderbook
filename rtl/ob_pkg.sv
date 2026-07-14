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
  // The full sizes are a simulation convenience: 2x 64 Mb async-read price
  // SRAM plus a 9.6 Mb hash table cannot exist in FPGA fabric (async read
  // maps only to LUTRAM, and every extra read port replicates the array).
  // Under synthesis (Vivado predefines SYNTHESIS) shrink to fit the Urbana
  // board's Spartan-7 XC7S50 (~600 Kb distributed RAM total).
  //
  // The book is PRICE-BANDED: level address = price - band_base, where
  // band_base auto-centers on the first Add (BAND_AUTO_INIT) and is fixed
  // thereafter. Events priced outside [band_base, band_base+PRICE_LEVELS)
  // are dropped whole and counted (band_drops) — never partially applied.
  // In simulation the window spans the generator's entire price space and
  // band_base stays 0, so behaviour is identical to the unbanded book; the
  // banded/synthesis config is exercised by test_banding against a second
  // Verilated model built with +define+SYNTHESIS.
`ifdef SYNTHESIS
  // Table sizing: with insert-at-remembered-tomb (order_ref_table) the table
  // saturates to live-or-tomb under churn and keeps working, so capacity is
  // bounded by peak LIVE orders (~1k on the real AAPL excerpt), not by
  // cumulative churn. 4096 slots x 147b ~= 17 RAMB36; 8192 doubled the BRAM
  // column spread and lost 1.2 ns of post-route slack at 100 MHz. A nonzero
  // tbl_ins_fails means this is undersized for the stream.
  localparam int TABLE_SIZE   = 1 << 12;
  localparam int HASH_W       = 12;
  localparam int PRICE_LEVELS = 1 << 10;
  localparam int LEVEL_AW     = 10;
  localparam bit BAND_AUTO_INIT = 1'b1;  // center band on the first Add
`else
  localparam int TABLE_SIZE   = 1 << 16; // 64K order-ref hash slots
  localparam int HASH_W       = 16;
  localparam int PRICE_LEVELS = 1 << 20; // covers all generator prices
  localparam int LEVEL_AW     = 20;
  localparam bit BAND_AUTO_INIT = 1'b0;  // band_base pinned to 0
`endif
  localparam int MAX_PROBE  = 64;        // linear-probe bound

  localparam int MSG_MAX_BYTES = 48;     // Trade('P') body = 46; pad to 48

  // ---- wide ingest path -----------------------------------------------------
  localparam int WORD_BYTES = 16;                    // ingest bus bytes per beat
  localparam int WORD_W     = WORD_BYTES * BYTE_W;   // 128
  localparam int NBYTES_W   = 5;                     // holds 1..16
  localparam int MSG_MAX_WORDS = (MSG_MAX_BYTES + WORD_BYTES - 1) / WORD_BYTES;
  localparam int WIDX_W     = 2;                     // body word index 0..2
  // The minimum frame (2-byte prefix + 14-byte System body) equals WORD_BYTES,
  // so at most one message can end within any single ingest word.

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

  // Fibonacci-style multiplicative hash (one DSP, comb between registers).
  // The original XOR fold mapped real NASDAQ order refs — which are
  // near-sequential — onto dense runs of ADJACENT slots, exactly what
  // defeats linear probing: on real data the probe chains saturated and
  // inserts failed (caught by tbl_ins_fails). The golden-ratio multiply
  // scatters sequential inputs; top HASH_W bits of the mod-2^25 product.
  // Mirrored in C++ by sim/tests/test_order_ref_table.cpp — keep in sync.
  function automatic logic [HASH_W-1:0] hash_ref(input logic [REF_W-1:0] r);
    logic [31:0] x;
    logic [24:0] y, p;
    x = r[31:0] ^ r[63:32];
    y = x[24:0] ^ {x[31:25], 18'b0};
    p = 25'(y * 25'h13C6EF5);
    hash_ref = HASH_W'(p >> (25 - HASH_W));
  endfunction

  // order-ref table operations
  localparam logic [1:0] OP_INSERT = 2'd0;
  localparam logic [1:0] OP_LOOKUP = 2'd1;
  localparam logic [1:0] OP_DELETE = 2'd2;

endpackage

`endif
