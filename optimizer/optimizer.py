from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from typing import Optional


CPP_SOURCE = os.path.join(os.path.dirname(__file__), "locusopt.cpp")


def _compile_cpp(binary_path: str) -> None:
    if not shutil.which("g++"):
        raise RuntimeError("g++ is required to build the C++ optimizer.")
    cmd = ["g++", "-std=c++17", "-O2", CPP_SOURCE, "-o", binary_path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "Failed to compile C++ optimizer.")


def _get_binary() -> str:
    binary_path = os.path.join(tempfile.gettempdir(), "locusopt_cpp")
    if (not os.path.isfile(binary_path) or
            os.path.getmtime(binary_path) < os.path.getmtime(CPP_SOURCE)):
        _compile_cpp(binary_path)
    return binary_path


def optimize_file(
    source: str,
    output: Optional[str] = None,
    func: Optional[str] = None,
    tile: int = 32,
    disable_interchange: bool = False,
    disable_tiling: bool = False,
) -> Optional[str]:
    binary = _get_binary()
    cmd = [binary, "optimize", source, "--tile", str(tile)]
    if func:
        cmd.extend(["--func", func])
    if output:
        cmd.extend(["--output", output])
    if disable_interchange:
        cmd.append("--no-interchange")
    if disable_tiling:
        cmd.append("--no-tiling")

    if output:
        proc = subprocess.run(cmd, text=True, capture_output=True)
    else:
        proc = subprocess.run(cmd, text=True, capture_output=True)

    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "C++ optimizer failed.")

    if output:
        return None

    return proc.stdout
