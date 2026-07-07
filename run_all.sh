#!/usr/bin/env bash
# One-shot build + test entry point.
#
# Handles the two MSYS2 gotchas (docs/BUILDING.md): puts the ucrt64 bin dir
# first on PATH and points Verilator at its share dir / real binary. Builds the
# software stack, runs the software test suite, the book benchmark, the RTL
# Verilator tests, drives the software binary to emit CSVs, and runs the
# analysis scripts. Exits 0 only if every stage passes.
#
# Env knobs:
#   SKIP_RTL=1       skip the Verilator RTL tests (software-only iteration)
#   SKIP_ANALYSIS=1  skip the Python analysis stage
set -u

# Capture the pre-MSYS PATH: the matplotlib-capable Windows Python lives here,
# whereas the ucrt64 python we prepend below has no matplotlib.
ORIG_PATH="$PATH"
export PATH="/c/msys64/ucrt64/bin:$PATH"
export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator"

# Pick a python that can import matplotlib (for plots); fall back to any python
# (the analysis scripts then emit CSV summaries only).
pick_python() {
    local user_dir="/c/Users/${USERNAME:-${USER:-$(whoami 2>/dev/null)}}/AppData/Local/Microsoft/WindowsApps/python"
    for p in python python3 "$user_dir" \
             "${LOCALAPPDATA:-}/Microsoft/WindowsApps/python"; do
        if PATH="$ORIG_PATH" "$p" -c "import matplotlib" >/dev/null 2>&1; then
            echo "$p"; return; fi
    done
    echo python
}

cd "$(dirname "$0")" || exit 2
mkdir -p build build/csv

CXX=g++
CXXFLAGS="-std=c++17 -O3 -march=native -falign-functions=64 -falign-loops=64 -Wall -Wextra -pthread"
LIB="sw/parser/itch_parser.cpp sw/book/order_ref_table.cpp sw/book/order_book.cpp sw/stats/stats_engine.cpp"

fail=0
step() { echo; echo "==== $* ===="; }
ok()   { echo "  [OK]  $*"; }
bad()  { echo "  [FAIL] $*"; fail=1; }

# 1. generator + sample data ------------------------------------------------
step "build generator + sample data"
$CXX -std=c++17 -O2 -Wall -Wextra tools/gen_itch.cpp -o build/gen_itch.exe \
    && ./build/gen_itch.exe data/sample.itch 200000 4 >/dev/null \
    && ok "data/sample.itch" || bad "generator"

# 2. software test suite ----------------------------------------------------
step "software test suite"
TESTS="sw/tests/test_main.cpp sw/tests/test_util.cpp sw/tests/test_order_ref_table.cpp \
       sw/tests/test_itch_parser.cpp sw/tests/test_book_engine.cpp sw/tests/test_stats.cpp \
       sw/tests/test_full_pipeline.cpp"
if $CXX $CXXFLAGS $TESTS $LIB -o build/run_tests.exe; then
    if ./build/run_tests.exe; then ok "software tests"; else bad "software tests"; fi
else bad "software tests (compile)"; fi

# 3. software order book binary + CSVs --------------------------------------
step "software order book binary"
if $CXX $CXXFLAGS sw/main.cpp $LIB sw/receiver/afxdp_receiver.cpp -o build/orderbook_sw.exe; then
    ./build/orderbook_sw.exe data/sample.itch --symbol 1 --csv build/csv --quiet \
        && ok "orderbook_sw + CSVs" || bad "orderbook_sw run"
else bad "orderbook_sw (compile)"; fi

# 4. book benchmark -----------------------------------------------------------
step "book benchmark (std::map vs SIMD)"
if $CXX $CXXFLAGS tools/bench.cpp $LIB -o build/bench.exe; then
    ./build/bench.exe 500000 | tee build/csv/benchmark.txt && ok "benchmark" || bad "benchmark"
else bad "benchmark (compile)"; fi

# 5. RTL Verilator tests -----------------------------------------------------
if [ "${SKIP_RTL:-0}" = "1" ]; then
    step "RTL Verilator tests (SKIPPED)"
elif [ -f sim/run_rtl_tests.sh ]; then
    step "RTL Verilator tests"
    if bash sim/run_rtl_tests.sh; then ok "RTL tests"; else bad "RTL tests"; fi
else
    step "RTL Verilator tests"
    bad "sim/run_rtl_tests.sh not found"
fi

# 6. analysis (best effort; missing matplotlib is not a hard failure) --------
if [ "${SKIP_ANALYSIS:-0}" != "1" ]; then
    step "analysis (plots/CSV summaries)"
    PY=$(pick_python)
    echo "  using python: $PY"
    for s in latency_compare message_profile book_depth; do
        if [ -f "analysis/$s.py" ]; then
            PATH="$ORIG_PATH" "$PY" "analysis/$s.py" --csv build/csv >/dev/null 2>&1 \
                && ok "analysis/$s.py" || echo "  [skip] analysis/$s.py"
        fi
    done
fi

echo
if [ "$fail" = "0" ]; then
    echo "======== ALL STAGES PASSED ========"
else
    echo "======== FAILURES PRESENT ========"
fi
exit $fail
