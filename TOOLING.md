# Tooling

What actually went into building, verifying, and writing this up.

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

## AI-assisted coding

I used Claude Code as a coding tool throughout this project, the way a lot of
people now pair-program with an LLM. I wrote the spec, ran the build, and
worked through the hardware bugs myself once the RTL was diffing against the
reference model on real traffic.

Two real bugs came out of that process. The order-ref hash table originally
deleted entries by clearing the slot, which is wrong for linear probing since
it orphans any colliding key stored after it. A 24-deep forced-collision test
caught it, and I fixed it with tombstone deletion. The other was a one-cycle
handshake race in the best-price tracker: the engine checked `!scanning` to
know a rescan had finished, but the tracker only raises `scanning` one cycle
after sampling the trigger, so the engine could commit a stale, zero-share
best price in that window. Fixed by tracking whether a scan is expected and
waiting for it to actually start and finish. Full write-ups are in
[docs/DESIGN.md](docs/DESIGN.md), they're honestly the most interesting part
of the project.

Happy to walk through either of those, or anything else in the pipeline, in
more depth.
