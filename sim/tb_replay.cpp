// tb_replay — replay a length-prefixed .itch file on whichever Verilated
// model this binary was linked against and print one machine-readable FINAL
// line (same format as `orderbook_sw --final-state`, plus band telemetry).
//
// Linked against sim/obj_rtl_band this is the banded SYNTHESIS-config book —
// the same configuration the bitstream ships — so tools/run_real_data.sh can
// diff  SW replay == banded RTL replay  on real converted data, and the
// hardware acceptance demo can diff its final snapshot against this output.
#include "rtl_driver.hpp"
#include "itch_file_reader.hpp"

#include <cstdio>
#include <string>

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.itch> [max_msgs] [band_base]\n", argv[0]);
        return 2;
    }
    std::string path = argv[1];
    uint64_t    cap  = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 0;
    long long   band = (argc > 3) ? std::strtoll(argv[3], nullptr, 10) : -1;

    std::vector<uint8_t> raw = read_itch_file(path);
    if (raw.empty()) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
    auto msgs = split_messages(raw, cap);
    auto stream = reserialize(msgs);

    RtlDriver drv; drv.reset(true);
    if (band >= 0) drv.set_band(uint32_t(band));   // pin the window like a host would
    uint64_t total = msgs.size();
    uint64_t committed = drv.drive(stream, total, [](uint64_t){});
    auto* top = drv.top();

    if (committed != total) {
        std::fprintf(stderr, "HANG: committed %llu of %llu\n",
                     (unsigned long long)committed, (unsigned long long)total);
        return 1;
    }
    std::printf("FINAL bid_px=%u bid_sh=%u ask_px=%u ask_sh=%u spread=%u "
                "vwap_num=%llu vwap_den=%llu trades=%llu msgs=%llu\n",
                top->best_bid_valid ? (unsigned)top->best_bid_price : 0,
                (unsigned)top->best_bid_shares,
                top->best_ask_valid ? (unsigned)top->best_ask_price : 0,
                (unsigned)top->best_ask_shares,
                top->spread_valid ? (unsigned)top->spread : 0,
                (unsigned long long)top->vwap_num,
                (unsigned long long)top->vwap_den,
                (unsigned long long)top->trade_count,
                (unsigned long long)top->msg_total);
    std::printf("BAND base=%u drops=%u ins_fails=%u\n",
                (unsigned)top->band_base, (unsigned)top->band_drops,
                (unsigned)top->tbl_ins_fails);
    return 0;
}
