// Applies decoded ITCH events to the book. Owns the direct price-indexed level
// SRAMs (bid_mem/ask_mem, async read), the order_ref_table, and two
// best_trackers. A single FSM serializes each message; the framer holds input
// (engine_done backpressure) until it's fully applied. Replace is
// remove-then-add on the original side.
//
// Trade-event note: Execute(E) carries no price, so after the table lookup the
// engine emits the trade at the order's RESTING price; Execute-with-price(C)
// uses the message's execution price (printable only); Trade(P) passes through.
// These feed VWAP. Running per-side volume feeds the imbalance stat.
module book_update_engine
  import ob_pkg::*;
(
  input  logic       clk,
  input  logic       rst,

  input  decoded_t   dec,
  input  logic       dec_valid,
  output logic       dec_accept,   // pulse: message latched (clears dec_valid)
  output logic       engine_done,
  output logic       busy,

  // book state (for the testbench diff and stats)
  output logic                best_bid_valid,
  output logic [PRICE_W-1:0]  best_bid_price,
  output logic [31:0]         best_bid_shares,
  output logic                best_ask_valid,
  output logic [PRICE_W-1:0]  best_ask_price,
  output logic [31:0]         best_ask_shares,
  output logic [63:0]         tot_bid_vol,
  output logic [63:0]         tot_ask_vol,

  // debug/query level read port (async): returns shares & order_count
  input  logic                dbg_is_bid,
  input  logic [PRICE_W-1:0]  dbg_price,
  output logic [31:0]         dbg_shares,
  output logic [31:0]         dbg_count,

  // unified trade event (to stats_engine: VWAP, trade count)
  output logic                trade_valid,
  output logic [PRICE_W-1:0]  trade_price,
  output logic [31:0]         trade_shares,

  // telemetry to perf_counters
  output logic                ev_done,
  output logic [7:0]          ev_type,
  output logic                any_scanning,
  output logic                in_replace,
  output logic                lk_probe1,
  output logic                lk_probe2,
  output logic                lk_probe_gt2
);

  // ---- price-level SRAMs --------------------------------------------------
  level_t bid_mem [PRICE_LEVELS];
  level_t ask_mem [PRICE_LEVELS];

  initial begin
    for (int i = 0; i < PRICE_LEVELS; i++) begin
      bid_mem[i] = '0;
      ask_mem[i] = '0;
    end
  end

  // ---- per-side resting order counts (for side-empty detection) -----------
  logic [31:0] cnt_bid, cnt_ask;

  // ---- order-ref table interface ------------------------------------------
  logic                tbl_cmd_valid;
  logic [1:0]          tbl_cmd_op;
  logic [REF_W-1:0]    tbl_cmd_ref;
  logic [PRICE_W-1:0]  tbl_cmd_price;
  logic [SHARES_W-1:0] tbl_cmd_shares;
  logic                tbl_cmd_is_bid;
  logic [LOCATE_W-1:0] tbl_cmd_locate;
  logic                tbl_busy, tbl_done, tbl_found, tbl_is_bid;
  logic [PRICE_W-1:0]  tbl_price;
  logic [SHARES_W-1:0] tbl_shares;
  logic [LOCATE_W-1:0] tbl_locate;

  order_ref_table u_tbl (
    .clk(clk), .rst(rst),
    .cmd_valid(tbl_cmd_valid), .cmd_op(tbl_cmd_op), .cmd_ref(tbl_cmd_ref),
    .cmd_price(tbl_cmd_price), .cmd_shares(tbl_cmd_shares),
    .cmd_is_bid(tbl_cmd_is_bid), .cmd_locate(tbl_cmd_locate),
    .busy(tbl_busy), .done(tbl_done), .res_found(tbl_found),
    .res_price(tbl_price), .res_shares(tbl_shares),
    .res_is_bid(tbl_is_bid), .res_locate(tbl_locate),
    .lk_probe1(lk_probe1), .lk_probe2(lk_probe2), .lk_probe_gt2(lk_probe_gt2)
  );

  // ---- best trackers ------------------------------------------------------
  logic                bt_bid_add_en, bt_ask_add_en;
  logic [PRICE_W-1:0]  bt_bid_add_price, bt_ask_add_price;
  logic                bt_bid_empt_en, bt_ask_empt_en;
  logic [PRICE_W-1:0]  bt_bid_empt_price, bt_ask_empt_price;
  logic                bt_bid_scanning, bt_ask_scanning;
  logic [LEVEL_AW-1:0] bt_bid_scan_addr, bt_ask_scan_addr;

  best_tracker #(.IS_BID(1'b1)) u_bid (
    .clk(clk), .rst(rst),
    .add_en(bt_bid_add_en), .add_price(bt_bid_add_price),
    .empt_en(bt_bid_empt_en), .empt_price(bt_bid_empt_price),
    .side_nonempty(cnt_bid != 0),
    .scan_rd_addr(bt_bid_scan_addr),
    .scan_rd_shares(bid_mem[bt_bid_scan_addr].total_shares),
    .scanning(bt_bid_scanning),
    .best_valid(best_bid_valid), .best_price(best_bid_price)
  );
  best_tracker #(.IS_BID(1'b0)) u_ask (
    .clk(clk), .rst(rst),
    .add_en(bt_ask_add_en), .add_price(bt_ask_add_price),
    .empt_en(bt_ask_empt_en), .empt_price(bt_ask_empt_price),
    .side_nonempty(cnt_ask != 0),
    .scan_rd_addr(bt_ask_scan_addr),
    .scan_rd_shares(ask_mem[bt_ask_scan_addr].total_shares),
    .scanning(bt_ask_scanning),
    .best_valid(best_ask_valid), .best_price(best_ask_price)
  );

  assign best_bid_shares = best_bid_valid ? bid_mem[best_bid_price].total_shares : 32'd0;
  assign best_ask_shares = best_ask_valid ? ask_mem[best_ask_price].total_shares : 32'd0;
  assign dbg_shares = dbg_is_bid ? bid_mem[dbg_price].total_shares
                                 : ask_mem[dbg_price].total_shares;
  assign dbg_count  = dbg_is_bid ? bid_mem[dbg_price].order_count
                                 : ask_mem[dbg_price].order_count;
  assign any_scanning = bt_bid_scanning | bt_ask_scanning;

  // ---- main FSM -----------------------------------------------------------
  typedef enum logic [3:0] {
    S_IDLE, S_ADD_TBL, S_ADD_LVL,
    S_LK, S_MOD_LVL, S_MOD_TBL, S_EMPTY, S_EMPTY_WAIT,
    S_RADD_TBL, S_RADD_LVL, S_FINISH
  } state_t;
  state_t state;

  // latched message context
  logic [7:0]          mtype_q;
  logic                is_replace_q;
  logic [REF_W-1:0]    ref_q, new_ref_q;
  logic                op_side_q;       // side under operation (add side / resting side)
  logic [PRICE_W-1:0]  op_price_q;      // add price
  logic [SHARES_W-1:0] op_shares_q;     // add shares
  logic [LOCATE_W-1:0] op_locate_q;
  logic [PRICE_W-1:0]  dec_price_q;     // C execution price / replace new price
  logic [SHARES_W-1:0] dec_shares_q;    // exec/cancel shares / replace new shares
  logic                printable_q;
  // resting-order context from lookup
  logic                r_side_q;
  logic [PRICE_W-1:0]  r_price_q;
  logic [SHARES_W-1:0] r_rem_q;
  logic [LOCATE_W-1:0] r_locate_q;
  logic                emptied_q;
  logic [SHARES_W-1:0] new_rem_q;
  logic                tbl_req_q;       // one outstanding table request guard
  logic                expect_scan_q;   // a best-level scan was triggered this msg
  logic                scan_seen_q;     // tracker scan has been observed active

  assign busy       = (state != S_IDLE);
  assign ev_type    = mtype_q;
  assign ev_done    = engine_done;
  assign in_replace = is_replace_q && busy;
  // accept the decoded message the cycle we latch it, so the decoder drops
  // dec_valid immediately and the engine never re-triggers on the same message.
  assign dec_accept = (state == S_IDLE) && dec_valid;

  function automatic logic [SHARES_W-1:0] min_u(input logic [SHARES_W-1:0] a,
                                                input logic [SHARES_W-1:0] b);
    min_u = (a < b) ? a : b;
  endfunction

  always_ff @(posedge clk) begin
    if (rst) begin
      state         <= S_IDLE;
      engine_done   <= 1'b0;
      cnt_bid       <= '0;
      cnt_ask       <= '0;
      tot_bid_vol   <= '0;
      tot_ask_vol   <= '0;
      trade_valid   <= 1'b0;
      tbl_cmd_valid <= 1'b0;
      bt_bid_add_en <= 1'b0; bt_ask_add_en <= 1'b0;
      bt_bid_empt_en<= 1'b0; bt_ask_empt_en<= 1'b0;
      is_replace_q  <= 1'b0;
      tbl_req_q     <= 1'b0;
      expect_scan_q <= 1'b0;
      scan_seen_q   <= 1'b0;
    end else begin
      // default deassertions (one-cycle pulses)
      engine_done   <= 1'b0;
      trade_valid   <= 1'b0;
      tbl_cmd_valid <= 1'b0;
      bt_bid_add_en <= 1'b0; bt_ask_add_en <= 1'b0;
      bt_bid_empt_en<= 1'b0; bt_ask_empt_en<= 1'b0;

      case (state)
        // ------------------------------------------------------------------
        S_IDLE: if (dec_valid) begin
          mtype_q      <= dec.mtype;
          ref_q        <= dec.order_ref;
          new_ref_q    <= dec.new_order_ref;
          dec_price_q  <= dec.price;
          dec_shares_q <= dec.shares;
          printable_q  <= dec.printable;
          op_locate_q  <= dec.stock_locate;
          is_replace_q <= 1'b0;
          unique case (dec.mtype)
            T_ADD, T_ADD_MPID: begin
              op_side_q   <= dec.is_bid;
              op_price_q  <= dec.price;
              op_shares_q <= dec.shares;
              state       <= S_ADD_TBL;
            end
            T_EXEC, T_EXEC_PR, T_CANCEL, T_DELETE: state <= S_LK;
            T_REPLACE: begin is_replace_q <= 1'b1; state <= S_LK; end
            T_TRADE: begin
              trade_valid  <= 1'b1;     // VWAP/trade-count, no book effect
              trade_price  <= dec.price;
              trade_shares <= dec.shares;
              state        <= S_FINISH;
            end
            default: state <= S_FINISH; // System / unknown
          endcase
        end

        // ---- Add path ----------------------------------------------------
        S_ADD_TBL: begin
          if (!tbl_req_q) begin
            tbl_cmd_valid  <= 1'b1;
            tbl_cmd_op     <= OP_INSERT;
            tbl_cmd_ref    <= ref_q;
            tbl_cmd_price  <= op_price_q;
            tbl_cmd_shares <= op_shares_q;
            tbl_cmd_is_bid <= op_side_q;
            tbl_cmd_locate <= op_locate_q;
            tbl_req_q      <= 1'b1;
          end else if (tbl_done) begin
            tbl_req_q <= 1'b0;
            state     <= S_ADD_LVL;
          end
        end
        S_ADD_LVL: begin
          if (op_side_q) begin
            bid_mem[op_price_q].total_shares <= bid_mem[op_price_q].total_shares + op_shares_q;
            bid_mem[op_price_q].order_count  <= bid_mem[op_price_q].order_count + 1'b1;
            cnt_bid       <= cnt_bid + 1'b1;
            tot_bid_vol   <= tot_bid_vol + 64'(op_shares_q);
            bt_bid_add_en <= 1'b1; bt_bid_add_price <= op_price_q;
          end else begin
            ask_mem[op_price_q].total_shares <= ask_mem[op_price_q].total_shares + op_shares_q;
            ask_mem[op_price_q].order_count  <= ask_mem[op_price_q].order_count + 1'b1;
            cnt_ask       <= cnt_ask + 1'b1;
            tot_ask_vol   <= tot_ask_vol + 64'(op_shares_q);
            bt_ask_add_en <= 1'b1; bt_ask_add_price <= op_price_q;
          end
          state <= S_FINISH;
        end

        // ---- Modify / Replace-remove path --------------------------------
        S_LK: begin
          if (!tbl_req_q) begin
            tbl_cmd_valid <= 1'b1;
            tbl_cmd_op    <= OP_LOOKUP;
            tbl_cmd_ref   <= ref_q;
            tbl_req_q     <= 1'b1;
          end else if (tbl_done) begin
            tbl_req_q <= 1'b0;
            if (!tbl_found) begin
              state <= S_FINISH;            // unknown ref: skip (stream safety)
            end else begin
              r_side_q   <= tbl_is_bid;
              r_price_q  <= tbl_price;
              r_rem_q    <= tbl_shares;
              r_locate_q <= tbl_locate;
              state      <= S_MOD_LVL;
            end
          end
        end
        S_MOD_LVL: begin
          // amount removed from the resting order
          logic [SHARES_W-1:0] amt;
          logic [SHARES_W-1:0] nrem;
          logic [31:0]         newtot;
          if (mtype_q == T_DELETE || is_replace_q) amt = r_rem_q;
          else                                     amt = min_u(dec_shares_q, r_rem_q);
          nrem = r_rem_q - amt;
          if (r_side_q) begin
            newtot = bid_mem[r_price_q].total_shares - amt;
            bid_mem[r_price_q].total_shares <= newtot;
            if (nrem == 0) bid_mem[r_price_q].order_count <= bid_mem[r_price_q].order_count - 1'b1;
            tot_bid_vol <= tot_bid_vol - 64'(amt);
            if (nrem == 0) cnt_bid <= cnt_bid - 1'b1;
          end else begin
            newtot = ask_mem[r_price_q].total_shares - amt;
            ask_mem[r_price_q].total_shares <= newtot;
            if (nrem == 0) ask_mem[r_price_q].order_count <= ask_mem[r_price_q].order_count - 1'b1;
            tot_ask_vol <= tot_ask_vol - 64'(amt);
            if (nrem == 0) cnt_ask <= cnt_ask - 1'b1;
          end
          new_rem_q <= nrem;
          emptied_q <= (newtot == 0);
          // unified trade events for Execute / Execute-with-price
          if (mtype_q == T_EXEC) begin
            trade_valid <= 1'b1; trade_price <= r_price_q; trade_shares <= amt;
          end else if (mtype_q == T_EXEC_PR && printable_q) begin
            trade_valid <= 1'b1; trade_price <= dec_price_q; trade_shares <= amt;
          end
          state <= S_MOD_TBL;
        end
        S_MOD_TBL: begin
          if (!tbl_req_q) begin
            tbl_cmd_valid <= 1'b1;
            tbl_cmd_ref   <= ref_q;
            if (new_rem_q == 0) begin
              tbl_cmd_op <= OP_DELETE;
            end else begin
              tbl_cmd_op     <= OP_INSERT;        // update remaining shares
              tbl_cmd_price  <= r_price_q;
              tbl_cmd_shares <= new_rem_q;
              tbl_cmd_is_bid <= r_side_q;
              tbl_cmd_locate <= r_locate_q;
            end
            tbl_req_q <= 1'b1;
          end else if (tbl_done) begin
            tbl_req_q <= 1'b0;
            state     <= S_EMPTY;
          end
        end
        S_EMPTY: begin
          // Pulse the emptied price to the relevant tracker. A scan is only
          // *expected* (and therefore only waited on) when the emptied level was
          // the current best AND that side still has resting orders — otherwise
          // the tracker either clears best (O(1)) or does nothing, and we must
          // not wait. best_*_price below is still the pre-update best because the
          // tracker only advances during the scan.
          expect_scan_q <= 1'b0;
          scan_seen_q   <= 1'b0;
          if (emptied_q) begin
            if (r_side_q) begin
              bt_bid_empt_en <= 1'b1; bt_bid_empt_price <= r_price_q;
              if (best_bid_valid && r_price_q == best_bid_price && cnt_bid != 0)
                expect_scan_q <= 1'b1;
            end else begin
              bt_ask_empt_en <= 1'b1; bt_ask_empt_price <= r_price_q;
              if (best_ask_valid && r_price_q == best_ask_price && cnt_ask != 0)
                expect_scan_q <= 1'b1;
            end
          end
          state <= S_EMPTY_WAIT;
        end
        S_EMPTY_WAIT: begin
          // The tracker raises `scanning` one cycle after it samples empt_en, so
          // we cannot simply test !scanning here (it would pass before the scan
          // even starts). Instead: if no scan is expected, proceed; otherwise
          // wait until we have seen scanning go active and then return to idle.
          if (any_scanning) scan_seen_q <= 1'b1;
          if (!expect_scan_q) begin
            state <= is_replace_q ? S_RADD_TBL : S_FINISH;
          end else if (scan_seen_q && !any_scanning) begin
            state <= is_replace_q ? S_RADD_TBL : S_FINISH;
          end
        end

        // ---- Replace-add path --------------------------------------------
        S_RADD_TBL: begin
          if (!tbl_req_q) begin
            tbl_cmd_valid  <= 1'b1;
            tbl_cmd_op     <= OP_INSERT;
            tbl_cmd_ref    <= new_ref_q;
            tbl_cmd_price  <= dec_price_q;
            tbl_cmd_shares <= dec_shares_q;
            tbl_cmd_is_bid <= r_side_q;
            tbl_cmd_locate <= r_locate_q;
            tbl_req_q      <= 1'b1;
          end else if (tbl_done) begin
            tbl_req_q <= 1'b0;
            state     <= S_RADD_LVL;
          end
        end
        S_RADD_LVL: begin
          if (r_side_q) begin
            bid_mem[dec_price_q].total_shares <= bid_mem[dec_price_q].total_shares + dec_shares_q;
            bid_mem[dec_price_q].order_count  <= bid_mem[dec_price_q].order_count + 1'b1;
            cnt_bid       <= cnt_bid + 1'b1;
            tot_bid_vol   <= tot_bid_vol + 64'(dec_shares_q);
            bt_bid_add_en <= 1'b1; bt_bid_add_price <= dec_price_q;
          end else begin
            ask_mem[dec_price_q].total_shares <= ask_mem[dec_price_q].total_shares + dec_shares_q;
            ask_mem[dec_price_q].order_count  <= ask_mem[dec_price_q].order_count + 1'b1;
            cnt_ask       <= cnt_ask + 1'b1;
            tot_ask_vol   <= tot_ask_vol + 64'(dec_shares_q);
            bt_ask_add_en <= 1'b1; bt_ask_add_price <= dec_price_q;
          end
          state <= S_FINISH;
        end

        // ------------------------------------------------------------------
        S_FINISH: begin
          engine_done <= 1'b1;
          state       <= S_IDLE;
        end
        default: state <= S_IDLE;
      endcase
    end
  end

endmodule
