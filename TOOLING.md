# Tooling

## Build and verification

- g++ / C++17 for the software side, with `-march=native` picking AVX-512,
  AVX2, or scalar SIMD paths automatically
- Verilator 5.x for the RTL track
- GNU Make and bash, one `run_all.sh` that builds everything, runs both test
  suites, benchmarks the two software books, and runs the analysis scripts
- Python with matplotlib for the latency and depth plots
- Streamlit for the interactive dashboard in `gui/`

Host-specific gotchas (MSYS2 PATH ordering, the broken Perl `verilator`
wrapper, matplotlib not being on the ucrt64 Python) are in
[docs/BUILDING.md](docs/BUILDING.md).

## How this got built

This project, and the docs describing it (README, DESIGN.md, this file),
were built with AI assistance and then reviewed and edited by me, not typed
out from nothing by hand. I wrote the spec, ran the build, and did the actual
debugging myself, especially on the hardware side, once the RTL was diffing
against the reference model on real traffic instead of toy cases.

Two real bugs are proof of that, not just a sentence claiming it happened.
The order-ref hash table originally deleted entries by clearing the slot,
which breaks linear probing since it orphans any colliding key stored after
it. A 24-deep forced-collision test caught it; tombstone deletion fixed it.
Separately, the best-price tracker had a one-cycle handshake race: the engine
checked `!scanning` to know a rescan had finished, but the tracker only
raises `scanning` one cycle after sampling the trigger, so the engine could
commit a stale, zero-share best price in that window. Fixed by tracking
whether a scan is expected and waiting for it to actually start and finish.
Both are written up properly in [docs/DESIGN.md](docs/DESIGN.md), and
they're honestly the most interesting part of the project to read.

If any of that raises questions, ask, I can go as deep as you want on either
one.
