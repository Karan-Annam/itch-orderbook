#!/usr/bin/env bash
# Verilate orderbook_top once, then build & run every RTL test.
#
# MSYS2 quirks handled here (docs/BUILDING.md): the Perl verilator wrapper is
# broken so we call verilator_bin.exe directly with VERILATOR_ROOT set, and
# /c/msys64/ucrt64/bin goes first on PATH. The model is Verilated once into a
# library (obj_rtl); each test in sim/tests/ plus the end-to-end tb_top is
# compiled and linked against it. Exits 0 only if every test passes.
set -u

export PATH="/c/msys64/ucrt64/bin:$PATH"
export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 2

VBIN=verilator_bin.exe
VINC="$VERILATOR_ROOT/include"
OBJ="$ROOT/sim/obj_rtl"
CXX=g++
CXXFLAGS="-std=c++17 -O2 -I$VINC -I$VINC/vltstd -I$ROOT/sim -I$OBJ -DVM_TRACE=0"

RTL="rtl/ob_pkg.sv rtl/udp_stripper.sv rtl/mold_stripper.sv rtl/itch_framer.sv \
     rtl/itch_decoder.sv rtl/order_ref_table.sv rtl/book_update_engine.sv \
     rtl/best_tracker.sv rtl/stats_engine.sv rtl/perf_counters.sv rtl/orderbook_top.sv"

echo "==== ensuring sample data ===="
if [ ! -f sim/rtl_sim.itch ]; then
    if [ -x build/gen_itch.exe ]; then
        build/gen_itch.exe sim/rtl_sim.itch 20000 1 >/dev/null
    else
        $CXX -std=c++17 -O2 tools/gen_itch.cpp -o build/gen_itch.exe && \
        build/gen_itch.exe sim/rtl_sim.itch 20000 1 >/dev/null
    fi
fi
echo "  sim/rtl_sim.itch ready ($(wc -c < sim/rtl_sim.itch) bytes)"

LOG="$ROOT/sim/rtl_build.log"
echo "==== Verilating orderbook_top (library) ===="
rm -rf "$OBJ"
$VBIN --cc -j 0 \
    -Wall -Wno-UNUSED -Wno-UNDRIVEN -Wno-DECLFILENAME -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    -Irtl --top-module orderbook_top --Mdir "$OBJ" $RTL > "$LOG" 2>&1
if [ $? -ne 0 ]; then echo "  [FAIL] Verilation"; tail -30 "$LOG"; exit 1; fi

echo "==== building Verilated library ===="
make -C "$OBJ" -f Vorderbook_top.mk Vorderbook_top__ALL.a >> "$LOG" 2>&1
# also compile the verilated runtime support objects
$CXX $CXXFLAGS -c "$VINC/verilated.cpp"         -o "$OBJ/verilated.o"         2>>"$LOG"
$CXX $CXXFLAGS -c "$VINC/verilated_threads.cpp" -o "$OBJ/verilated_threads.o" 2>>"$LOG"
if [ ! -f "$OBJ/Vorderbook_top__ALL.a" ]; then
    echo "  [FAIL] library build"; tail -30 "$LOG"; exit 1; fi
echo "  library built"

# Shared support objects (file reader, reference model, sc_time_stamp).
$CXX $CXXFLAGS -c sim/itch_file_reader.cpp -o "$OBJ/itch_file_reader.o"
$CXX $CXXFLAGS -c sim/reference_model.cpp  -o "$OBJ/reference_model.o"
$CXX $CXXFLAGS -c sim/sim_support.cpp      -o "$OBJ/sim_support.o"
SUPPORT="$OBJ/itch_file_reader.o $OBJ/reference_model.o $OBJ/sim_support.o \
         $OBJ/Vorderbook_top__ALL.a $OBJ/verilated.o $OBJ/verilated_threads.o"
LIBS="-pthread -latomic"

build_run() {  # <name> <src> [args...]
    local name="$1"; local src="$2"; shift 2
    local exe="$OBJ/$name.exe"
    if ! $CXX $CXXFLAGS "$src" $SUPPORT $LIBS -o "$exe" 2> "$OBJ/$name.build.log"; then
        echo "  [FAIL] compile $name"; tail -20 "$OBJ/$name.build.log"; return 1; fi
    if "$exe" "$@" > "$OBJ/$name.run.log" 2>&1; then
        echo "  [PASS] $name  ($(grep -hoE '[0-9]+ checks' "$OBJ/$name.run.log" | head -1))"
        return 0
    else
        echo "  [FAIL] $name"; tail -25 "$OBJ/$name.run.log"; return 1
    fi
}

# Second model: the SYNTHESIS config (banded 1024-level book, small table,
# dbg tied off) — the exact configuration Vivado sees. test_banding runs
# against this one; everything else uses the full-size sim config above.
OBJB="$ROOT/sim/obj_rtl_band"
echo "==== Verilating orderbook_top (SYNTHESIS/banded config) ===="
rm -rf "$OBJB"
$VBIN --cc -j 0 \
    -Wall -Wno-UNUSED -Wno-UNDRIVEN -Wno-DECLFILENAME -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    +define+SYNTHESIS -Irtl --top-module orderbook_top --Mdir "$OBJB" $RTL >> "$LOG" 2>&1
if [ $? -ne 0 ]; then echo "  [FAIL] Verilation (banded)"; tail -30 "$LOG"; exit 1; fi
make -C "$OBJB" -f Vorderbook_top.mk Vorderbook_top__ALL.a >> "$LOG" 2>&1
if [ ! -f "$OBJB/Vorderbook_top__ALL.a" ]; then
    echo "  [FAIL] banded library build"; tail -30 "$LOG"; exit 1; fi
CXXFLAGS_B="-std=c++17 -O2 -I$VINC -I$VINC/vltstd -I$ROOT/sim -I$OBJB -DVM_TRACE=0"
SUPPORT_B="$OBJ/itch_file_reader.o $OBJ/reference_model.o $OBJ/sim_support.o \
           $OBJB/Vorderbook_top__ALL.a $OBJ/verilated.o $OBJ/verilated_threads.o"
echo "  banded library built"

build_run_band() {  # <name> <src> [args...]
    local name="$1"; local src="$2"; shift 2
    local exe="$OBJB/$name.exe"
    if ! $CXX $CXXFLAGS_B "$src" $SUPPORT_B $LIBS -o "$exe" 2> "$OBJB/$name.build.log"; then
        echo "  [FAIL] compile $name"; tail -20 "$OBJB/$name.build.log"; return 1; fi
    if "$exe" "$@" > "$OBJB/$name.run.log" 2>&1; then
        echo "  [PASS] $name  ($(grep -hoE '[0-9]+ checks' "$OBJB/$name.run.log" | head -1))"
        return 0
    else
        echo "  [FAIL] $name"; tail -25 "$OBJB/$name.run.log"; return 1
    fi
}

echo "==== running RTL tests ===="
fail=0
build_run_band test_banding   sim/tests/test_banding.cpp                       || fail=1
build_run test_itch_decode     sim/tests/test_itch_decode.cpp                 || fail=1
build_run test_udp_strip       sim/tests/test_udp_strip.cpp                   || fail=1
build_run test_mold            sim/tests/test_mold.cpp                        || fail=1
build_run test_framer_wide     sim/tests/test_framer_wide.cpp                 || fail=1
build_run test_order_ref_table sim/tests/test_order_ref_table.cpp             || fail=1
build_run test_book_update     sim/tests/test_book_update.cpp                 || fail=1
build_run test_replace         sim/tests/test_replace.cpp                     || fail=1
build_run test_overlap         sim/tests/test_overlap.cpp                     || fail=1
build_run test_full_pipeline   sim/tests/test_full_pipeline.cpp sim/rtl_sim.itch 5000 || fail=1
build_run tb_top               sim/tb_top.cpp                  sim/rtl_sim.itch       || fail=1

echo "==== building tb_latency (perf tool, not a correctness test) ===="
exe="$OBJ/tb_latency.exe"
if $CXX $CXXFLAGS sim/tb_latency.cpp $SUPPORT $LIBS -o "$exe" 2>"$OBJ/tb_latency.build.log"; then
    echo "  [OK] tb_latency built at $exe"
else
    echo "  [WARN] tb_latency build failed (non-fatal for test suite)"
    cat "$OBJ/tb_latency.build.log"
fi

echo
if [ "$fail" = "0" ]; then echo "==== ALL RTL TESTS PASSED ===="; else echo "==== RTL TEST FAILURES ===="; fi
exit $fail
