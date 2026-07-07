"""Run shell commands with live output in Streamlit."""
from __future__ import annotations
import subprocess
import streamlit as st
from typing import Iterator


def _pick_bash() -> str:
    """Find a Bash interpreter (Git Bash or MSYS2 on Windows)."""
    import shutil, sys
    if sys.platform != 'win32':
        return 'bash'
    for candidate in [
        r'C:\msys64\usr\bin\bash.exe',
        r'C:\Program Files\Git\bin\bash.exe',
        r'C:\Program Files (x86)\Git\bin\bash.exe',
        'bash',
    ]:
        if shutil.which(candidate) or (candidate != 'bash' and
                                        __import__('os').path.exists(candidate)):
            return candidate
    return 'bash'


def run_bash(script: str, status_label: str = 'Running…',
             cwd: str | None = None, env: dict | None = None,
             stream: bool = True) -> tuple[int, str]:
    """
    Run a bash command string. Shows live output in a st.status expander.
    Returns (returncode, full_stdout).
    """
    bash = _pick_bash()
    cmd = [bash, '-c', script]

    output_lines: list[str] = []

    with st.status(status_label, expanded=True) as status:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, cwd=cwd, env=env, bufsize=1
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip()
            output_lines.append(line)
            if stream:
                st.text(line)
        proc.wait()
        if proc.returncode == 0:
            status.update(label=f'{status_label} — done', state='complete')
        else:
            status.update(label=f'{status_label} — FAILED (exit {proc.returncode})',
                          state='error')

    return proc.returncode, '\n'.join(output_lines)


def run_bash_quiet(script: str, cwd: str | None = None,
                   env: dict | None = None) -> tuple[int, str]:
    """Run bash quietly; return (returncode, stdout+stderr combined)."""
    bash = _pick_bash()
    result = subprocess.run(
        [bash, '-c', script], capture_output=True, text=True, cwd=cwd, env=env
    )
    return result.returncode, result.stdout + result.stderr
