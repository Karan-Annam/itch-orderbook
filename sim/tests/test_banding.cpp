// Price-banding semantics, run against the SYNTHESIS-config model (built with
// +define+SYNTHESIS: PRICE_LEVELS = 1024, band auto-init enabled, dbg port
// tied off). This is the exact configuration that ships to Vivado.
//
// Checks:
//   * band_base auto-centers on the FIRST Add (price - PRICE_LEVELS/2),
//   * both window edges: offset 0 and PRICE_LEVELS-1 are in, -1 and
//     PRICE_LEVELS are out,
//   * out-of-window Adds are dropped WHOLE (no volume, no best change, no
//     table entry — a later op on that ref is a no-op) and counted,
//   * a Replace whose new price is out-of-window applies the remove but
//     drops the re-add (new ref never exists),
//   * in-window behaviour (best tracking, scans, VWAP, spread) is unaffected
//     and reported prices are real prices, not level addresses.
//
// The C++ reference model has no banding, so all expectations are manual.
#include "scenario.hpp"

using namespace obsim;

static constexpr uint32_t WIN  = 1024;              // PRICE_LEVELS (SYNTHESIS)
static constexpr uint32_t P0   = 600000;            // first add -> base = P0-512
static constexpr uint32_t BASE = P0 - WIN / 2;      // 599488
static constexpr uint32_t LO   = BASE;              // offset 0    (in)
static constexpr uint32_t HI   = BASE + WIN - 1;    // offset 1023 (in)

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    TestCtx ctx{0, 0, "banding"};

    std::vector<uint8_t> s;
    append(s, add(1, 'B', 100, P0, 1));        // 0: first add sets the band
    append(s, add(2, 'B',  50, LO, 1));        // 1: lower edge, in
    append(s, add(3, 'B',  75, LO - 1, 1));    // 2: below window -> drop
    append(s, add(4, 'S',  80, HI, 1));        // 3: upper edge, in
    append(s, add(5, 'S',  60, HI + 1, 1));    // 4: above window -> drop
    append(s, del(3));                         // 5: ref 3 was dropped -> no-op
    append(s, exec(4, 30));                    // 6: trade 30 @ HI (resting px)
    append(s, replace(1, 10, 40, 700000));     // 7: remove ok, re-add out -> drop
    append(s, exec(10, 20));                   // 8: ref 10 never existed -> no-op
    append(s, replace(2, 11, 40, P0));         // 9: in-window replace
    const uint64_t total = 10;

    RtlDriver drv; drv.reset(true);
    auto* top = drv.top();
    SCHECK(ctx, top->band_base == 0 && top->band_drops == 0,
           "pre-stream band_base=%u drops=%u", (unsigned)top->band_base,
           (unsigned)top->band_drops);

    uint64_t committed = run_stream(drv, s, total, [&](uint64_t idx){
        switch (idx) {
        case 0:  // band initialised, centered on P0
            SCHECK(ctx, top->band_base == BASE, "@0 base=%u exp=%u",
                   (unsigned)top->band_base, BASE);
            SCHECK(ctx, top->best_bid_price == P0 && top->best_bid_shares == 100,
                   "@0 best_bid=%u(%u)", (unsigned)top->best_bid_price,
                   (unsigned)top->best_bid_shares);
            break;
        case 1:  // lower edge accepted; best unchanged; real price reported
            SCHECK(ctx, top->tot_bid_vol == 150 && top->best_bid_price == P0,
                   "@1 tot_bid=%llu best=%u", (unsigned long long)top->tot_bid_vol,
                   (unsigned)top->best_bid_price);
            break;
        case 2:  // below window: dropped whole
            SCHECK(ctx, top->band_drops == 1 && top->tot_bid_vol == 150 &&
                        top->best_bid_price == P0,
                   "@2 drops=%u tot_bid=%llu", (unsigned)top->band_drops,
                   (unsigned long long)top->tot_bid_vol);
            break;
        case 3:  // upper edge accepted
            SCHECK(ctx, top->best_ask_price == HI && top->best_ask_shares == 80 &&
                        top->tot_ask_vol == 80,
                   "@3 best_ask=%u(%u)", (unsigned)top->best_ask_price,
                   (unsigned)top->best_ask_shares);
            SCHECK(ctx, top->spread_valid && top->spread == HI - P0,
                   "@3 spread=%u exp=%u", (unsigned)top->spread, HI - P0);
            break;
        case 4:  // above window: dropped whole
            SCHECK(ctx, top->band_drops == 2 && top->tot_ask_vol == 80,
                   "@4 drops=%u tot_ask=%llu", (unsigned)top->band_drops,
                   (unsigned long long)top->tot_ask_vol);
            break;
        case 5:  // delete of a dropped ref: unknown-ref no-op
            SCHECK(ctx, top->tot_bid_vol == 150 && top->tot_ask_vol == 80,
                   "@5 vols changed on no-op delete");
            break;
        case 6:  // exec at resting price HI feeds VWAP with the REAL price
            SCHECK(ctx, top->tot_ask_vol == 50 && top->best_ask_shares == 50,
                   "@6 tot_ask=%llu", (unsigned long long)top->tot_ask_vol);
            SCHECK(ctx, top->vwap_num == uint64_t(HI) * 30 && top->vwap_den == 30 &&
                        top->trade_count == 1,
                   "@6 vwap=%llu/%llu", (unsigned long long)top->vwap_num,
                   (unsigned long long)top->vwap_den);
            break;
        case 7:  // replace: remove applied (best falls to LO), re-add dropped
            SCHECK(ctx, top->band_drops == 3 && top->tot_bid_vol == 50 &&
                        top->best_bid_valid && top->best_bid_price == LO &&
                        top->best_bid_shares == 50,
                   "@7 drops=%u best_bid=%u(%u) tot=%llu",
                   (unsigned)top->band_drops, (unsigned)top->best_bid_price,
                   (unsigned)top->best_bid_shares,
                   (unsigned long long)top->tot_bid_vol);
            break;
        case 8:  // the dropped re-add's ref never existed
            SCHECK(ctx, top->tot_bid_vol == 50 && top->vwap_den == 30,
                   "@8 no-op exec changed state");
            break;
        case 9:  // in-window replace: side emptied then re-added at P0
            SCHECK(ctx, top->best_bid_price == P0 && top->best_bid_shares == 40 &&
                        top->tot_bid_vol == 40,
                   "@9 best_bid=%u(%u)", (unsigned)top->best_bid_price,
                   (unsigned)top->best_bid_shares);
            break;
        default: break;
        }
    });
    SCHECK(ctx, committed == total, "committed %llu/%llu",
           (unsigned long long)committed, (unsigned long long)total);

    // band never re-centers; drops totalled; message counters include drops
    SCHECK(ctx, top->band_base == BASE, "final base=%u exp=%u",
           (unsigned)top->band_base, BASE);
    SCHECK(ctx, top->band_drops == 3, "final drops=%u exp=3",
           (unsigned)top->band_drops);
    SCHECK(ctx, top->mc_add == 5 && top->mc_replace == 2 && top->mc_delete == 1 &&
                top->mc_exec == 2,
           "counters add=%u repl=%u del=%u exec=%u", (unsigned)top->mc_add,
           (unsigned)top->mc_replace, (unsigned)top->mc_delete,
           (unsigned)top->mc_exec);

    // ---- pinned-band config: a host-set window replaces auto-centering ----
    // The FIRST add lands below the pinned window and must be dropped —
    // under auto-init it would have re-centered the band on itself instead.
    RtlDriver drv2; drv2.reset(true);
    drv2.set_band(BASE);
    auto* top2 = drv2.top();
    std::vector<uint8_t> s2;
    append(s2, add(21, 'B', 10, LO - 1, 1));   // first add, out-of-window
    append(s2, add(22, 'B', 10, P0, 1));       // in-window
    uint64_t c2 = run_stream(drv2, s2, 2, [&](uint64_t){});
    SCHECK(ctx, c2 == 2, "cfg: committed 2");
    SCHECK(ctx, top2->band_base == BASE, "cfg: pinned base=%u exp=%u",
           (unsigned)top2->band_base, BASE);
    SCHECK(ctx, top2->band_drops == 1, "cfg: first add dropped, no recenter (drops=%u)",
           (unsigned)top2->band_drops);
    SCHECK(ctx, top2->best_bid_valid && top2->best_bid_price == P0 &&
                top2->best_bid_shares == 10,
           "cfg: in-window add applied (best=%u(%u))",
           (unsigned)top2->best_bid_price, (unsigned)top2->best_bid_shares);

    std::printf("[banding] %d checks, %d failures\n", ctx.checks, ctx.fails);
    return ctx.fails ? 1 : 0;
}
