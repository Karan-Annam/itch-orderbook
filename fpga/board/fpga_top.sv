// Board top for the ITCH order book — RealDigital Urbana (xc7s50) or Digilent
// Arty A7-100 (xc7a100t), selected by the bitstream flow's XDC. The host PC
// streams ITCH messages and polls book-state snapshots over the USB-UART
// (OBLink framing, see uart_itch_bridge.sv); the entire ingest -> framer ->
// decoder -> banded book -> stats pipeline runs as real hardware at 100 MHz.
//
// LEDs: 0 heartbeat, 1 host-frame-accepted (stretched), 2 snapshot-sent
// (stretched), 3 sticky attention = UART resync happened OR the band dropped
// an out-of-window event (either means host data didn't all land in the book).
//
// Under Verilator (`VERILATOR` define) the MMCM is bypassed (clk100 drives
// the core directly) so the exact bitstream datapath — UART bits, bridge,
// banded book — simulates before hardware exists (fpga/board/test_board.cpp).
module fpga_top #(
  // core_clk / baud: 868 = 115200 (bring-up), 100 = 1 Mbaud (demo). The
  // bitstream flow overrides via -verilog_define UART_DIVISOR_OVR=N; the
  // board sim overrides the parameter directly with -GUART_DIVISOR.
`ifdef UART_DIVISOR_OVR
  parameter int unsigned UART_DIVISOR = `UART_DIVISOR_OVR
`else
  parameter int unsigned UART_DIVISOR = 868
`endif
)(
  input  logic clk100,      // 100 MHz board oscillator
  input  logic ck_rstn,     // board reset button (see RESET_ACTIVE_HIGH)
  input  logic uart_rx,     // host -> FPGA
  output logic uart_tx,     // FPGA -> host
  output logic [3:0] led
);
  import ob_pkg::*;

  // Arty's ck_rst button is active-low; Urbana's BTN0 is active-high
  // (pressed = 1). The bitstream flow defines RESET_ACTIVE_HIGH for Urbana.
  logic rstn_in;
`ifdef RESET_ACTIVE_HIGH
  assign rstn_in = !ck_rstn;
`else
  assign rstn_in = ck_rstn;
`endif

  // ---------------- clocking ----------------
  logic clk_core, mmcm_locked;

`ifdef VERILATOR
  assign clk_core    = clk100;
  assign mmcm_locked = 1'b1;
`else
  // 100 MHz x 9.0 / 9.0 = 100 MHz (VCO 900 MHz, legal for -1 speed grade on
  // both parts). The MMCM buys a LOCKED-gated reset release and a clean spot
  // to retune the core clock later.
  logic clk_fb, clk_core_unbuf;
  MMCME2_BASE #(
    .CLKIN1_PERIOD   (10.000),
    .CLKFBOUT_MULT_F (9.000),
    .DIVCLK_DIVIDE   (1),
    .CLKOUT0_DIVIDE_F(9.000)
  ) u_mmcm (
    .CLKIN1   (clk100),
    .CLKFBIN  (clk_fb),
    .CLKFBOUT (clk_fb),
    .CLKOUT0  (clk_core_unbuf),
    .CLKOUT0B (), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(),
    .CLKOUT3  (), .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
    .CLKFBOUTB(), .LOCKED(mmcm_locked), .PWRDWN(1'b0), .RST(1'b0)
  );
  BUFG u_bufg (.I(clk_core_unbuf), .O(clk_core));
`endif

  // ---------------- reset: fully synchronous inside the core domain --------
  // FPGA configuration initializes this pipe to zero. Keeping the distributed
  // reset synchronous prevents asynchronously-reset state from feeding BRAM
  // address/control pins, which Vivado flags as a possible corruption hazard.
  logic [2:0] rst_sync_q;
  (* max_fanout = 256 *) logic rst_n;
  assign rst_n = rst_sync_q[2];
  always_ff @(posedge clk_core) begin
    if (!rstn_in || !mmcm_locked)
      rst_sync_q <= '0;
    else
      rst_sync_q <= {rst_sync_q[1:0], 1'b1};
  end

  // ---------------- bridge + core ----------------
  logic [WORD_W-1:0]   in_data;
  logic [NBYTES_W-1:0] in_nbytes;
  logic                in_valid, in_ready;
  logic                band_cfg_valid;
  logic [31:0]         band_cfg_base;
  logic                rx_pulse, tx_pulse, frame_err;

  logic        best_bid_valid, best_ask_valid, spread_valid;
  logic [31:0] best_bid_price, best_bid_shares, best_ask_price, best_ask_shares;
  logic [63:0] tot_bid_vol, tot_ask_vol, vwap_num, vwap_den, trade_count, msg_total;
  logic [31:0] spread, band_base, band_drops;
  logic [31:0] mc_add, mc_addmpid, mc_exec, mc_execpr, mc_cancel;
  logic [31:0] mc_delete, mc_replace, mc_trade, mc_system;
  logic [31:0] tbl_ins_fails;

  uart_itch_bridge #(.DIVISOR(UART_DIVISOR)) u_bridge (
    .clk(clk_core), .rst_n(rst_n),
    .uart_rx(uart_rx), .uart_tx(uart_tx),
    .in_data(in_data), .in_nbytes(in_nbytes),
    .in_valid(in_valid), .in_ready(in_ready),
    .band_cfg_valid(band_cfg_valid), .band_cfg_base(band_cfg_base),
    .best_bid_valid(best_bid_valid), .best_bid_price(best_bid_price),
    .best_bid_shares(best_bid_shares),
    .best_ask_valid(best_ask_valid), .best_ask_price(best_ask_price),
    .best_ask_shares(best_ask_shares),
    .tot_bid_vol(tot_bid_vol), .tot_ask_vol(tot_ask_vol),
    .vwap_num(vwap_num), .vwap_den(vwap_den), .trade_count(trade_count),
    .spread_valid(spread_valid), .spread(spread),
    .band_base(band_base), .band_drops(band_drops), .msg_total(msg_total),
    .mc_add(mc_add), .mc_addmpid(mc_addmpid), .mc_exec(mc_exec),
    .mc_execpr(mc_execpr), .mc_cancel(mc_cancel), .mc_delete(mc_delete),
    .mc_replace(mc_replace), .mc_trade(mc_trade), .mc_system(mc_system),
    .tbl_ins_fails(tbl_ins_fails),
    .rx_frame_pulse(rx_pulse), .tx_frame_pulse(tx_pulse),
    .frame_err_sticky(frame_err)
  );

  orderbook_top u_ob (
    .clk(clk_core), .rst(!rst_n),
    .raw_mode(1'b1),                   // OBLink frames carry bare ITCH messages
    .mold_mode(1'b0),
    .in_data(in_data), .in_nbytes(in_nbytes), .in_valid(in_valid),
    .in_sop(1'b0), .in_ready(in_ready),
    .band_cfg_valid(band_cfg_valid), .band_cfg_base(band_cfg_base),
    .best_bid_valid(best_bid_valid), .best_bid_price(best_bid_price),
    .best_bid_shares(best_bid_shares),
    .best_ask_valid(best_ask_valid), .best_ask_price(best_ask_price),
    .best_ask_shares(best_ask_shares),
    .tot_bid_vol(tot_bid_vol), .tot_ask_vol(tot_ask_vol),
    .band_base(band_base), .band_drops(band_drops),
    .mold_next_seq(), .mold_gap_events(), .mold_gap_msgs(), .mold_session_end(),
    .dbg_is_bid(1'b0), .dbg_price('0), .dbg_shares(), .dbg_count(),
    .vwap_num(vwap_num), .vwap_den(vwap_den),
    .spread_valid(spread_valid), .spread(spread),
    .imbalance_q16(), .trade_count(trade_count), .msg_rate(),
    .mc_add(mc_add), .mc_addmpid(mc_addmpid), .mc_exec(mc_exec),
    .mc_execpr(mc_execpr), .mc_cancel(mc_cancel), .mc_delete(mc_delete),
    .mc_replace(mc_replace), .mc_trade(mc_trade), .mc_system(mc_system),
    .tbl_ins_fails(tbl_ins_fails),
    .add_cycles(), .delete_cycles(), .replace_cycles(),
    .scan_count(), .scan_cycles_total(),
    .hash_probe_1(), .hash_probe_2(), .hash_probe_gt2(),
    .pipeline_stall_cycles(), .ingest_stall_cycles(),
    .cycle_count(), .msg_total(msg_total),
    .dec_tap_valid(), .dec_tap_type(), .dec_tap_is_bid(), .dec_tap_printable(),
    .dec_tap_price(), .dec_tap_shares(), .dec_tap_ref(), .dec_tap_new_ref()
  );

  // ---------------- LEDs ----------------
  logic [26:0] hb_q;
  logic [22:0] rx_stretch_q, tx_stretch_q;
  always_ff @(posedge clk_core) begin
    if (!rst_n) begin
      hb_q <= '0; rx_stretch_q <= '0; tx_stretch_q <= '0;
    end else begin
      hb_q <= hb_q + 1'b1;
      rx_stretch_q <= rx_pulse ? '1 : (rx_stretch_q != 0 ? rx_stretch_q - 1'b1 : '0);
      tx_stretch_q <= tx_pulse ? '1 : (tx_stretch_q != 0 ? tx_stretch_q - 1'b1 : '0);
    end
  end
  assign led[0] = hb_q[26];             // ~0.7 Hz heartbeat at 100 MHz
  assign led[1] = (rx_stretch_q != 0);  // host frame accepted
  assign led[2] = (tx_stretch_q != 0);  // snapshot sent
  assign led[3] = frame_err || (band_drops != 0);  // sticky attention

endmodule
