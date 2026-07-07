#!/usr/bin/env bash
# build_linux.sh — build NATIVE Linux (ELF) binaries for the pipeline.
#
# Needed under WSL, where the Windows .exe builds can't be exec'd. Produces:
#   build/orderbook_sw            (SW order book; --engine simd|map)
#   build/gen_itch                (synthetic ITCH generator)
#   sim/obj_rtl_linux/tb_latency  (Verilated RTL latency harness)
#
# Requirements (Ubuntu/Debian):
#   sudo apt update
#   sudo apt install build-essential verilator
#
# Usage:  bash gui/build_linux.sh [sw|rtl|all]   (default: all)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 2

WHAT="${1:-all}"
CXX="${CXX:-g++}"
CXXSTD="-std=c++17"
OPT="-O3 -march=native -falign-functions=64 -falign-loops=64"
WARN="-Wall -Wextra"

die() { echo "[build_linux] ERROR: $*" >&2; exit 1; }

command -v "$CXX" >/dev/null 2>&1 || die "no C++ compiler ($CXX).
  Fix: sudo apt update && sudo apt install build-essential"

# ---- software ---------------------------------------------------------------
build_sw() {
    echo "==== [linux] building orderbook_sw + gen_itch ===="
    mkdir -p build
    local LIB="sw/parser/itch_parser.cpp sw/book/order_ref_table.cpp \
               sw/book/order_book.cpp sw/stats/stats_engine.cpp"
    # afxdp_receiver falls back to a portable stub unless OB_HAVE_XDP is set,
    # so it compiles without libxdp.
    echo "  orderbook_sw ..."
    $CXX $CXXSTD $OPT $WARN -pthread \
        sw/main.cpp $LIB sw/receiver/afxdp_receiver.cpp \
        -o build/orderbook_sw || die "orderbook_sw build failed"
    echo "  gen_itch ..."
    $CXX $CXXSTD -O2 $WARN tools/gen_itch.cpp -o build/gen_itch \
        || die "gen_itch build failed"
    echo "  [OK] build/orderbook_sw  build/gen_itch"
}

# ---- RTL (Verilator) --------------------------------------------------------
build_rtl() {
    echo "==== [linux] Verilating orderbook_top + building tb_latency ===="
    command -v verilator >/dev/null 2>&1 || die "verilator not found.
  Fix: sudo apt update && sudo apt install verilator"

    local VROOT VINC OBJ
    VROOT="$(verilator --getenv VERILATOR_ROOT 2>/dev/null)"
    [ -n "$VROOT" ] || VROOT=/usr/share/verilator
    VINC="$VROOT/include"
    [ -d "$VINC" ] || die "Verilator include dir not found at $VINC"
    OBJ="$ROOT/sim/obj_rtl_linux"
    local CXXFLAGS="$CXXSTD -O2 -I$VINC -I$VINC/vltstd -I$ROOT/sim -I$OBJ -DVM_TRACE=0"

    local RTL="rtl/ob_pkg.sv rtl/udp_stripper.sv rtl/itch_framer.sv rtl/itch_decoder.sv \
               rtl/order_ref_table.sv rtl/book_update_engine.sv rtl/best_tracker.sv \
               rtl/stats_engine.sv rtl/perf_counters.sv rtl/orderbook_top.sv"

    local LOG="$ROOT/sim/rtl_build_linux.log"
    rm -rf "$OBJ"
    echo "  verilating (log: $LOG) ..."
    verilator --cc -j 0 \
        -Wall -Wno-UNUSED -Wno-UNDRIVEN -Wno-DECLFILENAME -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
        -Irtl --top-module orderbook_top --Mdir "$OBJ" $RTL > "$LOG" 2>&1 \
        || { tail -30 "$LOG"; die "Verilation failed"; }

    echo "  building Verilated library ..."
    make -C "$OBJ" -f Vorderbook_top.mk Vorderbook_top__ALL.a >> "$LOG" 2>&1
    $CXX $CXXFLAGS -c "$VINC/verilated.cpp"         -o "$OBJ/verilated.o"         2>>"$LOG"
    $CXX $CXXFLAGS -c "$VINC/verilated_threads.cpp" -o "$OBJ/verilated_threads.o" 2>>"$LOG"
    [ -f "$OBJ/Vorderbook_top__ALL.a" ] || { tail -30 "$LOG"; die "library build failed"; }

    $CXX $CXXFLAGS -c sim/itch_file_reader.cpp -o "$OBJ/itch_file_reader.o" || die "itch_file_reader"
    $CXX $CXXFLAGS -c sim/reference_model.cpp  -o "$OBJ/reference_model.o"  || die "reference_model"
    $CXX $CXXFLAGS -c sim/sim_support.cpp      -o "$OBJ/sim_support.o"      || die "sim_support"
    local SUPPORT="$OBJ/itch_file_reader.o $OBJ/reference_model.o $OBJ/sim_support.o \
                   $OBJ/Vorderbook_top__ALL.a $OBJ/verilated.o $OBJ/verilated_threads.o"

    echo "  building tb_latency ..."
    $CXX $CXXFLAGS sim/tb_latency.cpp $SUPPORT -pthread -latomic \
        -o "$OBJ/tb_latency" || die "tb_latency build failed"
    echo "  [OK] $OBJ/tb_latency"
}

case "$WHAT" in
    sw)  build_sw ;;
    rtl) build_rtl ;;
    all) build_sw; build_rtl ;;
    *)   die "unknown target '$WHAT' (use sw|rtl|all)" ;;
esac
echo "==== [linux] build complete ===="
