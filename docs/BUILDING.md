# Building and running

## Requirements

- g++ with C++17 (tested with GCC 15, MSYS2 UCRT64) — the SIMD code compiles
  with `-march=native` and picks AVX-512, AVX2, or scalar paths automatically
- Verilator 5.x for the RTL track (tested with 5.040)
- GNU Make, bash
- Python 3 with matplotlib for the plots (optional — scripts fall back to CSV
  summaries without it)

## Run everything

```bash
bash run_all.sh            # build SW, run all SW tests, benchmark, RTL tests, analysis
SKIP_RTL=1 bash run_all.sh # software only
```

Individual pieces:

```bash
make test    # software test suite
make sim     # RTL Verilator tests
make bench   # std::map book vs direct-indexed+SIMD book
make data    # generate the synthetic ITCH file
make lint    # verilator --lint-only
```

## Test data

There's no NASDAQ sample file checked in (they're large and license-encumbered).
`make data` generates a byte-exact synthetic ITCH 5.0 stream instead — correct
length prefixes, big-endian fields, all message types, and a book that never
crosses by construction. The same file feeds the C++ and RTL pipelines, which is
what makes the end-to-end diff meaningful. `data/README.md` covers ingesting a
real NASDAQ file if you have one (note the timestamp-width difference described
there).

## Windows / MSYS2 notes

Same two traps as any Verilator project on MSYS2 (`run_all.sh` handles both):

1. `/c/msys64/ucrt64/bin` must be **first** on PATH, or `cc1plus` loads the
   wrong DLLs and g++ fails silently — exit 1, no error text, no output file.
2. If the Perl `verilator` wrapper dies with `Can't locate Pod/Usage.pm`, call
   the binary directly: `VERILATOR_ROOT=... verilator_bin.exe`.

One more quirk: MSYS2's ucrt64 Python has no pip. If matplotlib is missing,
`run_all.sh` probes for another interpreter that has it (e.g. a Windows-native
Python) and uses that for the plots.

## SIMD tiers

Every vectorized routine (`sw/util/simd.hpp` and its callers) has AVX-512, AVX2,
and scalar implementations selected at compile time via `-march=native`. All
three return identical results and the tests assert the vector paths against the
scalar one. If you're on a CPU without AVX-512, don't add `-mavx512*` flags by
hand — the binary will build and then SIGILL at runtime.
