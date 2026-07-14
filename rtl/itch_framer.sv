// Message framing, 16 bytes per beat. Wire format is [2-byte BE length][body];
// messages are packed back-to-back with no alignment, so a beat can carry the
// tail of one message and the head of the next (never more: the minimum frame,
// 2-byte prefix + 14-byte System body, equals WORD_BYTES).
//
// All alignment/straddle cases collapse into one mechanism: a 32-byte
// compacting window. Each cycle the FSM consumes bytes from the front (2 for
// the length prefix, up to 16 for a body beat) and an accepted input beat is
// appended at the back. A length prefix split across two input words is not a
// special case — it is just "window occupancy < 2, wait for more bytes".
//
// Body beats are emitted downstream aligned to the message body: beat widx
// carries body bytes [16*widx .. 16*widx+nbytes-1], lane 0 first. A non-final
// beat always carries 16 bytes, so the decoder writes whole aligned words.
//
// Hazard ordering: the final body beat lands in the decoder's body[] at edge
// t; msg_complete fires no earlier than cycle t+1 (S_SETTLE), so the decode of
// body[] is settled when dec is latched. After msg_complete the framer needs
// >= 2 cycles (S_SETTLE -> S_HDR -> S_BODY) before the next message's bytes
// touch body[], so the latched dec is never contaminated.
//
// Zero-length frames are discarded. Bodies larger than MSG_MAX_BYTES are
// consumed without being presented to the decoder, preserving alignment for
// the next frame while preventing decoder storage from being overrun.
module itch_framer
  import ob_pkg::*;
(
  input  logic                clk,
  input  logic                rst,

  // upstream (from udp_stripper): packed-contiguous beats, 1..16 bytes
  input  logic [WORD_W-1:0]   in_data,
  input  logic [NBYTES_W-1:0] in_nbytes,
  input  logic                in_valid,
  output logic                in_ready,

  // downstream body-aligned beats (to itch_decoder)
  output logic [WORD_W-1:0]   msg_data,
  output logic [NBYTES_W-1:0] msg_nbytes,
  output logic                msg_beat_valid,
  output logic [WIDX_W-1:0]   msg_widx,
  output logic                msg_start,      // first beat of a message
  output logic                msg_complete,   // pulse: body fully delivered & settled
  output logic [15:0]         msg_len,

  // decoupling handshake: high while the decoder holds a message the engine
  // has not yet accepted. Ingest of message N+1 overlaps engine processing of
  // N; the framer only stalls (in S_SETTLE) when N+1 has been fully assembled
  // before the engine accepted N. Gating msg_complete on !dec_pending means
  // dec is never re-latched while pending, and dec_valid always drops for at
  // least one cycle between consecutive messages (the C++ tap capture relies
  // on that edge).
  input  logic                dec_pending
);

  localparam int WIN_BYTES = 2 * WORD_BYTES;   // 32-byte compacting window

  typedef enum logic [1:0] {S_HDR, S_BODY, S_SETTLE, S_DROP} state_t;
  state_t state;

  logic [7:0]        win_q [WIN_BYTES];  // byte 0 = oldest unconsumed byte
  logic [5:0]        cnt_q;              // window occupancy, 0..32
  logic [15:0]       len_q;              // body length of current message
  logic [15:0]       remain_q;           // body bytes not yet emitted
  logic [WIDX_W-1:0] widx_q;             // body word index of the next beat

  // ---- per-state front consumption (registered inputs only) ---------------
  logic [5:0] consume_c;
  always_comb begin
    consume_c = '0;
    unique case (state)
      S_HDR:  if (cnt_q >= 6'd2) consume_c = 6'd2;
      S_BODY: begin
        if (remain_q <= 16'(WORD_BYTES)) begin
          if (16'(cnt_q) >= remain_q) consume_c = remain_q[5:0];  // final beat
        end else begin
          if (cnt_q >= 6'(WORD_BYTES)) consume_c = 6'(WORD_BYTES);
        end
      end
      S_DROP: begin
        if (remain_q <= 16'(WORD_BYTES)) begin
          if (16'(cnt_q) >= remain_q) consume_c = remain_q[5:0];
        end else if (cnt_q >= 6'(WORD_BYTES)) begin
          consume_c = 6'(WORD_BYTES);
        end
      end
      default: ;  // S_SETTLE consumes nothing
    endcase
  end

  logic [5:0] keep_c;                    // bytes surviving the front consume
  assign keep_c = cnt_q - consume_c;

  // Accept a beat only when a full 16 bytes fit behind the survivors. Depends
  // only on registered state — no combinational loop with upstream valid.
  assign in_ready = keep_c <= 6'(WIN_BYTES - WORD_BYTES);

  logic accept;
  assign accept = in_valid && in_ready;

  // ---- window compaction + append ------------------------------------------
  logic [7:0] win_n [WIN_BYTES];
  always_comb begin
    for (int i = 0; i < WIN_BYTES; i++) begin
      if (i < int'(keep_c))
        win_n[i] = win_q[i + int'(consume_c)];             // survivors shift down
      else if (accept && i >= int'(keep_c) && i < int'(keep_c) + int'(in_nbytes))
        win_n[i] = in_data[8 * (i - int'(keep_c)) +: 8];   // new beat appends
      else
        win_n[i] = win_q[i];                               // don't-care
    end
  end

  // ---- outputs --------------------------------------------------------------
  assign msg_len        = len_q;
  assign msg_nbytes     = consume_c[NBYTES_W-1:0];
  assign msg_beat_valid = (state == S_BODY) && (consume_c != 6'd0);
  assign msg_widx       = widx_q;
  assign msg_start      = msg_beat_valid && (widx_q == '0);
  assign msg_complete   = (state == S_SETTLE) && !dec_pending;

  for (genvar gi = 0; gi < WORD_BYTES; gi++) begin : g_msg_data
    assign msg_data[8*gi +: 8] = win_q[gi];
  end

  // ---- state ----------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst) begin
      state    <= S_HDR;
      cnt_q    <= '0;
      len_q    <= '0;
      remain_q <= '0;
      widx_q   <= '0;
    end else begin
      for (int i = 0; i < WIN_BYTES; i++) win_q[i] <= win_n[i];
      cnt_q <= keep_c + (accept ? 6'(in_nbytes) : 6'd0);

      case (state)
        S_HDR: if (cnt_q >= 6'd2) begin
          len_q    <= {win_q[0], win_q[1]};
          remain_q <= {win_q[0], win_q[1]};
          widx_q   <= '0;
          if ({win_q[0], win_q[1]} == 16'd0)
            state <= S_HDR;
          else if ({win_q[0], win_q[1]} > 16'(MSG_MAX_BYTES))
            state <= S_DROP;
          else
            state <= S_BODY;
        end
        S_BODY: if (consume_c != 6'd0) begin
          remain_q <= remain_q - 16'(consume_c);
          if (remain_q <= 16'(WORD_BYTES)) state <= S_SETTLE;  // that was the final beat
          else                             widx_q <= widx_q + WIDX_W'(1);
        end
        S_SETTLE: if (!dec_pending) state <= S_HDR;  // msg_complete pulses now
        S_DROP: if (consume_c != 6'd0) begin
          remain_q <= remain_q - 16'(consume_c);
          if (remain_q <= 16'(WORD_BYTES)) state <= S_HDR;
        end
        default:  state <= S_HDR;
      endcase
    end
  end

endmodule
