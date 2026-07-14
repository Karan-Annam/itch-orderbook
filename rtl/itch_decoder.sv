// Field decoder. Framer beats land in body[] 16 bytes at a time (beat widx
// carries body bytes 16*widx..16*widx+nbytes-1); on msg_complete the fields
// come out by plain bit-wiring — a big-endian field at body[k..k+n-1] is just
// {body[k],...,body[k+n-1]}, so the byte-swap costs nothing. Each message type
// is normalized into decoded_t and held (dec_valid) until dec_accept.
module itch_decoder
  import ob_pkg::*;
(
  input  logic                clk,
  input  logic                rst,

  input  logic [WORD_W-1:0]   msg_data,
  input  logic [NBYTES_W-1:0] msg_nbytes,
  input  logic                msg_beat_valid,
  input  logic [WIDX_W-1:0]   msg_widx,
  input  logic                msg_complete,
  input  logic [15:0]         msg_len,

  input  logic        dec_accept,     // engine latched the current message

  output decoded_t    dec,
  output logic        dec_valid
);

  logic [7:0] body [MSG_MAX_BYTES];

  // big-endian field extractors (combinational, body[k] is MSB)
  function automatic logic [15:0] be16(input int k);
    be16 = {body[k], body[k+1]};
  endfunction
  function automatic logic [31:0] be32(input int k);
    be32 = {body[k], body[k+1], body[k+2], body[k+3]};
  endfunction
  function automatic logic [63:0] be64(input int k);
    be64 = {body[k], body[k+1], body[k+2], body[k+3],
            body[k+4], body[k+5], body[k+6], body[k+7]};
  endfunction

  decoded_t d_c;  // combinational decode of current body

  always_comb begin
    d_c               = '0;
    d_c.mtype         = body[0];
    d_c.stock_locate  = be16(1);
    d_c.timestamp     = be64(5);
    d_c.order_ref     = '0;
    d_c.new_order_ref = '0;
    d_c.price         = '0;
    d_c.shares        = '0;
    d_c.is_bid        = 1'b0;
    d_c.printable     = 1'b0;
    unique case (body[0])
      T_ADD, T_ADD_MPID: begin
        d_c.order_ref = be64(13);
        d_c.is_bid    = (body[21] == SIDE_BUY);
        d_c.shares    = be32(22);
        d_c.price     = be32(34);
      end
      T_EXEC: begin
        d_c.order_ref = be64(13);
        d_c.shares    = be32(21);     // executed shares
      end
      T_EXEC_PR: begin
        d_c.order_ref = be64(13);
        d_c.shares    = be32(21);     // executed shares
        d_c.printable = (body[33] == "Y");
        d_c.price     = be32(34);     // execution price (for VWAP)
      end
      T_CANCEL: begin
        d_c.order_ref = be64(13);
        d_c.shares    = be32(21);     // cancelled shares
      end
      T_DELETE: begin
        d_c.order_ref = be64(13);
      end
      T_REPLACE: begin
        d_c.order_ref     = be64(13); // original ref
        d_c.new_order_ref = be64(21); // new ref
        d_c.shares        = be32(29); // new shares
        d_c.price         = be32(33); // new price
      end
      T_TRADE: begin
        d_c.order_ref = be64(13);     // usually 0
        d_c.is_bid    = (body[21] == SIDE_BUY);
        d_c.shares    = be32(22);
        d_c.price     = be32(34);
      end
      T_SYSTEM: begin
        // no fields needed downstream
      end
      default: ;
    endcase
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      dec_valid <= 1'b0;
      dec       <= '0;
    end else begin
      if (msg_beat_valid) begin
        for (int i = 0; i < WORD_BYTES; i++) begin
          if (i < int'(msg_nbytes) && (int'(msg_widx) * WORD_BYTES + i) < MSG_MAX_BYTES)
            body[int'(msg_widx) * WORD_BYTES + i] <= msg_data[8*i +: 8];
        end
      end
      if (msg_complete) begin
        if (body_len(body[0]) != 0 && msg_len == body_len(body[0])) begin
          dec       <= d_c;
          dec_valid <= 1'b1;
        end else begin
          dec       <= '0;
          dec_valid <= 1'b0;
        end
      end else if (dec_accept) begin
        dec_valid <= 1'b0;
      end
    end
  end

endmodule
