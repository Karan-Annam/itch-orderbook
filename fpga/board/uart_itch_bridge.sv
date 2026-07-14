// UART <-> order-book transport bridge ("OBLink"). Carries ITCH messages down
// to orderbook_top's ingest bus and book-state snapshots back up, over plain
// 8N1 serial. UART engines (RX 2FF-sync + mid-bit sample, TX) are lifted from
// the NMC project's uart_flit_bridge; the framing layer is adapted:
//
//   host -> FPGA, fixed 52 bytes:
//     [0]=0xA5 [1]=0x5A  preamble (hunt resyncs after a lost byte)
//     [2]    type: 0x01 ITCH_MSG | 0x02 SNAP_REQ | 0x03 SET_BAND
//     [3:50] payload[48]: ITCH_MSG = one length-prefixed message
//            ([2B BE len][body], len 1..46), zero-padded to 48;
//            SET_BAND = band base u32 LE in bytes [0..3] (pin the book's
//            price window before streaming a symbol)
//     [51]   csum: 8-bit sum of bytes [2..50]
//   FPGA -> host, fixed 132 bytes on each SNAP_REQ:
//     A5 5A | 0x81 | payload[128] (LE fields, layout below) | csum over [2..130]
//
// A checksum-or-length reject drops the frame whole (frame_err_count++,
// sticky error out) and the preamble hunt recovers on the next frame — one
// corrupted UART byte can never leak bytes into the book's framer, which
// would silently corrupt every message after it.
//
// Snapshot payload layout (offsets in bytes, little-endian; keep in lockstep
// with fpga/board/test_board.cpp and tools/ob_host.py):
//   0  u8  version=1     1  u8  flags(b0 bid_v,b1 ask_v,b2 spread_v)  2 u16 rsvd
//   4  u32 bid_px        8  u32 bid_sh    12 u32 ask_px   16 u32 ask_sh
//   20 u64 tot_bid_vol  28 u64 tot_ask_vol
//   36 u64 vwap_num     44 u64 vwap_den   52 u64 trade_count
//   60 u32 spread       64 u32 band_base  68 u32 band_drops
//   72 u64 msg_total    80 u32 x9 mc (A F E C X D U P S)
//   116 u32 frame_err_count   120 u32 tbl_ins_fails   124..127 rsvd
//
// UART is orders of magnitude slower than the 100 MHz core, so one frame
// buffer per direction suffices; the ITCH feeder pushes 1 byte/beat into the
// 16-byte ingest bus honoring in_ready (the bus is ~3 decades faster than
// the link — width is wasted, correctness is not).
module uart_itch_bridge
  import ob_pkg::*;
#(
  parameter int unsigned DIVISOR = 868   // 100 MHz / 115200 baud
)(
  input  logic        clk,
  input  logic        rst_n,

  // serial pins
  input  logic        uart_rx,
  output logic        uart_tx,

  // ingest side (connects to orderbook_top)
  output logic [WORD_W-1:0]   in_data,
  output logic [NBYTES_W-1:0] in_nbytes,
  output logic                in_valid,
  input  logic                in_ready,

  // band config (connects to orderbook_top band_cfg_*)
  output logic                band_cfg_valid,
  output logic [31:0]         band_cfg_base,

  // book state to snapshot (connects to orderbook_top outputs)
  input  logic        best_bid_valid,
  input  logic [31:0] best_bid_price,
  input  logic [31:0] best_bid_shares,
  input  logic        best_ask_valid,
  input  logic [31:0] best_ask_price,
  input  logic [31:0] best_ask_shares,
  input  logic [63:0] tot_bid_vol,
  input  logic [63:0] tot_ask_vol,
  input  logic [63:0] vwap_num,
  input  logic [63:0] vwap_den,
  input  logic [63:0] trade_count,
  input  logic        spread_valid,
  input  logic [31:0] spread,
  input  logic [31:0] band_base,
  input  logic [31:0] band_drops,
  input  logic [63:0] msg_total,
  input  logic [31:0] mc_add, mc_addmpid, mc_exec, mc_execpr, mc_cancel,
  input  logic [31:0] mc_delete, mc_replace, mc_trade, mc_system,
  input  logic [31:0] tbl_ins_fails,

  // observability (LEDs / debug)
  output logic        rx_frame_pulse,   // a good frame was accepted
  output logic        tx_frame_pulse,   // a snapshot finished sending
  output logic        frame_err_sticky
);

  localparam int unsigned DW = $clog2(DIVISOR);

  localparam int PAYLOAD_DN = 48;                 // downstream payload bytes
  localparam int SNAP_BYTES = 128;                // upstream payload bytes
  localparam logic [7:0] T_ITCH = 8'h01, T_SNAP = 8'h02, T_BAND = 8'h03;

  // ---------------- UART RX (2FF sync, mid-bit sample) ----------------
  logic rxd_m, rxd_s;
  always_ff @(posedge clk) begin
    rxd_m <= uart_rx;
    rxd_s <= rxd_m;
  end

  logic [DW:0]  rxdiv_q;
  logic [3:0]   rxbit_q;
  logic         rx_busy_q;
  logic [7:0]   rxsh_q;
  logic         rxb_v;         // one-cycle: rxb_q holds a received byte
  logic [7:0]   rxb_q;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rxdiv_q <= '0; rxbit_q <= '0; rx_busy_q <= 1'b0;
      rxsh_q <= '0; rxb_v <= 1'b0; rxb_q <= '0;
    end else begin
      rxb_v <= 1'b0;
      if (!rx_busy_q) begin
        if (!rxd_s) begin                          // start bit edge
          rx_busy_q <= 1'b1;
          rxdiv_q   <= (DW+1)'(DIVISOR / 2);       // sample mid-bit
          rxbit_q   <= 4'd0;
        end
      end else if (rxdiv_q != '0) begin
        rxdiv_q <= rxdiv_q - 1'b1;
      end else begin
        rxdiv_q <= (DW+1)'(DIVISOR - 1);
        if (rxbit_q == 4'd0) begin                 // start-bit centre
          if (rxd_s) rx_busy_q <= 1'b0;            // glitch: abort
          rxbit_q <= 4'd1;
        end else if (rxbit_q <= 4'd8) begin        // data bits, LSB first
          rxsh_q  <= {rxd_s, rxsh_q[7:1]};
          rxbit_q <= rxbit_q + 4'd1;
        end else begin                             // stop bit
          rx_busy_q <= 1'b0;
          if (rxd_s) begin
            rxb_v <= 1'b1;
            rxb_q <= rxsh_q;
          end
        end
      end
    end
  end

  // ---------------- UART TX ----------------
  logic [DW:0] txdiv_q;
  logic [3:0]  txbit_q;
  logic        tx_busy_q;
  logic [9:0]  txsh_q;
  logic        txb_go;
  logic [7:0]  txb_q;

  assign uart_tx = tx_busy_q ? txsh_q[0] : 1'b1;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      txdiv_q <= '0; txbit_q <= '0; tx_busy_q <= 1'b0; txsh_q <= '1;
    end else if (!tx_busy_q) begin
      if (txb_go) begin
        tx_busy_q <= 1'b1;
        txsh_q    <= {1'b1, txb_q, 1'b0};
        txbit_q   <= 4'd0;
        txdiv_q   <= (DW+1)'(DIVISOR - 1);
      end
    end else if (txdiv_q != '0) begin
      txdiv_q <= txdiv_q - 1'b1;
    end else begin
      txdiv_q <= (DW+1)'(DIVISOR - 1);
      if (txbit_q == 4'd9) tx_busy_q <= 1'b0;
      else begin
        txsh_q  <= {1'b1, txsh_q[9:1]};
        txbit_q <= txbit_q + 4'd1;
      end
    end
  end

  // ---------------- frame RX + ITCH feeder ----------------
  typedef enum logic [2:0] { FR_MAG0, FR_MAG1, FR_TYPE, FR_BODY, FR_CSUM,
                             FR_FEED, FR_SNAP } frx_e;
  frx_e frx_q;
  logic [7:0]  ftype_q;
  logic [5:0]  frx_cnt_q;                          // 0..47 payload index
  logic [7:0]  pay_q [PAYLOAD_DN];
  logic [7:0]  csum_q;                             // running sum of [type..payload]
  logic [31:0] frame_err_q;
  logic [5:0]  feed_idx_q, feed_len_q;             // ITCH feed cursor / 2+len
  logic        snap_go;                            // pulse into TX FSM
  logic        snap_busy;                          // TX FSM serializing

  logic [15:0] itch_len;
  assign itch_len = {pay_q[0], pay_q[1]};          // big-endian length prefix

  // 1-byte-per-beat feeder into the ingest bus
  assign in_data   = {{(WORD_W-8){1'b0}}, pay_q[feed_idx_q]};
  assign in_nbytes = NBYTES_W'(1);
  assign in_valid  = (frx_q == FR_FEED);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      frx_q <= FR_MAG0; frx_cnt_q <= '0; ftype_q <= '0; csum_q <= '0;
      frame_err_q <= '0; feed_idx_q <= '0; feed_len_q <= '0;
      rx_frame_pulse <= 1'b0; frame_err_sticky <= 1'b0; snap_go <= 1'b0;
      band_cfg_valid <= 1'b0; band_cfg_base <= '0;
    end else begin
      rx_frame_pulse <= 1'b0;
      snap_go        <= 1'b0;
      band_cfg_valid <= 1'b0;
      unique case (frx_q)
        FR_MAG0: if (rxb_v) begin
          if (rxb_q == 8'hA5) frx_q <= FR_MAG1;
          else                frame_err_sticky <= 1'b1;   // hunting
        end
        FR_MAG1: if (rxb_v) begin
          if (rxb_q == 8'h5A) begin
            frx_q  <= FR_TYPE;
            csum_q <= '0;
          end else begin
            frame_err_sticky <= 1'b1;
            frx_q <= (rxb_q == 8'hA5) ? FR_MAG1 : FR_MAG0;
          end
        end
        FR_TYPE: if (rxb_v) begin
          ftype_q   <= rxb_q;
          csum_q    <= rxb_q;
          frx_cnt_q <= '0;
          frx_q     <= FR_BODY;
        end
        FR_BODY: if (rxb_v) begin
          pay_q[frx_cnt_q] <= rxb_q;
          csum_q <= csum_q + rxb_q;
          if (frx_cnt_q == 6'(PAYLOAD_DN - 1)) frx_q <= FR_CSUM;
          else frx_cnt_q <= frx_cnt_q + 6'd1;
        end
        FR_CSUM: if (rxb_v) begin
          if (rxb_q != csum_q) begin
            frame_err_q      <= frame_err_q + 1'b1;       // corrupt: drop whole
            frame_err_sticky <= 1'b1;
            frx_q            <= FR_MAG0;
          end else if (ftype_q == T_ITCH) begin
            if (itch_len == 0 || itch_len > 16'(MSG_MAX_BYTES - 2)) begin
              frame_err_q      <= frame_err_q + 1'b1;     // bad length: drop
              frame_err_sticky <= 1'b1;
              frx_q            <= FR_MAG0;
            end else begin
              feed_idx_q <= '0;
              feed_len_q <= 6'(itch_len + 16'd2);         // prefix + body
              frx_q      <= FR_FEED;
            end
          end else if (ftype_q == T_SNAP) begin
            snap_go <= 1'b1;
            frx_q   <= FR_SNAP;
          end else if (ftype_q == T_BAND) begin
            band_cfg_base  <= {pay_q[3], pay_q[2], pay_q[1], pay_q[0]};  // LE
            band_cfg_valid <= 1'b1;                       // one-cycle pulse
            rx_frame_pulse <= 1'b1;
            frx_q          <= FR_MAG0;
          end else begin
            frame_err_q      <= frame_err_q + 1'b1;       // unknown type
            frame_err_sticky <= 1'b1;
            frx_q            <= FR_MAG0;
          end
        end
        FR_FEED: if (in_ready) begin                      // in_valid is high here
          if (feed_idx_q == feed_len_q - 6'd1) begin
            rx_frame_pulse <= 1'b1;
            frx_q          <= FR_MAG0;
          end else begin
            feed_idx_q <= feed_idx_q + 6'd1;
          end
        end
        FR_SNAP: if (!snap_busy && !snap_go) begin        // wait serializer done
          rx_frame_pulse <= 1'b1;
          frx_q          <= FR_MAG0;
        end
        default: frx_q <= FR_MAG0;
      endcase
    end
  end

  // ---------------- snapshot latch + frame TX ----------------
  logic [8*SNAP_BYTES-1:0] snap_q;                 // byte k = bits [8k+7:8k]
  logic [8*SNAP_BYTES-1:0] snap_now;

  always_comb begin
    snap_now = '0;
    snap_now[8*0   +: 8]  = 8'd1;                                  // version
    snap_now[8*1   +: 8]  = {5'b0, spread_valid, best_ask_valid, best_bid_valid};
    snap_now[8*4   +: 32] = best_bid_price;
    snap_now[8*8   +: 32] = best_bid_shares;
    snap_now[8*12  +: 32] = best_ask_price;
    snap_now[8*16  +: 32] = best_ask_shares;
    snap_now[8*20  +: 64] = tot_bid_vol;
    snap_now[8*28  +: 64] = tot_ask_vol;
    snap_now[8*36  +: 64] = vwap_num;
    snap_now[8*44  +: 64] = vwap_den;
    snap_now[8*52  +: 64] = trade_count;
    snap_now[8*60  +: 32] = spread;
    snap_now[8*64  +: 32] = band_base;
    snap_now[8*68  +: 32] = band_drops;
    snap_now[8*72  +: 64] = msg_total;
    snap_now[8*80  +: 32] = mc_add;
    snap_now[8*84  +: 32] = mc_addmpid;
    snap_now[8*88  +: 32] = mc_exec;
    snap_now[8*92  +: 32] = mc_execpr;
    snap_now[8*96  +: 32] = mc_cancel;
    snap_now[8*100 +: 32] = mc_delete;
    snap_now[8*104 +: 32] = mc_replace;
    snap_now[8*108 +: 32] = mc_trade;
    snap_now[8*112 +: 32] = mc_system;
    snap_now[8*116 +: 32] = frame_err_q;
    snap_now[8*120 +: 32] = tbl_ins_fails;
  end

  typedef enum logic [2:0] { FT_IDLE, FT_MAG0, FT_MAG1, FT_TYPE, FT_BODY,
                             FT_CSUM } ftx_e;
  ftx_e ftx_q;
  logic [7:0] ftx_cnt_q;
  logic [7:0] tx_csum_q;

  assign snap_busy = (ftx_q != FT_IDLE);

  always_comb begin
    txb_go = 1'b0;
    txb_q  = 8'h00;
    if (!tx_busy_q) begin
      unique case (ftx_q)
        FT_MAG0: begin txb_go = 1'b1; txb_q = 8'hA5; end
        FT_MAG1: begin txb_go = 1'b1; txb_q = 8'h5A; end
        FT_TYPE: begin txb_go = 1'b1; txb_q = 8'h81; end
        FT_BODY: begin txb_go = 1'b1; txb_q = snap_q[8*ftx_cnt_q +: 8]; end
        FT_CSUM: begin txb_go = 1'b1; txb_q = tx_csum_q; end
        default: ;
      endcase
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      ftx_q <= FT_IDLE; ftx_cnt_q <= '0; snap_q <= '0; tx_csum_q <= '0;
      tx_frame_pulse <= 1'b0;
    end else begin
      tx_frame_pulse <= 1'b0;
      unique case (ftx_q)
        FT_IDLE: if (snap_go) begin
          snap_q <= snap_now;                      // atomic single-cycle latch
          ftx_q  <= FT_MAG0;
        end
        FT_MAG0: if (txb_go) ftx_q <= FT_MAG1;
        FT_MAG1: if (txb_go) begin
          ftx_q     <= FT_TYPE;
          tx_csum_q <= '0;
        end
        FT_TYPE: if (txb_go) begin
          tx_csum_q <= 8'h81;
          ftx_cnt_q <= '0;
          ftx_q     <= FT_BODY;
        end
        FT_BODY: if (txb_go) begin
          tx_csum_q <= tx_csum_q + snap_q[8*ftx_cnt_q +: 8];
          if (ftx_cnt_q == 8'(SNAP_BYTES - 1)) ftx_q <= FT_CSUM;
          else ftx_cnt_q <= ftx_cnt_q + 8'd1;
        end
        FT_CSUM: if (txb_go) begin
          tx_frame_pulse <= 1'b1;
          ftx_q <= FT_IDLE;
        end
        default: ftx_q <= FT_IDLE;
      endcase
    end
  end

endmodule
