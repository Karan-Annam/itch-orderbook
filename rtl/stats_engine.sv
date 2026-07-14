// Running market statistics.
//   VWAP           : 64-bit sum(price*shares) / sum(shares) over trade events (P,E,C)
//   spread         : best_ask - best_bid, valid when both sides present
//   imbalance      : (bid_vol - ask_vol) / (bid_vol + ask_vol), signed Q16.
//                    Computed over FULL per-side resting volume (kept O(1) by the
//                    engine's incremental volume tracking) rather than top-N levels.
//   trade count, message rate (RATE_WINDOW cycles), top-of-book depth
module stats_engine
  import ob_pkg::*;
#(
  parameter int RATE_WINDOW = 4096
)(
  input  logic                clk,
  input  logic                rst,

  // unified trade events
  input  logic                trade_valid,
  input  logic [PRICE_W-1:0]  trade_price,
  input  logic [31:0]         trade_shares,

  // book top state
  input  logic                best_bid_valid,
  input  logic [PRICE_W-1:0]  best_bid_price,
  input  logic                best_ask_valid,
  input  logic [PRICE_W-1:0]  best_ask_price,
  input  logic [63:0]         tot_bid_vol,
  input  logic [63:0]         tot_ask_vol,

  // message commit pulse (for message rate)
  input  logic                msg_commit,

  output logic [63:0]         vwap_num,
  output logic [63:0]         vwap_den,
  output logic                spread_valid,
  output logic [PRICE_W-1:0]  spread,
  output logic signed [31:0]  imbalance_q16,
  output logic [63:0]         trade_count,
  output logic [31:0]         msg_rate
);

  // VWAP + trade count.
  // Two stages: the 32x32 product is registered before the 64-bit accumulate.
  // A single-cycle multiply-accumulate was the design's only failing path at
  // 100 MHz on Spartan-7; the product register folds into the DSP48 pipeline.
  // num/den/count all update from the staged event so a reader never sees a
  // torn num/den pair, and the staged update lands no later than the edge
  // where the commit becomes observable (perf_counters registers ev_done,
  // which trails trade_valid by >=1 cycle), so per-commit testbench diffs
  // still hold.
  logic        tv_q;
  logic [63:0] prod_q, sh_q;
  always_ff @(posedge clk) begin
    if (rst) begin
      tv_q <= 1'b0;
    end else begin
      tv_q   <= trade_valid;
      prod_q <= 64'(trade_price) * 64'(trade_shares);
      sh_q   <= 64'(trade_shares);
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      vwap_num    <= '0;
      vwap_den    <= '0;
      trade_count <= '0;
    end else if (tv_q) begin
      vwap_num    <= vwap_num + prod_q;
      vwap_den    <= vwap_den + sh_q;
      trade_count <= trade_count + 1'b1;
    end
  end

  // spread (combinational)
  assign spread_valid = best_bid_valid && best_ask_valid;
  assign spread       = spread_valid ? (best_ask_price - best_bid_price) : 32'd0;

  // order-flow imbalance as signed Q16 fixed point (combinational)
  logic [63:0] vsum, vdiff_abs, mag;
  logic        neg;
  always_comb begin
    vsum = tot_bid_vol + tot_ask_vol;
    if (tot_bid_vol >= tot_ask_vol) begin
      vdiff_abs = tot_bid_vol - tot_ask_vol; neg = 1'b0;
    end else begin
      vdiff_abs = tot_ask_vol - tot_bid_vol; neg = 1'b1;
    end
    mag = (vsum == 0) ? 64'd0 : ((vdiff_abs <<< 16) / vsum);  // |imbalance| Q16
    imbalance_q16 = neg ? -$signed(mag[31:0]) : $signed(mag[31:0]);
  end

  // rolling message rate over RATE_WINDOW cycles
  logic [31:0] win_cnt, win_timer;
  always_ff @(posedge clk) begin
    if (rst) begin
      win_cnt   <= '0;
      win_timer <= '0;
      msg_rate  <= '0;
    end else begin
      if (win_timer == RATE_WINDOW-1) begin
        msg_rate  <= win_cnt + (msg_commit ? 32'd1 : 32'd0);
        win_cnt   <= '0;
        win_timer <= '0;
      end else begin
        win_timer <= win_timer + 1'b1;
        if (msg_commit) win_cnt <= win_cnt + 1'b1;
      end
    end
  end

endmodule
