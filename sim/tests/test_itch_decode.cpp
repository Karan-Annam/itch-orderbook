// itch_framer + itch_decoder field extraction.
//
// Crafts one message of each type with known field values and checks the
// decoded-tap outputs (the decoder's byte-swapped fields) match — proving the
// big-endian wiring and per-type offsets in the RTL agree with the C++ layout.
#include "scenario.hpp"

using namespace obsim;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "itch_decode"};

    // Build a stream with one of each interesting type. Refs/prices chosen with
    // distinct nonzero bytes so a byte-swap error would be obvious.
    std::vector<uint8_t> s;
    append(s, add(0x1122334455667788ULL, 'B', 250, 1500600, 1)); // A bid
    append(s, add(0x0000000000000009ULL, 'S', 100,  990000, 1)); // A ask
    append(s, exec(0x1122334455667788ULL, 50));                  // E
    append(s, exec_price(0x1122334455667788ULL, 25, 1500700, true)); // C printable
    append(s, cancel(0x1122334455667788ULL, 10));               // X
    append(s, replace(0x1122334455667788ULL, 0xABCDEF01ULL, 75, 1500500)); // U
    append(s, del(0x00000009ULL));                              // D
    append(s, trade('B', 300, 1234500));                        // P
    append(s, sysevt('Q'));                                     // S

    RtlDriver drv; drv.reset(true);
    auto taps = run_capture_decode(drv, s, 9);
    SCHECK(ctx, taps.size() == 9, "captured %zu taps, expected 9", taps.size());
    if (taps.size() == 9) {
        // A bid
        SCHECK(ctx, taps[0].type=='A' && taps[0].is_bid && taps[0].ref==0x1122334455667788ULL
                    && taps[0].shares==250 && taps[0].price==1500600,
               "A bid decode ref=%llu sh=%u px=%u bid=%d",
               (unsigned long long)taps[0].ref, taps[0].shares, taps[0].price, taps[0].is_bid);
        // A ask
        SCHECK(ctx, taps[1].type=='A' && !taps[1].is_bid && taps[1].ref==9
                    && taps[1].shares==100 && taps[1].price==990000, "A ask decode");
        // E
        SCHECK(ctx, taps[2].type=='E' && taps[2].ref==0x1122334455667788ULL && taps[2].shares==50,
               "E decode ref=%llu sh=%u", (unsigned long long)taps[2].ref, taps[2].shares);
        // C printable + execution price
        SCHECK(ctx, taps[3].type=='C' && taps[3].printable && taps[3].price==1500700
                    && taps[3].shares==25, "C decode px=%u sh=%u pr=%d",
               taps[3].price, taps[3].shares, taps[3].printable);
        // X
        SCHECK(ctx, taps[4].type=='X' && taps[4].ref==0x1122334455667788ULL && taps[4].shares==10,
               "X decode");
        // U with orig + new ref
        SCHECK(ctx, taps[5].type=='U' && taps[5].ref==0x1122334455667788ULL
                    && taps[5].new_ref==0xABCDEF01ULL && taps[5].shares==75 && taps[5].price==1500500,
               "U decode ref=%llu nref=%llu", (unsigned long long)taps[5].ref,
               (unsigned long long)taps[5].new_ref);
        // D
        SCHECK(ctx, taps[6].type=='D' && taps[6].ref==9, "D decode ref=%llu",
               (unsigned long long)taps[6].ref);
        // P trade
        SCHECK(ctx, taps[7].type=='P' && taps[7].shares==300 && taps[7].price==1234500, "P decode");
        // S
        SCHECK(ctx, taps[8].type=='S', "S decode");
    }

    std::printf("[itch_decode] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
