#!/usr/bin/env bash
# Verilate the BOARD chain — fpga_top (UART bridge + banded orderbook_top,
# +define+SYNTHESIS so it is the exact configuration the bitstream ships) —
# and run fpga/board/test_board.cpp against it: bit-banged UART in, snapshot
# frames out. Same MSYS2 quirks as run_rtl_tests.sh (verilator_bin.exe direct,
# ucrt64 first on PATH). UART divisor is overridden to 16 so a frame takes
# ~8k cycles instead of ~450k.
set -u

if [ -d /c/msys64/ucrt64/bin ]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 2

if command -v verilator_bin.exe >/dev/null 2>&1; then
    VBIN=verilator_bin.exe
else
    VBIN=verilator
fi
if [ -z "${VERILATOR_ROOT:-}" ]; then
    export VERILATOR_ROOT="$($VBIN -V | awk '/^VERILATOR_ROOT/{print $3; exit}')"
fi
VINC="$VERILATOR_ROOT/include"
OBJ="$ROOT/sim/obj_board"
CXX=g++
CXXFLAGS="-std=c++17 -O2 -I$VINC -I$VINC/vltstd -I$ROOT/sim -I$OBJ -DVM_TRACE=0"

RTL="rtl/ob_pkg.sv rtl/udp_stripper.sv rtl/mold_stripper.sv rtl/itch_framer.sv \
     rtl/itch_decoder.sv rtl/order_ref_table.sv rtl/book_update_engine.sv \
     rtl/best_tracker.sv rtl/stats_engine.sv rtl/perf_counters.sv \
     rtl/orderbook_top.sv fpga/board/uart_itch_bridge.sv fpga/board/fpga_top.sv"

LOG="$ROOT/sim/board_build.log"
echo "==== Verilating fpga_top (board chain, SYNTHESIS config) ===="
rm -rf "$OBJ"
$VBIN --cc -j 0 \
    -Wall -Wno-UNUSED -Wno-UNDRIVEN -Wno-DECLFILENAME -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    -Wno-PINCONNECTEMPTY \
    +define+SYNTHESIS -GUART_DIVISOR=16 \
    -Irtl --top-module fpga_top --Mdir "$OBJ" $RTL > "$LOG" 2>&1
if [ $? -ne 0 ]; then echo "  [FAIL] Verilation"; tail -30 "$LOG"; exit 1; fi

make -C "$OBJ" -f Vfpga_top.mk Vfpga_top__ALL.a >> "$LOG" 2>&1
$CXX $CXXFLAGS -c "$VINC/verilated.cpp"         -o "$OBJ/verilated.o"         2>>"$LOG"
$CXX $CXXFLAGS -c "$VINC/verilated_threads.cpp" -o "$OBJ/verilated_threads.o" 2>>"$LOG"
if [ ! -f "$OBJ/Vfpga_top__ALL.a" ]; then
    echo "  [FAIL] library build"; tail -30 "$LOG"; exit 1; fi
echo "  library built"

echo "==== running board-chain test ===="
if ! $CXX $CXXFLAGS fpga/board/test_board.cpp \
        "$OBJ/Vfpga_top__ALL.a" "$OBJ/verilated.o" "$OBJ/verilated_threads.o" \
        -pthread -latomic -o "$OBJ/test_board.exe" 2> "$OBJ/test_board.build.log"; then
    echo "  [FAIL] compile test_board"; tail -20 "$OBJ/test_board.build.log"; exit 1
fi
if "$OBJ/test_board.exe" > "$OBJ/test_board.run.log" 2>&1; then
    echo "  [PASS] test_board  ($(grep -hoE '[0-9]+ checks' "$OBJ/test_board.run.log" | head -1))"
    exit 0
else
    echo "  [FAIL] test_board"; tail -25 "$OBJ/test_board.run.log"; exit 1
fi
