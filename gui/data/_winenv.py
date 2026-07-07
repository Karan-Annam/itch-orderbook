"""_winenv.py — cross-environment path helpers.

The GUI's Python may run under WSL (Linux) while the compute binaries are
native Windows .exe files (built with MSYS2 g++/Verilator). WSL interop can
*launch* a Windows .exe, but the .exe uses Win32 file APIs and cannot open a
Linux path like /mnt/c/foo — it needs C:\\foo.

These helpers translate a path into the form a Windows .exe expects, only when
we're actually under WSL. Everywhere else they return the path unchanged.

Python keeps using the ORIGINAL (Linux) path for its own open()/os.path calls;
only the argument handed to a Windows .exe is converted.
"""
from __future__ import annotations
import functools
import subprocess


@functools.lru_cache(maxsize=1)
def is_wsl() -> bool:
    try:
        with open('/proc/version', 'r') as f:
            v = f.read().lower()
        return 'microsoft' in v or 'wsl' in v
    except OSError:
        return False


def arg_path(binary, path) -> str:
    """Path in the form the given `binary` expects for a filename argument.

    Only a Windows .exe launched under WSL needs a translated Windows path
    (C:\\...). Native Linux (ELF) binaries — and MSYS2 programs — take the
    original /mnt/c or /c path unchanged, so we leave it alone. This lets the
    same code drive either the native Linux build or the Windows .exe build.
    """
    p = str(path)
    if not (is_wsl() and str(binary).endswith('.exe')):
        return p
    if len(p) >= 2 and p[1] == ':':          # already a Windows path
        return p
    try:
        out = subprocess.run(['wslpath', '-w', p],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip() or p
    except (subprocess.CalledProcessError, FileNotFoundError):
        return p


def to_exe_path(p) -> str:
    """Back-compat shim: assume a Windows .exe target."""
    return arg_path('x.exe', p)
