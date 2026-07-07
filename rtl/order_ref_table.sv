// order_ref -> order-state hash table in SRAM. Open addressing with linear
// probing; hash is an XOR-fold of the 64-bit ref down to 16 bits (pure wires).
// One probe per cycle: read slot, compare, step on collision.
//
// DELETE leaves a TOMBSTONE, not an empty slot. Clearing the slot would cut
// every probe chain that runs through it and orphan colliding keys stored
// beyond it (found the hard way — see docs/DESIGN.md). Lookups probe past
// tombstones and stop only at truly-empty; inserts reuse the first tombstone.
// Probe depth (1 / 2 / >2) is reported to the perf counters.
//
// Async-read behavioral SRAM; a real FPGA build would use registered BRAM.
module order_ref_table
  import ob_pkg::*;
(
  input  logic                 clk,
  input  logic                 rst,

  input  logic                 cmd_valid,
  input  logic [1:0]           cmd_op,      // OP_INSERT/LOOKUP/DELETE
  input  logic [REF_W-1:0]     cmd_ref,
  input  logic [PRICE_W-1:0]   cmd_price,
  input  logic [SHARES_W-1:0]  cmd_shares,
  input  logic                 cmd_is_bid,
  input  logic [LOCATE_W-1:0]  cmd_locate,

  output logic                 busy,
  output logic                 done,        // 1-cycle pulse
  output logic                 res_found,
  output logic [PRICE_W-1:0]   res_price,
  output logic [SHARES_W-1:0]  res_shares,
  output logic                 res_is_bid,
  output logic [LOCATE_W-1:0]  res_locate,

  // probe-depth telemetry (pulses, qualified by a completed LOOKUP)
  output logic                 lk_probe1,
  output logic                 lk_probe2,
  output logic                 lk_probe_gt2
);

  order_entry_t mem [TABLE_SIZE];

  typedef enum logic [1:0] {S_IDLE, S_PROBE, S_DONE} state_t;
  state_t                 state;
  logic [1:0]             op_q;
  logic [REF_W-1:0]       ref_q;
  logic [PRICE_W-1:0]     price_q;
  logic [SHARES_W-1:0]    shares_q;
  logic                   is_bid_q;
  logic [LOCATE_W-1:0]    locate_q;
  logic [HASH_W-1:0]      idx_q;
  logic [31:0]            probes_q;
  // first tombstone seen on an INSERT probe — reused so deleted slots don't make
  // chains grow without bound.
  logic                   have_tomb_q;
  logic [HASH_W-1:0]      tomb_idx_q;

  order_entry_t cur;
  assign cur = mem[idx_q];   // async read

  assign busy = (state != S_IDLE);

  // initialise memory to empty (valid=0) for simulation determinism
  initial begin
    for (int i = 0; i < TABLE_SIZE; i++) mem[i] = '0;
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      state        <= S_IDLE;
      done         <= 1'b0;
      res_found    <= 1'b0;
      lk_probe1    <= 1'b0;
      lk_probe2    <= 1'b0;
      lk_probe_gt2 <= 1'b0;
    end else begin
      done         <= 1'b0;
      lk_probe1    <= 1'b0;
      lk_probe2    <= 1'b0;
      lk_probe_gt2 <= 1'b0;

      case (state)
        S_IDLE: if (cmd_valid) begin
          op_q        <= cmd_op;
          ref_q       <= cmd_ref;
          price_q     <= cmd_price;
          shares_q    <= cmd_shares;
          is_bid_q    <= cmd_is_bid;
          locate_q    <= cmd_locate;
          idx_q       <= hash_ref(cmd_ref);
          probes_q    <= 1;
          have_tomb_q <= 1'b0;
          state       <= S_PROBE;
        end

        S_PROBE: begin
          if (op_q == OP_INSERT) begin
            if (cur.valid && cur.oref == ref_q) begin
              // key already present: update in place
              mem[idx_q] <= '{valid:1'b1, tomb:1'b0, is_bid:is_bid_q, locate:locate_q,
                              shares:shares_q, price:price_q, oref:ref_q};
              res_found  <= 1'b1;
              state      <= S_DONE;
            end else if (!cur.valid && !cur.tomb) begin
              // true empty: insert here, or at the first tombstone we passed
              if (have_tomb_q)
                mem[tomb_idx_q] <= '{valid:1'b1, tomb:1'b0, is_bid:is_bid_q, locate:locate_q,
                                     shares:shares_q, price:price_q, oref:ref_q};
              else
                mem[idx_q]      <= '{valid:1'b1, tomb:1'b0, is_bid:is_bid_q, locate:locate_q,
                                     shares:shares_q, price:price_q, oref:ref_q};
              res_found <= 1'b1;
              state     <= S_DONE;
            end else begin
              // occupied-by-other OR tombstone: remember first tomb, keep probing
              if (cur.tomb && !have_tomb_q) begin
                have_tomb_q <= 1'b1;
                tomb_idx_q  <= idx_q;
              end
              idx_q    <= idx_q + 1'b1;
              probes_q <= probes_q + 1'b1;
              if (probes_q >= MAX_PROBE) state <= S_DONE; // table full (shouldn't happen)
            end
          end else begin // LOOKUP or DELETE
            if (cur.valid && cur.oref == ref_q) begin
              res_found  <= 1'b1;
              res_price  <= cur.price;
              res_shares <= cur.shares;
              res_is_bid <= cur.is_bid;
              res_locate <= cur.locate;
              // DELETE leaves a tombstone so colliding keys further along the
              // chain remain reachable (do NOT clear to all-zero / empty).
              if (op_q == OP_DELETE)
                mem[idx_q] <= '{valid:1'b0, tomb:1'b1, is_bid:1'b0, locate:'0,
                                shares:'0, price:'0, oref:'0};
              if (op_q == OP_LOOKUP) begin
                lk_probe1    <= (probes_q == 1);
                lk_probe2    <= (probes_q == 2);
                lk_probe_gt2 <= (probes_q  > 2);
              end
              state <= S_DONE;
            end else if (!cur.valid && !cur.tomb) begin
              // true empty: key not present (tombstones are skipped, not stops)
              res_found <= 1'b0;
              if (op_q == OP_LOOKUP) begin
                lk_probe1    <= (probes_q == 1);
                lk_probe2    <= (probes_q == 2);
                lk_probe_gt2 <= (probes_q  > 2);
              end
              state <= S_DONE;
            end else begin
              // occupied-by-other key OR tombstone: keep probing
              idx_q    <= idx_q + 1'b1;
              probes_q <= probes_q + 1'b1;
              if (probes_q >= MAX_PROBE) begin
                res_found <= 1'b0;
                state     <= S_DONE;
              end
            end
          end
        end

        S_DONE: begin
          done  <= 1'b1;
          state <= S_IDLE;
        end

        default: state <= S_IDLE;
      endcase
    end
  end

endmodule
