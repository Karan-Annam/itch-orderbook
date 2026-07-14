// Board-chain test, run under Verilator BEFORE hardware exists: bit-banged
// UART bytes -> uart_itch_bridge (OBLink framing) -> orderbook_top (banded
// SYNTHESIS config — the build defines it) -> snapshot frames back over UART.
// Proves the exact bitstream datapath minus the MMCM (bypassed under
// `VERILATOR`), so board bring-up starts from a known-good chain.
//
// Checks: empty-book snapshot, book building over the link (best/vols/VWAP/
// counters/band telemetry), checksum-corrupted frame dropped WHOLE with
// frame_err_count++ and the next frame still landing (preamble-hunt
// recovery), zero-length frame rejected, sticky attention LED.
//
// OBLink frame layouts live in uart_itch_bridge.sv; the byte constants here
// and in tools/ob_host.py must match it.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include "Vfpga_top.h"
#include "verilated.h"
#include "../../sim/itch_build.hpp"

double sc_time_stamp() { return 0; }

static Vfpga_top* top;
static int fails = 0, checks = 0;
#define CHECKB(c, msg) do { ++checks; if (!(c)) { std::printf("    CHECK failed: %s\n", msg); fails++; } } while (0)

// must match the -GUART_DIVISOR override in sim/run_board_sim.sh
static const int DIV = 16;

// software UART endpoint watching/driving the serial pins each core cycle
struct SwUart {
    int  rx_state = 0, rx_cnt = 0, rx_bit = 0;
    uint8_t rx_sh = 0;
    std::deque<uint8_t> rx_bytes;
    void sample(int txd) {
        switch (rx_state) {
            case 0: if (!txd) { rx_state = 1; rx_cnt = DIV / 2; } break;
            case 1: if (--rx_cnt == 0) { rx_state = 2; rx_cnt = DIV; rx_bit = 0; rx_sh = 0; } break;
            case 2: if (--rx_cnt == 0) {
                        rx_sh = uint8_t((rx_sh >> 1) | (txd ? 0x80 : 0));
                        rx_cnt = DIV;
                        if (++rx_bit == 8) rx_state = 3;
                    } break;
            case 3: if (--rx_cnt == 0) { if (txd) rx_bytes.push_back(rx_sh); rx_state = 0; } break;
        }
    }
    std::deque<int> tx_bits;
    int tx_cnt = 0, tx_cur = 1;
    void queue_byte(uint8_t b) {
        tx_bits.push_back(0);
        for (int i = 0; i < 8; i++) tx_bits.push_back((b >> i) & 1);
        tx_bits.push_back(1);
        tx_bits.push_back(1);
    }
    int drive() {
        if (tx_cnt == 0) {
            if (!tx_bits.empty()) { tx_cur = tx_bits.front(); tx_bits.pop_front(); tx_cnt = DIV; }
            else tx_cur = 1;
        }
        if (tx_cnt > 0) tx_cnt--;
        return tx_cur;
    }
};

static SwUart uart;

static void tick() {
    top->uart_rx = uart.drive() ? 1 : 0;
    top->clk100 = 0; top->eval();
    top->clk100 = 1; top->eval();
    uart.sample(top->uart_tx);
}

// ---- OBLink framing (mirror of uart_itch_bridge.sv / tools/ob_host.py) ----
static const int PAYLOAD_DN = 48, SNAP_BYTES = 128;

static void send_frame(uint8_t type, const std::vector<uint8_t>& payload,
                       int corrupt_at = -1) {
    std::vector<uint8_t> pay(PAYLOAD_DN, 0);
    for (size_t i = 0; i < payload.size() && i < size_t(PAYLOAD_DN); ++i)
        pay[i] = payload[i];
    uint8_t csum = type;
    for (auto b : pay) csum = uint8_t(csum + b);
    if (corrupt_at >= 0 && corrupt_at < PAYLOAD_DN) pay[corrupt_at] ^= 0xFF;
    uart.queue_byte(0xA5);
    uart.queue_byte(0x5A);
    uart.queue_byte(type);
    for (auto b : pay) uart.queue_byte(b);
    uart.queue_byte(csum);   // csum of UNcorrupted payload -> mismatch if corrupted
}

static void send_itch(const std::vector<uint8_t>& msg, int corrupt_at = -1) {
    send_frame(0x01, msg, corrupt_at);
}
static void send_snap_req() { send_frame(0x02, {}); }

// snapshot decode
struct Snap {
    uint8_t  version = 0, flags = 0;
    uint32_t bid_px = 0, bid_sh = 0, ask_px = 0, ask_sh = 0;
    uint64_t tot_bid = 0, tot_ask = 0, vwap_num = 0, vwap_den = 0, trades = 0;
    uint32_t spread = 0, band_base = 0, band_drops = 0;
    uint64_t msg_total = 0;
    uint32_t mc[9] = {0};
    uint32_t frame_errs = 0, tbl_ins_fails = 0;
};
static uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static uint64_t rd64(const uint8_t* p) {
    return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32);
}

static bool next_snapshot(Snap& s, int max_cycles) {
    std::vector<uint8_t> buf;
    int hunting = 0;
    uint8_t type = 0;
    for (int c = 0; c < max_cycles; c++) {
        tick();
        while (!uart.rx_bytes.empty()) {
            uint8_t b = uart.rx_bytes.front(); uart.rx_bytes.pop_front();
            if (hunting == 0)      { if (b == 0xA5) hunting = 1; }
            else if (hunting == 1) { hunting = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); }
            else if (hunting == 2) { type = b; hunting = 3; buf.clear(); }
            else {
                buf.push_back(b);
                if (int(buf.size()) == SNAP_BYTES + 1) {       // payload + csum
                    uint8_t csum = type;
                    for (int k = 0; k < SNAP_BYTES; k++) csum = uint8_t(csum + buf[k]);
                    if (type != 0x81 || csum != buf[SNAP_BYTES]) return false;
                    const uint8_t* p = buf.data();
                    s.version = p[0]; s.flags = p[1];
                    s.bid_px = rd32(p + 4);  s.bid_sh = rd32(p + 8);
                    s.ask_px = rd32(p + 12); s.ask_sh = rd32(p + 16);
                    s.tot_bid = rd64(p + 20); s.tot_ask = rd64(p + 28);
                    s.vwap_num = rd64(p + 36); s.vwap_den = rd64(p + 44);
                    s.trades = rd64(p + 52);
                    s.spread = rd32(p + 60); s.band_base = rd32(p + 64);
                    s.band_drops = rd32(p + 68);
                    s.msg_total = rd64(p + 72);
                    for (int k = 0; k < 9; k++) s.mc[k] = rd32(p + 80 + 4 * k);
                    s.frame_errs = rd32(p + 116);
                    s.tbl_ins_fails = rd32(p + 120);
                    return true;
                }
            }
        }
    }
    return false;
}

static void idle(int cycles) { for (int i = 0; i < cycles; i++) tick(); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    top = new Vfpga_top;

    // reset: button low, release after a few cycles
    top->ck_rstn = 0; top->uart_rx = 1;
    for (int i = 0; i < 10; i++) tick();
    top->ck_rstn = 1;
    for (int i = 0; i < 10; i++) tick();

    using namespace obsim;
    const uint32_t P0 = 600000, BASE = P0 - 512;

    // 1. empty-book snapshot
    Snap s;
    send_snap_req();
    CHECKB(next_snapshot(s, 500000), "snapshot 1 arrives");
    CHECKB(s.version == 1, "version");
    CHECKB(s.flags == 0, "empty book: no valid flags");
    CHECKB(s.msg_total == 0 && s.frame_errs == 0, "clean start");

    // 1b. SET_BAND pins the window before any traffic (band_base readable
    // in the snapshot while msg_total is still 0 — impossible via auto-init)
    send_frame(0x03, {uint8_t(BASE), uint8_t(BASE >> 8),
                      uint8_t(BASE >> 16), uint8_t(BASE >> 24)});
    send_snap_req();
    CHECKB(next_snapshot(s, 500000), "snapshot 1b arrives");
    CHECKB(s.band_base == BASE && s.msg_total == 0, "band pinned by SET_BAND");

    // 2. build a book over the link
    send_itch(add(1, 'B', 100, P0, 1));
    send_itch(add(2, 'S', 80, P0 + 100, 1));
    send_itch(exec(1, 30));
    send_snap_req();
    CHECKB(next_snapshot(s, 800000), "snapshot 2 arrives");
    CHECKB(s.flags == 0x7, "bid+ask+spread valid");
    CHECKB(s.bid_px == P0 && s.bid_sh == 70, "best bid 600000(70)");
    CHECKB(s.ask_px == P0 + 100 && s.ask_sh == 80, "best ask 600100(80)");
    CHECKB(s.tot_bid == 70 && s.tot_ask == 80, "volumes");
    CHECKB(s.vwap_num == uint64_t(P0) * 30 && s.vwap_den == 30, "vwap at resting px");
    CHECKB(s.trades == 1 && s.msg_total == 3, "counters");
    CHECKB(s.mc[0] == 2 && s.mc[2] == 1, "mc_add=2 mc_exec=1");
    CHECKB(s.spread == 100, "spread");
    CHECKB(s.band_base == BASE && s.band_drops == 0, "band centered, no drops");

    // 3. checksum-corrupted frame: dropped whole, book untouched
    send_itch(del(2), /*corrupt_at=*/5);
    idle(20000);                                   // let it hit the bridge
    send_snap_req();
    CHECKB(next_snapshot(s, 800000), "snapshot 3 arrives");
    CHECKB(s.msg_total == 3, "corrupt frame did not reach the book");
    CHECKB(s.frame_errs == 1, "frame_err_count incremented");
    CHECKB((s.flags & 0x2) != 0, "ask still valid");
    CHECKB(top->led & 0x8, "attention LED sticky after corruption");

    // 4. recovery: the very next valid frame lands
    send_itch(del(2));
    send_snap_req();
    CHECKB(next_snapshot(s, 800000), "snapshot 4 arrives");
    CHECKB(s.msg_total == 4 && s.mc[5] == 1, "delete applied after resync");
    CHECKB((s.flags & 0x2) == 0, "ask side emptied");

    // 5. zero-length ITCH frame rejected
    send_frame(0x01, {0x00, 0x00});
    idle(20000);
    send_snap_req();
    CHECKB(next_snapshot(s, 800000), "snapshot 5 arrives");
    CHECKB(s.frame_errs == 2 && s.msg_total == 4, "bad length rejected");

    std::printf("[board] %d checks, %d failures\n", checks, fails);
    delete top;
    return fails ? 1 : 0;
}
