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

I wrote the RTL and the software side with AI assistance, then debugged it
myself and read through it to modify things, especially once the hardware
started diverging from the C++ reference model on real traffic and needed
actual fixes, not just re-generation.

Ask if you want to go deeper on any part of the pipeline.
