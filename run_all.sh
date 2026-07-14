#!/usr/bin/env bash
# Rebuild data, run all software/RTL/board tests, collect measurements, and
# regenerate the checked-in result summary. Run from MSYS2 or Git Bash.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ORIG_PATH="$PATH"
if [ -d /c/msys64/ucrt64/bin ]; then
    export PATH="/c/msys64/ucrt64/bin:/usr/bin:$PATH"
    export VERILATOR_ROOT="${VERILATOR_ROOT:-C:/msys64/ucrt64/share/verilator}"
fi
export TMPDIR="${TMPDIR:-$ROOT/build/tmp}"
export TEMP="$TMPDIR"
export TMP="$TMPDIR"
export MPLCONFIGDIR="${MPLCONFIGDIR:-$ROOT/build/mplconfig}"
mkdir -p build/csv "$TMPDIR"

pick_python() {
    local candidates=(
        "/c/Users/${USERNAME:-${USER:-karan}}/anaconda3/python.exe"
        "/c/Users/${USERNAME:-${USER:-karan}}/AppData/Local/Programs/Python/Python313/python.exe"
        python python3
    )
    local p
    for p in "${candidates[@]}"; do
        if PATH="$ORIG_PATH" "$p" -c "import matplotlib" >/dev/null 2>&1; then
            printf '%s\n' "$p"
            return
        fi
    done
    printf '%s\n' python
}

echo "==== software build and tests ===="
make data test sw bench

echo "==== software measurements ===="
rm -f build/csv/*.csv build/csv/*.png build/csv/benchmark.txt
MEASURE_CORE="${MEASURE_CORE:-0}"
./build/orderbook_sw data/sample.itch --engine simd --symbol 1 \
    --pin "$MEASURE_CORE" --csv build/csv --quiet
./build/orderbook_sw data/sample.itch --engine map --symbol 1 \
    --pin "$MEASURE_CORE" --csv build/csv --quiet
./build/bench 500000 "$MEASURE_CORE" | tee build/csv/benchmark.txt

echo "==== RTL lint and tests ===="
make lint sim
./sim/obj_rtl/tb_latency.exe data/sample.itch --max 20000 --csv build/csv --mhz 100 --quiet

echo "==== board-chain simulation ===="
make sim-board

echo "==== analysis and result summary ===="
PY="$(pick_python)"
PATH="$ORIG_PATH" "$PY" analysis/latency_compare.py --csv build/csv
PATH="$ORIG_PATH" "$PY" analysis/message_profile.py --csv build/csv
PATH="$ORIG_PATH" "$PY" analysis/book_depth.py --csv build/csv
PATH="$ORIG_PATH" "$PY" tools/write_results.py --csv build/csv --output docs/results.json

mkdir -p docs/img
for image in latency_compare message_profile book_depth; do
    if [ -f "build/csv/$image.png" ]; then cp "build/csv/$image.png" docs/img/; fi
done

echo "==== ALL CHECKS PASSED ===="
