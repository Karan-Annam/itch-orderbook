// Strips MoldUDP64 headers from the udp_stripper's payload stream and tracks
// the session sequence. Mold message blocks ([2-byte BE length][body]) are
// exactly the itch_framer's native input, so past the 20-byte header
// (session[10] | seq[8] BE | count[2] BE) the datagram payload passes through
// untouched. Blocks never straddle datagrams, so a lost datagram loses whole
// messages — framing sync survives any gap by construction.
//
// Sequence policy (telemetry only — no retransmit request path, which would
// need a TX socket/session manager):
//   seq == expected : normal; expected += count (heartbeat count=0 is a no-op)
//   seq >  expected : gap; count it, accept from here (expected = seq+count)
//   seq <  expected : stale/duplicate datagram; drop it WHOLE (never
//                     double-apply), expected unchanged
//   count == 0xFFFF : end-of-session; sticky flag, payload dropped
// The first datagram seeds `expected` without counting a gap.
//
// Structure (timing-driven; a single-cycle passthrough version lost 8.7 ns
// of slack at 100 MHz by stacking the header merge + 64-bit seq compare +
// byte shifter on top of the stripper's and framer's own comb logic):
//   stage A   registers every incoming beat;
//   S_STREAM  consumes header bytes / forwards payload from stage A into the
//             output register, one beat per cycle;
//   S_DECIDE  one stall cycle per datagram: the seq policy is evaluated on
//             REGISTERED header fields; the completing beat's payload (if
//             any) is held in stage A across the stall and forwarded after.
// Every cone is register-to-register. Payload gains 2 cycles of latency and
// each datagram one stall cycle — invisible at any realistic feed rate.
// mold_mode=0 bypasses combinationally (bit-identical to the pre-mold
// pipeline); the mode is static per session.
module mold_stripper
  import ob_pkg::*;
(
  input  logic                clk,
  input  logic                rst,
  input  logic                mold_mode,

  // upstream: udp_stripper payload beats + datagram boundary
  input  logic [WORD_W-1:0]   in_data,
  input  logic [NBYTES_W-1:0] in_nbytes,
  input  logic                in_valid,
  input  logic                in_sop,     // first payload beat of a datagram
  output logic                in_ready,

  // downstream: itch_framer (interface unchanged)
  output logic [WORD_W-1:0]   out_data,
  output logic [NBYTES_W-1:0] out_nbytes,
  output logic                out_valid,
  input  logic                out_ready,

  // telemetry
  output logic [63:0]         mold_next_seq,
  output logic [31:0]         mold_gap_events,
  output logic [63:0]         mold_gap_msgs,
  output logic                mold_session_end
);

  localparam int HDR_BYTES = 20;

  typedef enum logic [0:0] {S_STREAM, S_DECIDE} state_t;
  state_t state;

  // ---- stage A: registered input beat --------------------------------------
  logic [WORD_W-1:0]   a_data_q;
  logic [NBYTES_W-1:0] a_nbytes_q;
  logic                a_sop_q, a_valid_q;

  // ---- header consumption (operates on stage A) ----------------------------
  logic [4:0]  hdr_left_q;      // header bytes still to consume (0..20)
  logic [4:0]  hdr_left_eff;    // a sop beat restarts the header
  logic [4:0]  k;               // header bytes this beat contributes
  logic        hdr_active, completes;
  logic [4:0]  a_off_q;         // header bytes to skip in the HELD beat
  logic        drop_q;          // datagram payload is being discarded

  assign hdr_left_eff = (a_valid_q && a_sop_q) ? 5'(HDR_BYTES) : hdr_left_q;
  assign hdr_active   = (hdr_left_eff != 0);
  assign k            = (hdr_left_eff < 5'(a_nbytes_q)) ? hdr_left_eff : 5'(a_nbytes_q);
  assign completes    = hdr_active && (k == hdr_left_eff);

  // header bytes land at position (20 - hdr_left_eff) onward; the merged view
  // exists only to capture seq/cnt at the completion beat
  logic [7:0] hdr_q [HDR_BYTES];
  logic [7:0] hdr_n [HDR_BYTES];
  always_comb begin
    for (int b = 0; b < HDR_BYTES; b++) hdr_n[b] = hdr_q[b];
    for (int i = 0; i < WORD_BYTES; i++) begin
      automatic int idx = (HDR_BYTES - int'(hdr_left_eff)) + i;
      if (i < int'(k) && idx >= 0 && idx < HDR_BYTES)
        hdr_n[idx] = a_data_q[8*i +: 8];
    end
  end

  // registered at header completion; S_DECIDE evaluates these
  logic [63:0] seq_q;
  logic [15:0] cnt_q;

  // ---- sequence tracking ----------------------------------------------------
  logic        seq_init_q;
  logic [63:0] expected_q;
  logic [31:0] gap_events_q;
  logic [63:0] gap_msgs_q;
  logic        session_end_q;

  assign mold_next_seq    = expected_q;
  assign mold_gap_events  = gap_events_q;
  assign mold_gap_msgs    = gap_msgs_q;
  assign mold_session_end = session_end_q;

  // ---- output register stage ------------------------------------------------
  logic [WORD_W-1:0]   out_data_q;
  logic [NBYTES_W-1:0] out_nbytes_q;
  logic                out_valid_q;
  logic                out_free;
  assign out_free = !out_valid_q || out_ready;

  // ---- stage-A consumption rules (S_STREAM only) ----------------------------
  logic hdr_pay;                       // completing beat carries payload too
  logic [NBYTES_W:0] pay_n;            // payload bytes of a pure-payload beat
  assign hdr_pay = completes && (5'(a_nbytes_q) > k);
  assign pay_n   = (NBYTES_W+1)'(a_nbytes_q) - (NBYTES_W+1)'(a_off_q);

  logic a_consume;
  always_comb begin
    a_consume = 1'b0;
    if (state == S_STREAM && a_valid_q) begin
      if (hdr_active) a_consume = !hdr_pay;      // held across DECIDE if payload
      else if (drop_q) a_consume = 1'b1;         // discard dropped payload
      else a_consume = out_free;                 // forward payload
    end
  end

  assign in_ready = mold_mode ? (!a_valid_q || a_consume) : out_ready;

  always_comb begin
    if (!mold_mode) begin
      out_data   = in_data;
      out_nbytes = in_nbytes;
      out_valid  = in_valid;
    end else begin
      out_data   = out_data_q;
      out_nbytes = out_nbytes_q;
      out_valid  = out_valid_q;
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      state         <= S_STREAM;
      a_valid_q     <= 1'b0;
      hdr_left_q    <= '0;
      a_off_q       <= '0;
      drop_q        <= 1'b0;
      out_valid_q   <= 1'b0;
      seq_init_q    <= 1'b0;
      expected_q    <= '0;
      gap_events_q  <= '0;
      gap_msgs_q    <= '0;
      session_end_q <= 1'b0;
    end else if (mold_mode) begin
      if (out_valid_q && out_ready) out_valid_q <= 1'b0;   // drained

      unique case (state)
        S_STREAM: if (a_valid_q) begin
          if (hdr_active) begin
            for (int b = 0; b < HDR_BYTES; b++) hdr_q[b] <= hdr_n[b];
            hdr_left_q <= hdr_left_eff - k;
            if (completes) begin
              // the completing beat is never a sop beat (a beat is <=16 bytes,
              // the header is 20), so the held beat cannot re-trigger the
              // hdr_left_eff sop restart after DECIDE
              seq_q   <= {hdr_n[10], hdr_n[11], hdr_n[12], hdr_n[13],
                          hdr_n[14], hdr_n[15], hdr_n[16], hdr_n[17]};
              cnt_q   <= {hdr_n[18], hdr_n[19]};
              a_off_q <= hdr_pay ? k : 5'd0;     // held beat skips its header
              state   <= S_DECIDE;
            end
          end else if (drop_q) begin
            a_off_q <= '0;                       // discarded
          end else if (out_free) begin
            out_data_q   <= a_data_q >> (8 * a_off_q);
            out_nbytes_q <= NBYTES_W'(pay_n);
            out_valid_q  <= 1'b1;
            a_off_q      <= '0;
          end
        end

        S_DECIDE: begin
          // one stall cycle: policy on registered seq/cnt, reg-to-reg only
          if (cnt_q == 16'hFFFF) begin
            session_end_q <= 1'b1;
            drop_q        <= 1'b1;
          end else if (!seq_init_q) begin
            seq_init_q <= 1'b1;
            expected_q <= seq_q + 64'(cnt_q);
            drop_q     <= 1'b0;
          end else if (seq_q == expected_q) begin
            expected_q <= seq_q + 64'(cnt_q);
            drop_q     <= 1'b0;
          end else if (seq_q > expected_q) begin
            gap_events_q <= gap_events_q + 1'b1;
            gap_msgs_q   <= gap_msgs_q + (seq_q - expected_q);
            expected_q   <= seq_q + 64'(cnt_q);
            drop_q       <= 1'b0;
          end else begin
            drop_q <= 1'b1;                      // stale: expected unchanged
          end
          state <= S_STREAM;
        end
      endcase

      // stage A load / clear (load wins when both happen in one cycle)
      if (in_valid && in_ready) begin
        a_data_q   <= in_data;
        a_nbytes_q <= in_nbytes;
        a_sop_q    <= in_sop;
        a_valid_q  <= 1'b1;
      end else if (a_consume) begin
        a_valid_q <= 1'b0;
      end
    end
  end

endmodule
