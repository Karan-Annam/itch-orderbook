# Current State & Roadmap — Order Book

Snapshot as of 2026-07-13.

## Current state

| component | state | verified by |
|---|---|---|
| ITCH parser + SIMD scan (SW) | ✅ | directed tests; scalar path as SIMD oracle |
| Order-ref hash table (SW, Wang/SoA/backward-shift) | ✅ | 200k-op differential vs `std::unordered_map` |
| Direct-indexed book + vectorized best rescan (SW) | ✅ | bit-for-bit diff vs `std::map` reference book |
| Receivers (file replay; raw socket & AF_XDP Linux-only) | ✅ code-complete | file replay exercised; kernel paths **never run live** (no feed/libxdp on this machine) |
| Stats engine (VWAP, spread, imbalance, depth) | ✅ | `test_stats` |
| RTL pipeline (framer/decoder/table/engine/trackers/stats) | ✅, lint-clean | 8 Verilator testbenches + 20k+ message full-state reference diff |
| Wide front end (16 B/cycle + overlap) | ✅ | `test_framer_wide`, `test_overlap`; 4.2× measured |
| Perf/latency harness (`tb_latency`) | ✅ | service-time histograms, per-type CSVs |
| Streamlit GUI (3 tabs, shells to the same binaries) | ✅ | manual use; run artifacts in `gui/runs/` |
| Synthetic generator + bench | ✅ | byte-exact stream consumed by both sides |
| FPGA synthesis config (banded 1024-level book, de-structed RAMs) | ✅ timing MET 100 MHz on xc7s50 (WNS +0.27) | `test_banding` on the exact `SYNTHESIS` model; `fpga/reports/` |
| MoldUDP64 decap + sequence tracking (RTL `mold_stripper` + SW `mold_parser`) | ✅ | `test_mold` (RTL), `test_mold_parser` (SW), gap/stale/heartbeat/end vectors |
| Board port: UART OBLink bridge + fpga_top + XDCs (Urbana/Arty) + bitstream flow | ✅ chain simulated | `make sim-board` (bit-banged UART through the shipped config) |
| Real-data path (converter `--tick-scale`/`--after-hm`, excerpt, triple agreement) | ✅ | `tools/run_real_data.sh` |

Fixed RTL bugs (documented in DESIGN.md, regression-pinned): hash-delete
orphaning (→ tombstones + first-tomb reuse), best-tracker handshake race
(→ `expect_scan_q`/`scan_seen_q`), plus the `in_ready` protocol tightening on
the framer handshake.

### Known rough edges
- Single-symbol RTL vs multi-symbol software (real-data replay filters one
  symbol host-side).
- The FPGA band (1024 ticks, fixed after auto-centering) needs host-side
  tick rescaling for real symbols; production sizing = wider window (BRAM
  remap) + re-centering.
- MoldUDP64 gap handling is detection-only (no retransmit request path —
  needs a TX socket/session manager).
- 8-byte internal timestamps (real ITCH: 6; normalized by
  `gui/data/nasdaq_itch.py`, kept deliberately — the framer's 16-byte
  minimum-frame invariant depends on it).
- Kernel-bypass receivers unexercised against live traffic.
- Windows/MSYS2 PATH-ordering build trap (documented in BUILDING.md).
- The RTL "latency in ns" story is clock-honest now (100 MHz on Spartan-7),
  but the HW-beats-SW absolute-ns claim needs a faster part (Kintex/US+
  at 250-400 MHz); cycles + bounded tail are the portable result.

## Roadmap (by value)

1. ~~**FPGA reality pass**~~ **done 2026-07-13**: banded/de-structed memories,
   timing met at 100 MHz on xc7s50 (OOC, WNS +0.27 ns), board port with UART
   host link + bitstream flow for Urbana/Arty (`fpga/board/`). Remaining:
   flash on hardware + run the bring-up ladder (`fpga/board/README.md`).
2. ~~**Real data**~~ **done 2026-07-13**: MoldUDP64 decap in RTL + SW with
   gap/stale/heartbeat semantics (`rtl/mold_stripper.sv`,
   `sw/parser/mold_parser.hpp`, `tools/mold_wrap.cpp`); real NASDAQ sample
   day converted (timestamps normalized host-side) and verified by triple
   agreement (`tools/run_real_data.sh`). Retransmit *requests* remain open
   (see rough edges).
3. **Multi-symbol hardware**: book-bank per hot symbol or locate-indexed
   book cache; front end unchanged.
4. **Band re-centering**: shift the window when best drifts near an edge
   (copy/invalidate or circular-offset scheme) — removes the tick-rescale
   requirement for volatile symbols.
5. **SW tuning tier**: hugepages, prefetch on probe, batch parsing, and an
   AVX-512 machine to light up the 8-wide probe path.
6. **Tail attribution**: per-message cycle breakdown in hardware (already
   partially in perf counters) and a matching software flamegraph at p99.9.
7. **CI**: build + full test suite on push (software side is portable;
   Verilator job for RTL).
