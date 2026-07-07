// Performance counters: per-type message counts (A,F,E,C,X,D,U,P,S),
// engine-busy cycles by type, best-price scan count + total scan cycles,
// hash probe-depth distribution (1 / 2 / >2), stall cycles, and
// cycle/message totals for throughput.
module perf_counters
  import ob_pkg::*;
(
  input  logic        clk,
  input  logic        rst,

  input  logic        ev_done,     // a message was committed this cycle
  input  logic [7:0]  ev_type,     // its message type
  input  logic        busy,        // engine processing a message
  input  logic        any_scanning,
  input  logic        in_replace,
  input  logic        lk_probe1,
  input  logic        lk_probe2,
  input  logic        lk_probe_gt2,

  output logic [31:0] msg_count [9],
  output logic [63:0] add_cycles,
  output logic [63:0] delete_cycles,
  output logic [63:0] replace_cycles,
  output logic [63:0] scan_count,
  output logic [63:0] scan_cycles_total,
  output logic [63:0] hash_probe_1,
  output logic [63:0] hash_probe_2,
  output logic [63:0] hash_probe_gt2,
  output logic [63:0] pipeline_stall_cycles,
  output logic [63:0] cycle_count,
  output logic [63:0] msg_total
);

  logic scan_d;   // previous-cycle scanning, for rising-edge detection

  function automatic int unsigned type_idx(input logic [7:0] t);
    case (t)
      T_ADD:      type_idx = 0;
      T_ADD_MPID: type_idx = 1;
      T_EXEC:     type_idx = 2;
      T_EXEC_PR:  type_idx = 3;
      T_CANCEL:   type_idx = 4;
      T_DELETE:   type_idx = 5;
      T_REPLACE:  type_idx = 6;
      T_TRADE:    type_idx = 7;
      T_SYSTEM:   type_idx = 8;
      default:    type_idx = 8;
    endcase
  endfunction

  always_ff @(posedge clk) begin
    if (rst) begin
      for (int i = 0; i < 9; i++) msg_count[i] <= '0;
      add_cycles            <= '0;
      delete_cycles         <= '0;
      replace_cycles        <= '0;
      scan_count            <= '0;
      scan_cycles_total     <= '0;
      hash_probe_1          <= '0;
      hash_probe_2          <= '0;
      hash_probe_gt2        <= '0;
      pipeline_stall_cycles <= '0;
      cycle_count           <= '0;
      msg_total             <= '0;
      scan_d                <= 1'b0;
    end else begin
      cycle_count <= cycle_count + 1'b1;
      scan_d      <= any_scanning;

      if (ev_done) begin
        msg_count[type_idx(ev_type)] <= msg_count[type_idx(ev_type)] + 1'b1;
        msg_total                    <= msg_total + 1'b1;
      end

      // per-type cycle attribution while the engine is busy
      if (busy) begin
        case (ev_type)
          T_ADD, T_ADD_MPID: add_cycles     <= add_cycles + 1'b1;
          T_DELETE:          delete_cycles   <= delete_cycles + 1'b1;
          T_REPLACE:         replace_cycles  <= replace_cycles + 1'b1;
          default: ;
        endcase
      end

      if (any_scanning)            scan_cycles_total <= scan_cycles_total + 1'b1;
      if (any_scanning && !scan_d) scan_count        <= scan_count + 1'b1;
      if (busy && (any_scanning || in_replace))
        pipeline_stall_cycles <= pipeline_stall_cycles + 1'b1;

      if (lk_probe1)    hash_probe_1   <= hash_probe_1   + 1'b1;
      if (lk_probe2)    hash_probe_2   <= hash_probe_2   + 1'b1;
      if (lk_probe_gt2) hash_probe_gt2 <= hash_probe_gt2 + 1'b1;
    end
  end

endmodule
