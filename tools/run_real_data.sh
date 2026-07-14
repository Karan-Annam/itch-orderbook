#!/usr/bin/env bash
# Triple-agreement check on real converted NASDAQ data:
#   (a) software book  (build/orderbook_sw --final-state)
#   (b) full-size RTL model vs reference model, every commit (sim/tb_top)
#   (c) banded SYNTHESIS-config RTL model (sim/tb_replay on obj_rtl_band),
#       asserting band_drops == 0 and final state equal to (a)
# Pass = the same file produces the same book everywhere, including in the
# exact configuration the bitstream ships.
#
# Prereqs: `make sw` and `make sim` (builds obj_rtl / obj_rtl_band).
# Usage:   bash tools/run_real_data.sh [file.itch]  (default data/real_sample_scaled.itch)
set -u
export PATH="/c/msys64/ucrt64/bin:$PATH"
export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 2

FILE="${1:-data/real_sample_scaled.itch}"
[ -f "$FILE" ] || { echo "missing $FILE — see data/README.md for the conversion recipe"; exit 2; }
[ -x build/orderbook_sw ]              || { echo "run 'make sw' first"; exit 2; }
[ -f sim/obj_rtl/Vorderbook_top__ALL.a ]      || { echo "run 'make sim' first"; exit 2; }
[ -f sim/obj_rtl_band/Vorderbook_top__ALL.a ] || { echo "run 'make sim' first"; exit 2; }

VINC="$VERILATOR_ROOT/include"
CXX=g++
fail=0

# sidecar carries the symbol's locate code and the band base the hardware
# must be configured with (tools/band_filter.py)
META="$FILE.meta.json"
LOC=$(python -c "import json;print((json.load(open('$META')).get('locates') or [1])[0])" 2>/dev/null || echo 1)
BAND=$(python -c "import json;print(json.load(open('$META')).get('band_base',-1))" 2>/dev/null || echo -1)
echo "meta: locate=$LOC band_base=$BAND"

echo "==== (a) software book ===="
SW_FINAL=$(build/orderbook_sw "$FILE" --quiet --final-state --symbol "$LOC" | grep '^FINAL') || fail=1
echo "  $SW_FINAL"

echo "==== (b) full-size RTL vs reference model (tb_top) ===="
if sim/obj_rtl/tb_top.exe "$FILE" > /tmp/real_tbtop.log 2>&1; then
    echo "  [PASS] $(grep -hoE '[0-9]+ checks' /tmp/real_tbtop.log | head -1)"
else
    echo "  [FAIL] tb_top"; tail -10 /tmp/real_tbtop.log; fail=1
fi

echo "==== (c) banded SYNTHESIS-config RTL ===="
OBJB="sim/obj_rtl_band"
if [ ! -x "$OBJB/tb_replay.exe" ] || [ sim/tb_replay.cpp -nt "$OBJB/tb_replay.exe" ]; then
    $CXX -std=c++17 -O2 -I"$VINC" -I"$VINC/vltstd" -Isim -I"$OBJB" -DVM_TRACE=0 \
        sim/tb_replay.cpp sim/itch_file_reader.cpp sim/sim_support.cpp \
        "$OBJB/Vorderbook_top__ALL.a" sim/obj_rtl/verilated.o sim/obj_rtl/verilated_threads.o \
        -pthread -latomic -o "$OBJB/tb_replay.exe" || { echo "  [FAIL] build tb_replay"; exit 1; }
fi
OUT=$("$OBJB/tb_replay.exe" "$FILE" 0 "$BAND") || { echo "  [FAIL] banded replay"; echo "$OUT"; exit 1; }
BAND_FINAL=$(echo "$OUT" | grep '^FINAL')
BAND_INFO=$(echo "$OUT" | grep '^BAND')
echo "  $BAND_FINAL"
echo "  $BAND_INFO"

drops=$(echo "$BAND_INFO" | sed 's/.*drops=\([0-9]*\).*/\1/')
insf=$(echo "$BAND_INFO" | sed 's/.*ins_fails=//')
if [ "$drops" != "0" ]; then
    echo "  [FAIL] band dropped $drops events — window/base mismatch"
    echo "         (re-run tools/band_filter.py, check the sidecar band_base)"
    fail=1
fi
if [ "$insf" != "0" ]; then
    echo "  [FAIL] $insf table inserts failed — TABLE_SIZE undersized for this churn"
    fail=1
fi
if [ "$SW_FINAL" = "$BAND_FINAL" ]; then
    echo "  [PASS] banded RTL final state == software final state"
else
    echo "  [FAIL] final-state mismatch:"
    echo "    sw:     $SW_FINAL"
    echo "    banded: $BAND_FINAL"
    fail=1
fi

echo
if [ "$fail" = "0" ]; then echo "==== REAL-DATA TRIPLE AGREEMENT: PASS ===="
else echo "==== REAL-DATA CHECK FAILED ===="; fi
exit $fail
