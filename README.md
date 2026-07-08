# ITCH order book, same feed, C++ and RTL

A NASDAQ ITCH 5.0 limit order book built twice: once as SIMD-tuned C++ chasing
sub-microsecond software latency, and once as synthesizable SystemVerilog doing
deterministic nanoseconds. Both consume the same byte stream and must produce
bit-identical book state, the RTL is diffed against a reference model on every
message. The point is to measure the software/hardware latency gap instead of
just quoting it.

## Contents

- [Quick start](#quick-start)
- [Results](#results)
- [What's inside](#whats-inside)
- [GUI](#gui)
- [Layout](#layout)
- [Limitations](#limitations)
- [Tooling](#tooling)

## Quick start

```bash
bash run_all.sh            # build + all tests + benchmark + analysis
SKIP_RTL=1 bash run_all.sh # software only
make test                  # software suite
make sim                   # RTL Verilator tests
make bench                 # std::map book vs direct-indexed+SIMD book
```

No market data download needed. `make data` generates a byte-exact synthetic
ITCH stream that feeds both pipelines. Windows/MSYS2 users: read
[docs/BUILDING.md](docs/BUILDING.md) first (PATH-ordering trap).

## Results

Software, per message (parse + book update, RDTSC, AVX2 on this machine):

| | p50 | p99 | p99.9 |
|---|---|---|---|
| all messages | ~190 ns | ~540 ns | ~1.3 µs |
| Trade `P` (no book op) | ~35 ns | ~90 ns | |
| Replace `U` (worst type) | ~260 ns | ~650 ns | |

Hardware: ~344 ns for an Add at the modeled 250 MHz, every single time. Notice
software's p50 (~190 ns) actually *beats* that. That's not a knock on the
hardware, it's a fair result with a real explanation: the CPU is clocked
16-20× higher (4-5 GHz vs 250 MHz) and is very good at exactly this kind of
hot, branch-predictable, cache-resident integer work, while the RTL streams
the message in one byte per cycle through the front end, so a 38-byte Add
spends roughly half its ~86 cycles just shifting bytes in before book logic
even starts. 250 MHz is also a conservative, unsynthesized clock target, not
a placed-and-routed ceiling; a real FPGA build with a wider ingest path would
close both gaps. So the honest comparison isn't "hardware wins on speed", it's
that software's *best* case beats hardware's *only* case, but software's
worst case is ~7× its own median (cache misses, OS jitter) while hardware's
worst case equals its best case, every time, including during the bursts and
volatility spikes when everyone else's software also slows down. That
invariance, not a lower mean, is what an FPGA actually buys here.

![latency comparison](docs/img/latency_compare.png)

The direct-indexed + SIMD book is about **2×** faster than the `std::map`
version on the same stream (~90–110 vs ~200–235 ns/msg book-only at 1M
messages; run-to-run it lands anywhere from 1.5× to 2.6×. `make bench`).

The synthetic feed's reference mid-price doesn't drift, so best bid/ask stay
pinned near $49.99/$50.01 for the whole session; what actually moves is
resting size at the top of book, which grows steadily as the generator adds
far more liquidity than it consumes:

![book depth](docs/img/book_depth.png)

## What's inside

**Software** (`sw/`): file-replay / raw-socket / AF_XDP receivers (the kernel
ones Linux-guarded), a lock-free SPSC ring, an ITCH parser with a SIMD header
scan, an open-addressing order-ref table (structure-of-arrays, Wang hash,
SIMD probe, backward-shift deletion), a direct price-indexed book with a
vectorized best-price rescan, and a stats engine (VWAP, spread, top-5 imbalance,
depth). Every SIMD routine has AVX-512 / AVX2 / scalar paths chosen at compile
time; the scalar path is the test oracle.

**Hardware** (`rtl/`): `udp_stripper → itch_framer → itch_decoder →
order_ref_table → book_update_engine (+ best_tracker) → stats_engine`, one byte
per clock, SRAM-backed. The message layouts in `ob_pkg.sv` mirror
`itch_messages.hpp` exactly, so both sides decode identical bytes to identical
fields.

**Verification**: the software suite is ~140k assertions including a 200k-op
differential test of the hash table against `std::unordered_map` and a
bit-for-bit diff of the fast book against a `std::map` reference. The RTL suite
replays 20k+ messages through the Verilated pipeline and diffs full book state
against the reference model.

Building the RTL against that diff surfaced two genuine hardware bugs: a
linear-probe delete that orphaned collision chains (fixed with tombstones) and a
one-cycle handshake race in the best-price tracker that could commit a stale
best. Both write-ups are in [docs/DESIGN.md](docs/DESIGN.md); they're the most
instructive part of the project.

## GUI

There's also a small Streamlit dashboard for poking at it interactively:

```bash
pip install -r gui/requirements.txt
streamlit run gui/app.py
```

Three tabs: pick/generate a data source, replay it and watch the book (depth
ladder, spread), then run the same feed through all three engines (`std::map` /
SIMD / RTL) and compare latency distributions. It shells out to the same
binaries the command line uses, so the numbers match `run_all.sh`.

## Layout

```
sw/        receiver/ parser/ book/ stats/ util/ tests/ + main.cpp
rtl/       *.sv, the streaming pipeline
sim/       Verilator harness, reference model, RTL tests
tools/     gen_itch.cpp (synthetic feed), bench.cpp
gui/       Streamlit dashboard
analysis/  latency/profile/depth plots
docs/      DESIGN.md, BUILDING.md, images
```

## Limitations

Single-symbol RTL (software is multi-symbol); async-read behavioral SRAMs
(re-map to registered BRAM for a real FPGA build); the hardware latency is a
cycle count at a modeled 250 MHz, not post-P&R timing; 8-byte timestamps per the
internal format (real ITCH uses 6, see `data/README.md`).

## Tooling

Build toolchain, AI-assisted coding, the two real bugs it surfaced, and how
the docs themselves were written are covered in [TOOLING.md](TOOLING.md).
