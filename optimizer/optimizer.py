from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
from typing import List, Optional


CPP_SOURCE = os.path.join(os.path.dirname(__file__), "locusopt.cpp")
if os.name == "nt":
    _CXX_CANDIDATES = ("g++", "clang++")
else:
    _CXX_CANDIDATES = ("g++", "clang++", "c++")


def _resolve_cxx() -> List[str]:
    env = os.environ.get("LOX_CXX")
    if env is None:
        env = os.environ.get("CXX")
    if env is not None:
        env = env.strip()
        if not env:
            raise RuntimeError("CXX is set but empty.")
        parts = shlex.split(env)
        if not parts or not parts[0]:
            raise RuntimeError("CXX environment variable contains no valid command.")
        if not shutil.which(parts[0]):
            raise RuntimeError(f"C++ compiler not found: {parts[0]}")
        return parts
    for candidate in _CXX_CANDIDATES:
        if shutil.which(candidate):
            return [candidate]
    raise RuntimeError(
        "C++ compiler not found. Install g++, clang++, or c++ (or set LOX_CXX/CXX)."
    )


def _compile_cpp(binary_path: str) -> None:
    cxx = _resolve_cxx()
    cmd = cxx + ["-std=c++17", "-O2", CPP_SOURCE, "-o", binary_path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "Failed to compile C++ optimizer.")


def _get_binary() -> str:
    suffix = ".exe" if os.name == "nt" else ""
    binary_path = os.path.join(
        tempfile.gettempdir(),
        f"locusopt_cpp_{os.getpid()}{suffix}",
    )
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

    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "C++ optimizer failed.")

    if output:
        return None

    return proc.stdout


def analyze_file_cpp(source: str, func: Optional[str] = None) -> str:
    binary = _get_binary()
    cmd = [binary, "analyze", source]
    if func:
        cmd.extend(["--func", func])
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "C++ analyzer failed.")
    return proc.stdout
