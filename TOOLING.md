# Tooling

## Build and verification

- g++ / C++17 for the software side (`-march=native` picks AVX-512, AVX2, or
  scalar automatically)
- Verilator 5.x for the RTL track
- GNU Make and bash, one `run_all.sh` for everything: both test suites, the
  bench, and the analysis scripts
- Python; matplotlib is optional for plots
- Streamlit for the dashboard in `gui/`

Host quirks (MSYS2 PATH ordering, the broken Perl `verilator` wrapper,
matplotlib missing from the ucrt64 Python) are in
[docs/BUILDING.md](docs/BUILDING.md).

## AI use

AI-assisted tools were used for implementation support, debugging, and
documentation. I reviewed and modified the resulting RTL and C++ code and
validated them with software tests, RTL simulation, and differential checking
against the reference model.
