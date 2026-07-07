#!/usr/bin/env bash
# run_gui.sh — launch the Streamlit order-book GUI.
#
# Creates a venv (on first run), installs deps, then: streamlit run gui/app.py
#
# In WSL the venv lives on the NATIVE Linux filesystem (~/.local/share/) to
# avoid pip-vendor corruption that happens when pip unpacks its bundled packages
# onto an NTFS mount (/mnt/c/).  MSYS2 keeps the venv inside gui/venv/ as
# before.
#
# Run from the orderbook_project/ directory:
#   bash gui/run_gui.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

# ---- detect environment -----------------------------------------------------
is_wsl() { grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; }

# Venv location: Linux filesystem in WSL (avoids NTFS pip-vendor issues),
# project-local in MSYS2 / native Linux.
if is_wsl; then
    VENV="${HOME}/.local/share/orderbook-gui-venv"
else
    VENV="$HERE/venv"
fi

# ---- find a working system Python -------------------------------------------
pick_system_python() {
    local candidates=()
    if is_wsl || [[ "$(uname -s)" == "Linux" ]]; then
        candidates=(python3 python /usr/bin/python3 /usr/local/bin/python3)
    else
        # MSYS2 / Git Bash
        local user="${USERNAME:-${USER:-$(whoami)}}"
        candidates=(
            "/c/Users/$user/AppData/Local/Microsoft/WindowsApps/python3"
            "/c/Users/$user/AppData/Local/Microsoft/WindowsApps/python"
            "/c/msys64/ucrt64/bin/python3"
            python3
            python
        )
    fi
    for py in "${candidates[@]}"; do
        if command -v "$py" &>/dev/null || [[ -x "$py" ]]; then
            "$py" --version &>/dev/null && echo "$py" && return 0
        fi
    done
    return 1
}

# ---- locate venv python, verify pip is intact --------------------------------
# In WSL: only accept bin/python (Linux venv); Scripts/python.exe = wrong.
# Also check pip._vendor — it gets corrupted when ensurepip is interrupted on NTFS.
find_venv_python() {
    if is_wsl || [[ "$(uname -s)" == "Linux" ]]; then
        [[ -f "$VENV/bin/python" ]] || return 1
        "$VENV/bin/python" --version &>/dev/null || return 1
        "$VENV/bin/python" -c "import pip._vendor" &>/dev/null || return 1
        echo "$VENV/bin/python"
        return 0
    fi
    # MSYS2 / native Windows: accept either venv layout
    local vpy=""
    [[ -f "$VENV/bin/python" ]]                      && vpy="$VENV/bin/python"
    [[ -z "$vpy" && -f "$VENV/Scripts/python.exe" ]] && vpy="$VENV/Scripts/python.exe"
    [[ -z "$vpy" ]] && return 1
    "$vpy" --version &>/dev/null || return 1
    echo "$vpy"
}

# ---- create venv + install requirements -------------------------------------
# ALL status messages go to stderr (>&2 2>&1) so stdout stays clean.
# The single `echo "$vpy"` at the end is the only thing captured by VENV_PY=.
setup_venv() {
    local base_py="$1"

    local vpy
    vpy="$(find_venv_python)" || vpy=""

    if [[ -z "$vpy" ]]; then
        echo "[run_gui] creating venv at $VENV ..." >&2
        [[ -d "$VENV" ]] && rm -rf "$VENV"
        mkdir -p "$(dirname "$VENV")"
        "$base_py" -m venv "$VENV" || {
            echo "[run_gui] ERROR: venv creation failed." >&2
            if is_wsl || [[ "$(uname -s)" == "Linux" ]]; then
                echo "  Fix: sudo apt update && sudo apt install python3-venv python3-full" >&2
            fi
            return 1
        }
        vpy="$(find_venv_python)" || {
            echo "[run_gui] ERROR: venv created but pip is broken or python not found." >&2
            echo "  Try: rm -rf $VENV && bash gui/run_gui.sh" >&2
            return 1
        }
    fi

    echo "[run_gui] installing/updating requirements ..." >&2
    # IMPORTANT: redirect order is >&2 2>&1 (stdout→stderr first, then stderr→stdout-now-stderr)
    # Wrong order (2>&1 >&2) would send pip errors into the capture and corrupt VENV_PY.
    "$vpy" -m pip install -q --upgrade pip            >&2 2>&1 || true
    "$vpy" -m pip install -q -r gui/requirements.txt  >&2 2>&1 || {
        echo "[run_gui] ERROR: pip install failed." >&2
        echo "  Check network, then retry: bash gui/run_gui.sh" >&2
        return 1
    }

    echo "$vpy"   # sole stdout line → captured into VENV_PY
}

# ---- main -------------------------------------------------------------------
BASE_PY="$(pick_system_python)" || true
if [[ -z "$BASE_PY" ]]; then
    echo "[run_gui] ERROR: no Python interpreter found." >&2
    if is_wsl || [[ "$(uname -s)" == "Linux" ]]; then
        echo "  Fix: sudo apt update && sudo apt install python3 python3-venv" >&2
    else
        echo "  Fix: install Python from https://python.org or the Microsoft Store" >&2
    fi
    exit 1
fi

PY_VER=$("$BASE_PY" --version 2>&1)
echo "[run_gui] base Python: $BASE_PY ($PY_VER)"
echo "[run_gui] venv: $VENV"

VENV_PY="$(setup_venv "$BASE_PY")"
if [[ -z "$VENV_PY" ]]; then
    echo "[run_gui] ERROR: setup failed." >&2
    exit 1
fi

echo "[run_gui] launching Streamlit — open http://localhost:8501 in your browser"
"$VENV_PY" -m streamlit run gui/app.py
