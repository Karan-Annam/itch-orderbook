// Incremental best-price tracker for one side of the book. Instantiated twice:
// bids (best = highest, scans down) and asks (best = lowest, scans up).
//
// Works entirely in LEVEL-ADDRESS space (price - band_base). Because the band
// base is fixed after init, address order == price order, so the engine can
// reconstruct best_price = band_base + best_addr.
//
// An Add that beats the current best updates it in one cycle. Emptying the
// best level triggers a scan of the price SRAM, one level per cycle through
// the engine's read port, until the next non-empty level — unless the engine
// says the whole side is empty (side_nonempty=0), in which case the scan is
// skipped outright.
module best_tracker
  import ob_pkg::*;
#(
  parameter bit IS_BID = 1'b1
)(
  input  logic                 clk,
  input  logic                 rst,

  // incremental add update
  input  logic                 add_en,
  input  logic [LEVEL_AW-1:0]  add_addr,

  // top-of-book level emptied at this address
  input  logic                 empt_en,
  input  logic [LEVEL_AW-1:0]  empt_addr,
  input  logic                 side_nonempty,  // any resting order remains this side

  // scan read port into the price SRAM
  output logic [LEVEL_AW-1:0]  scan_rd_addr,
  input  logic [SHARES_W-1:0]  scan_rd_shares,

  output logic                 scanning,
  output logic                 best_valid,
  output logic [LEVEL_AW-1:0]  best_addr
);

  typedef enum logic [0:0] {S_IDLE, S_SCAN} state_t;
  state_t              state;
  logic [LEVEL_AW-1:0] scan_idx;

  assign scanning     = (state == S_SCAN);
  assign scan_rd_addr = scan_idx;

  always_ff @(posedge clk) begin
    if (rst) begin
      state      <= S_IDLE;
      best_valid <= 1'b0;
      best_addr  <= '0;
      scan_idx   <= '0;
    end else begin
      case (state)
        S_IDLE: begin
          // add update has priority and is independent of scans
          if (add_en) begin
            if (!best_valid ||
                ( IS_BID && add_addr > best_addr) ||
                (!IS_BID && add_addr < best_addr)) begin
              best_addr  <= add_addr;
              best_valid <= 1'b1;
            end
          end
          if (empt_en && best_valid && (empt_addr == best_addr)) begin
            if (!side_nonempty) begin
              best_valid <= 1'b0;        // side fully empty: O(1), no scan
            end else begin
              // begin scan from the adjacent level toward the book interior
              scan_idx <= IS_BID ? (best_addr - 1'b1)
                                 : (best_addr + 1'b1);
              state    <= S_SCAN;
            end
          end
        end

        S_SCAN: begin
          if (scan_rd_shares != 0) begin
            best_addr  <= scan_idx;
            best_valid <= 1'b1;
            state      <= S_IDLE;
          end else if (( IS_BID && scan_idx == '0) ||
                       (!IS_BID && scan_idx == LEVEL_AW'(PRICE_LEVELS-1))) begin
            best_valid <= 1'b0;          // reached boundary: side empty
            state      <= S_IDLE;
          end else begin
            scan_idx <= IS_BID ? (scan_idx - 1'b1) : (scan_idx + 1'b1);
          end
        end

        default: state <= S_IDLE;
      endcase
    end
  end

endmodule
