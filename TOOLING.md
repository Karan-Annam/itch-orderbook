# Tooling

## Build and verification

- g++ / C++17 for the software side (`-march=native` picks AVX-512, AVX2, or
  scalar automatically)
- Verilator 5.x for the RTL track
- GNU Make and bash, one `run_all.sh` for everything: both test suites, the
  bench, and the analysis scripts
- Python with matplotlib for the plots
- Streamlit for the dashboard in `gui/`

Host quirks (MSYS2 PATH ordering, the broken Perl `verilator` wrapper,
matplotlib missing from the ucrt64 Python) are in
[docs/BUILDING.md](docs/BUILDING.md).

## AI use

Built with Claude Code from a spec I wrote, with me doing the hardware
debugging directly once the RTL was diffing against the reference model on
real traffic.

Two bugs came out of that debugging: a linear-probe delete that orphaned
collision chains (fixed with tombstones), and a one-cycle handshake race in
the best-price tracker that could commit a stale best price. Both are
written up in [docs/DESIGN.md](docs/DESIGN.md) if you want the detail.

Ask if you want to go deeper on either one, or anything else in the
pipeline.
