// Message framing. Wire format is [2-byte BE length][body]; body bytes stream
// downstream tagged with msg_offset and msg_start (offset 0 = the type byte).
// After the last body byte, msg_complete pulses and input stalls until the
// engine reports engine_done — this is the pipeline's backpressure point, since
// Replace and best-price scans take many cycles.
module itch_framer
  import ob_pkg::*;
(
  input  logic       clk,
  input  logic       rst,

  // upstream (from udp_stripper)
  input  logic [7:0] in_byte,
  input  logic       in_valid,
  output logic       in_ready,

  // downstream byte stream (to itch_decoder)
  output logic [7:0] msg_byte,
  output logic       msg_byte_valid,
  output logic [5:0] msg_offset,
  output logic       msg_start,
  output logic       msg_complete,   // pulse: body fully delivered & settled
  output logic [15:0] msg_len,

  // backpressure from the processing core
  input  logic       engine_done
);

  typedef enum logic [2:0] {S_LEN_HI, S_LEN_LO, S_BODY, S_COMPLETE, S_STALL} state_t;
  state_t      state;
  logic [15:0] len_q;
  logic [15:0] off_q;

  assign msg_len = len_q;

  // accept input bytes only while framing the header/body
  assign in_ready = (state == S_LEN_HI) || (state == S_LEN_LO) || (state == S_BODY);

  // byte stream out (combinational on the accepted input byte)
  assign msg_byte       = in_byte;
  assign msg_byte_valid = (state == S_BODY) && in_valid;
  assign msg_offset     = off_q[5:0];
  assign msg_start      = (state == S_BODY) && (off_q == 16'd0);
  assign msg_complete   = (state == S_COMPLETE);

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_LEN_HI;
      len_q <= '0;
      off_q <= '0;
    end else begin
      case (state)
        S_LEN_HI: if (in_valid) begin
          len_q[15:8] <= in_byte;
          state       <= S_LEN_LO;
        end
        S_LEN_LO: if (in_valid) begin
          len_q[7:0]  <= in_byte;
          off_q       <= '0;
          state       <= S_BODY;
        end
        S_BODY: if (in_valid) begin
          if (off_q == len_q - 16'd1) state <= S_COMPLETE;  // last body byte
          off_q <= off_q + 16'd1;
        end
        S_COMPLETE: state <= S_STALL;   // body settled; engine starts now
        S_STALL:    if (engine_done) state <= S_LEN_HI;
        default:    state <= S_LEN_HI;
      endcase
    end
  end

endmodule
