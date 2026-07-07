// The full pipeline:
//
//   in_byte -> udp_stripper -> itch_framer -> itch_decoder
//           -> book_update_engine (ref table + price SRAMs + best trackers)
//           -> stats_engine / perf_counters
//
// One byte per cycle while in_ready is high; in_ready drops while a message is
// being applied (Replace, best-price scans). Multi-field outputs are flattened
// to scalars at this boundary so the Verilated C++ harness can read them.
module orderbook_top
  import ob_pkg::*;
(
  input  logic       clk,
  input  logic       rst,
  input  logic       raw_mode,    // 1 = input is raw ITCH (no UDP wrapper)

  input  logic [7:0] in_byte,
  input  logic       in_valid,
  input  logic       in_sop,      // first byte of a UDP frame (UDP mode only)
  output logic       in_ready,

  // ---- book state (testbench diff + inspection) --------------------------
  output logic                best_bid_valid,
  output logic [PRICE_W-1:0]  best_bid_price,
  output logic [31:0]         best_bid_shares,
  output logic                best_ask_valid,
  output logic [PRICE_W-1:0]  best_ask_price,
  output logic [31:0]         best_ask_shares,
  output logic [63:0]         tot_bid_vol,
  output logic [63:0]         tot_ask_vol,

  input  logic                dbg_is_bid,
  input  logic [PRICE_W-1:0]  dbg_price,
  output logic [31:0]         dbg_shares,
  output logic [31:0]         dbg_count,

  // ---- statistics --------------------------------------------------------
  output logic [63:0]         vwap_num,
  output logic [63:0]         vwap_den,
  output logic                spread_valid,
  output logic [PRICE_W-1:0]  spread,
  output logic signed [31:0]  imbalance_q16,
  output logic [63:0]         trade_count,
  output logic [31:0]         msg_rate,

  // ---- performance counters (flattened) ----------------------------------
  output logic [31:0]         mc_add,
  output logic [31:0]         mc_addmpid,
  output logic [31:0]         mc_exec,
  output logic [31:0]         mc_execpr,
  output logic [31:0]         mc_cancel,
  output logic [31:0]         mc_delete,
  output logic [31:0]         mc_replace,
  output logic [31:0]         mc_trade,
  output logic [31:0]         mc_system,
  output logic [63:0]         add_cycles,
  output logic [63:0]         delete_cycles,
  output logic [63:0]         replace_cycles,
  output logic [63:0]         scan_count,
  output logic [63:0]         scan_cycles_total,
  output logic [63:0]         hash_probe_1,
  output logic [63:0]         hash_probe_2,
  output logic [63:0]         hash_probe_gt2,
  output logic [63:0]         pipeline_stall_cycles,
  output logic [63:0]         cycle_count,
  output logic [63:0]         msg_total,

  // ---- decoded-message tap (for decode-only tests) -----------------------
  output logic                dec_tap_valid,
  output logic [7:0]          dec_tap_type,
  output logic                dec_tap_is_bid,
  output logic                dec_tap_printable,
  output logic [PRICE_W-1:0]  dec_tap_price,
  output logic [31:0]         dec_tap_shares,
  output logic [REF_W-1:0]    dec_tap_ref,
  output logic [REF_W-1:0]    dec_tap_new_ref
);

  // stripper -> framer
  logic [7:0] strip_byte;  logic strip_valid, strip_ready;
  // framer -> decoder
  logic [7:0] f_byte;      logic f_valid;  logic [5:0] f_offset;
  logic       f_start, f_complete;  logic [15:0] f_len;
  // decoder -> engine
  decoded_t   dec;         logic dec_valid, dec_accept;
  // engine -> framer backpressure / telemetry
  logic       engine_done, engine_busy;
  logic       trade_valid; logic [PRICE_W-1:0] trade_price; logic [31:0] trade_shares;
  logic       ev_done;     logic [7:0] ev_type;
  logic       any_scanning, in_replace, lk_p1, lk_p2, lk_pg2;
  logic [31:0] msg_count_arr [9];

  assign dec_tap_valid     = dec_valid;
  assign dec_tap_type      = dec.mtype;
  assign dec_tap_is_bid    = dec.is_bid;
  assign dec_tap_printable = dec.printable;
  assign dec_tap_price     = dec.price;
  assign dec_tap_shares    = dec.shares;
  assign dec_tap_ref       = dec.order_ref;
  assign dec_tap_new_ref   = dec.new_order_ref;

  assign mc_add     = msg_count_arr[0];
  assign mc_addmpid = msg_count_arr[1];
  assign mc_exec    = msg_count_arr[2];
  assign mc_execpr  = msg_count_arr[3];
  assign mc_cancel  = msg_count_arr[4];
  assign mc_delete  = msg_count_arr[5];
  assign mc_replace = msg_count_arr[6];
  assign mc_trade   = msg_count_arr[7];
  assign mc_system  = msg_count_arr[8];

  udp_stripper u_strip (
    .clk(clk), .rst(rst), .raw_mode(raw_mode),
    .in_byte(in_byte), .in_valid(in_valid), .in_sop(in_sop), .in_ready(in_ready),
    .out_byte(strip_byte), .out_valid(strip_valid), .out_ready(strip_ready)
  );

  itch_framer u_fr (
    .clk(clk), .rst(rst),
    .in_byte(strip_byte), .in_valid(strip_valid), .in_ready(strip_ready),
    .msg_byte(f_byte), .msg_byte_valid(f_valid), .msg_offset(f_offset),
    .msg_start(f_start), .msg_complete(f_complete), .msg_len(f_len),
    .engine_done(engine_done)
  );

  itch_decoder u_dec (
    .clk(clk), .rst(rst),
    .msg_byte(f_byte), .msg_byte_valid(f_valid), .msg_offset(f_offset),
    .msg_complete(f_complete), .dec_accept(dec_accept),
    .dec(dec), .dec_valid(dec_valid)
  );

  book_update_engine u_eng (
    .clk(clk), .rst(rst),
    .dec(dec), .dec_valid(dec_valid), .dec_accept(dec_accept),
    .engine_done(engine_done), .busy(engine_busy),
    .best_bid_valid(best_bid_valid), .best_bid_price(best_bid_price),
    .best_bid_shares(best_bid_shares),
    .best_ask_valid(best_ask_valid), .best_ask_price(best_ask_price),
    .best_ask_shares(best_ask_shares),
    .tot_bid_vol(tot_bid_vol), .tot_ask_vol(tot_ask_vol),
    .dbg_is_bid(dbg_is_bid), .dbg_price(dbg_price),
    .dbg_shares(dbg_shares), .dbg_count(dbg_count),
    .trade_valid(trade_valid), .trade_price(trade_price), .trade_shares(trade_shares),
    .ev_done(ev_done), .ev_type(ev_type),
    .any_scanning(any_scanning), .in_replace(in_replace),
    .lk_probe1(lk_p1), .lk_probe2(lk_p2), .lk_probe_gt2(lk_pg2)
  );

  stats_engine u_stats (
    .clk(clk), .rst(rst),
    .trade_valid(trade_valid), .trade_price(trade_price), .trade_shares(trade_shares),
    .best_bid_valid(best_bid_valid), .best_bid_price(best_bid_price),
    .best_ask_valid(best_ask_valid), .best_ask_price(best_ask_price),
    .tot_bid_vol(tot_bid_vol), .tot_ask_vol(tot_ask_vol),
    .msg_commit(ev_done),
    .vwap_num(vwap_num), .vwap_den(vwap_den),
    .spread_valid(spread_valid), .spread(spread),
    .imbalance_q16(imbalance_q16), .trade_count(trade_count), .msg_rate(msg_rate)
  );

  perf_counters u_perf (
    .clk(clk), .rst(rst),
    .ev_done(ev_done), .ev_type(ev_type), .busy(engine_busy),
    .any_scanning(any_scanning), .in_replace(in_replace),
    .lk_probe1(lk_p1), .lk_probe2(lk_p2), .lk_probe_gt2(lk_pg2),
    .msg_count(msg_count_arr),
    .add_cycles(add_cycles), .delete_cycles(delete_cycles),
    .replace_cycles(replace_cycles), .scan_count(scan_count),
    .scan_cycles_total(scan_cycles_total),
    .hash_probe_1(hash_probe_1), .hash_probe_2(hash_probe_2),
    .hash_probe_gt2(hash_probe_gt2),
    .pipeline_stall_cycles(pipeline_stall_cycles),
    .cycle_count(cycle_count), .msg_total(msg_total)
  );

endmodule
