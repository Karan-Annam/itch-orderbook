// Strips Ethernet/IP/UDP headers from a 16-byte-per-beat word stream.
// Non-fragmented IPv4 without options puts the ITCH payload at byte 42
// (14 eth + 20 ip + 8 udp) = word 2, byte offset 10 — so words 0-1 are
// discarded whole, word 2 is repacked to lane 0 with a constant shift, and
// words 3+ pass through untouched (byte 48 is word-aligned again).
// raw_mode=1 bypasses stripping entirely (the test file is raw
// length-prefixed ITCH with no UDP wrapper).
//
// Beats are packed-contiguous: bytes 0..nbytes-1 of in_data are valid, byte i
// lives at in_data[8*i +: 8]. Partial beats (nbytes < 16) are legal anywhere.
// in_sop marks the first word of a datagram and takes effect combinationally
// (the byte-serial version consulted a stale counter on the sop byte, leaking
// one header byte of a second datagram as payload).
module udp_stripper
  import ob_pkg::*;
(
  input  logic                clk,
  input  logic                rst,
  input  logic                raw_mode,

  input  logic [WORD_W-1:0]   in_data,
  input  logic [NBYTES_W-1:0] in_nbytes,
  input  logic                in_valid,
  input  logic                in_sop,     // first word of a UDP frame (ignored in raw_mode)
  output logic                in_ready,

  output logic [WORD_W-1:0]   out_data,
  output logic [NBYTES_W-1:0] out_nbytes,
  output logic                out_valid,
  input  logic                out_ready
);

  localparam int HDR_BYTES      = 42;
  localparam int HDR_SKIP_WORDS = HDR_BYTES / WORD_BYTES;  // 2 whole header words
  localparam int HDR_SKIP_REM   = HDR_BYTES % WORD_BYTES;  // 10 header bytes of word 2

  logic [15:0] word_idx;
  logic [15:0] word_idx_eff;   // sop forces word 0 in the same cycle
  logic        hdr_word;       // this beat is entirely header — discard
  logic        split_word;     // this beat straddles the header/payload boundary

  assign word_idx_eff = (in_valid && in_sop && !raw_mode) ? 16'd0 : word_idx;
  assign hdr_word     = !raw_mode && (word_idx_eff <  16'(HDR_SKIP_WORDS));
  assign split_word   = !raw_mode && (word_idx_eff == 16'(HDR_SKIP_WORDS));

  always_comb begin
    if (split_word) begin
      out_data   = in_data >> (8 * HDR_SKIP_REM);
      out_nbytes = (in_nbytes > NBYTES_W'(HDR_SKIP_REM))
                   ? in_nbytes - NBYTES_W'(HDR_SKIP_REM) : '0;
      out_valid  = in_valid && (in_nbytes > NBYTES_W'(HDR_SKIP_REM));
    end else begin
      out_data   = in_data;
      out_nbytes = in_nbytes;
      out_valid  = in_valid && !hdr_word;
    end
  end

  // in_ready must not depend on in_valid/in_sop: the driver samples it before
  // presenting a word, so it has to stay true for whatever arrives next cycle.
  // Whole header words are always consumable; everything else (payload words,
  // the split word, and a sop word arriving mid-payload-region — harmlessly
  // conservative) waits for downstream ready.
  assign in_ready = out_ready || (!raw_mode && (word_idx < 16'(HDR_SKIP_WORDS)));

  always_ff @(posedge clk) begin
    if (rst) begin
      word_idx <= '0;
    end else if (in_valid && in_ready) begin
      if (word_idx_eff < 16'hFFFF) word_idx <= word_idx_eff + 16'd1;
    end
  end

endmodule
