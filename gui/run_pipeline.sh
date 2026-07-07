#!/usr/bin/env bash
# run_pipeline.sh — run all three engines on an ITCH file, write CSVs.
#
# Usage:
#   bash gui/run_pipeline.sh <itch_file> [run_dir] [--max N]
#
# Under WSL we use NATIVE Linux (ELF) binaries — the Windows .exe builds can't be
# exec'd there — building them via gui/build_linux.sh on first run. Under MSYS2/
# Git Bash we use the Windows .exe builds. Either way, all paths stay in native
# (Linux) form: native ELF and MSYS2 programs both accept /mnt/c or /c paths, so
# no path translation is needed.
#
# Exits 0 iff all three engines complete.
set -euo pipefail

# ---- environment ------------------------------------------------------------
IS_WSL=0
uname -r 2>/dev/null | grep -qi microsoft && IS_WSL=1

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

if [[ "$IS_WSL" == "1" ]]; then
    SW_BIN="$ROOT/build/orderbook_sw"
    TB_LAT="$ROOT/sim/obj_rtl_linux/tb_latency"
    ENV_NAME="WSL (native Linux)"
else
    export PATH="/c/msys64/ucrt64/bin:$PATH"
    export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator"
    SW_BIN="$ROOT/build/orderbook_sw.exe"
    TB_LAT="$ROOT/sim/obj_rtl/tb_latency.exe"
    ENV_NAME="MSYS2 (Windows .exe)"
fi

# ---- args -------------------------------------------------------------------
ITCH_FILE="${1:-}"
RUN_DIR="${2:-}"
MAX_MSGS=0
shift_next=0
for arg in "$@"; do
    if [[ "$arg" == "--max" ]]; then shift_next=1
    elif [[ "$shift_next" == "1" ]]; then MAX_MSGS="$arg"; shift_next=0; fi
done

[[ -n "$ITCH_FILE" ]] || { echo "Usage: bash gui/run_pipeline.sh <itch_file> [run_dir] [--max N]" >&2; exit 2; }
[[ -f "$ITCH_FILE" ]] || { echo "[pipeline] ERROR: ITCH file not found: $ITCH_FILE" >&2; exit 2; }

[[ -n "$RUN_DIR" ]] || RUN_DIR="$ROOT/gui/runs/$(date '+%Y%m%d_%H%M%S')"
mkdir -p "$RUN_DIR"

echo "==== pipeline: $ITCH_FILE ===="
echo "  run dir:  $RUN_DIR"
echo "  max msgs: ${MAX_MSGS:-all}"
echo "  env:      $ENV_NAME"

# ---- build helpers ----------------------------------------------------------
build_sw() {
    if [[ "$IS_WSL" == "1" ]]; then bash gui/build_linux.sh sw
    else make sw; fi
}
build_rtl() {
    if [[ "$IS_WSL" == "1" ]]; then bash gui/build_linux.sh rtl
    else bash sim/run_rtl_tests.sh; fi
}

# ---- [1/5] SW binary --------------------------------------------------------
echo; echo "==== [1/5] orderbook_sw ===="
if [[ ! -f "$SW_BIN" ]]; then
    echo "  missing — building ..."
    build_sw 2>&1 || { echo "[pipeline] FAIL: SW build failed" >&2; exit 1; }
    [[ -f "$SW_BIN" ]] || { echo "[pipeline] FAIL: $SW_BIN still missing" >&2; exit 1; }
else
    echo "  $SW_BIN ready"
fi

# ---- [2/5] tb_latency -------------------------------------------------------
echo; echo "==== [2/5] tb_latency (RTL harness) ===="
if [[ ! -f "$TB_LAT" ]]; then
    echo "  missing — building RTL ..."
    build_rtl 2>&1 || { echo "[pipeline] FAIL: RTL build failed" >&2; exit 1; }
    [[ -f "$TB_LAT" ]] || { echo "[pipeline] FAIL: $TB_LAT still missing" >&2; exit 1; }
else
    echo "  $TB_LAT ready"
fi

# ---- [3/5] SW engine=simd ---------------------------------------------------
echo; echo "==== [3/5] SW engine=simd ===="
"$SW_BIN" "$ITCH_FILE" --csv "$RUN_DIR" --quiet --ladder "$RUN_DIR" --engine simd 2>&1 \
    || { echo "[pipeline] FAIL: SW simd run failed" >&2; exit 1; }
echo "  simd done — latency_simd_*.csv"

# ---- [4/5] SW engine=map ----------------------------------------------------
echo; echo "==== [4/5] SW engine=map ===="
"$SW_BIN" "$ITCH_FILE" --csv "$RUN_DIR" --quiet --engine map 2>&1 \
    || { echo "[pipeline] FAIL: SW map run failed" >&2; exit 1; }
echo "  map done — latency_map_*.csv"

# ---- [5/5] RTL latency ------------------------------------------------------
echo; echo "==== [5/5] RTL latency (tb_latency) ===="
RTL_ARGS=("$ITCH_FILE" --csv "$RUN_DIR")
[[ "$MAX_MSGS" -gt 0 ]] && RTL_ARGS+=(--max "$MAX_MSGS")
"$TB_LAT" "${RTL_ARGS[@]}" 2>&1 || { echo "[pipeline] FAIL: RTL latency run failed" >&2; exit 1; }
echo "  RTL done — latency_hw_*.csv"

# ---- summary ----------------------------------------------------------------
echo; echo "==== pipeline complete ===="
ls "$RUN_DIR"/*.csv 2>/dev/null | while IFS= read -r f; do echo "    $(basename "$f")"; done

cat > "$RUN_DIR/manifest.json" <<EOF
{
  "itch_file": "$(realpath "$ITCH_FILE")",
  "run_dir":   "$(realpath "$RUN_DIR")",
  "engines":   ["simd", "map", "hw"],
  "env":       "$ENV_NAME",
  "timestamp": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
EOF
echo "  manifest.json written"
