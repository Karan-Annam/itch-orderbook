// Strips Ethernet/IP/UDP headers. Non-fragmented IPv4 without options puts the
// ITCH payload at byte 42 (14 eth + 20 ip + 8 udp), so a byte counter discards
// 0..41 and forwards from there. raw_mode=1 bypasses stripping entirely (the
// test file is raw length-prefixed ITCH with no UDP wrapper).
//
// Ready/valid with backpressure: forwarded bytes wait for downstream ready,
// discarded header bytes are always consumed. in_sop resets the counter.
module udp_stripper
  import ob_pkg::*;
(
  input  logic       clk,
  input  logic       rst,
  input  logic       raw_mode,

  input  logic [7:0] in_byte,
  input  logic       in_valid,
  input  logic       in_sop,     // first byte of a UDP frame (ignored in raw_mode)
  output logic       in_ready,

  output logic [7:0] out_byte,
  output logic       out_valid,
  input  logic       out_ready
);

  localparam int HDR_BYTES = 42;

  logic [15:0] byte_cnt;
  logic        in_payload;

  assign in_payload = raw_mode || (byte_cnt >= 16'(HDR_BYTES));

  // Forward only payload bytes; discard header bytes immediately.
  assign out_byte  = in_byte;
  assign out_valid = in_valid && in_payload;
  assign in_ready  = in_payload ? out_ready : 1'b1;

  always_ff @(posedge clk) begin
    if (rst) begin
      byte_cnt <= '0;
    end else if (in_valid && (in_sop && !raw_mode)) begin
      // start of a new packet: this byte is index 0, count it as consumed
      byte_cnt <= 16'd1;
    end else if (in_valid && in_ready) begin
      if (byte_cnt < 16'hFFFF) byte_cnt <= byte_cnt + 16'd1;
    end
  end

endmodule
