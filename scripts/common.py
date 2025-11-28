#!/usr/bin/env python3
"""Shared utilities for c3 scripts."""

from __future__ import annotations

import atexit
import math
import os
import shutil
import signal
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional

# Project root directory
ROOT = Path(__file__).resolve().parent.parent

# Default output directory for fastchess results
FASTCHESS_DIR = ROOT / "Testing" / "fastchess"

# Track active subprocess for cleanup on forced exit
_active_process: Optional[subprocess.Popen] = None


def _cleanup_process() -> None:
  """Terminate and wait for the active subprocess if it exists."""
  global _active_process
  if _active_process is not None and _active_process.poll() is None:
    # Send SIGTERM to the process group to kill fastchess and its child engines
    try:
      if sys.platform != "win32":
        os.killpg(os.getpgid(_active_process.pid), signal.SIGTERM)
      else:
        _active_process.terminate()
      _active_process.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired, OSError):
      # Force kill if SIGTERM didn't work
      try:
        if sys.platform != "win32":
          os.killpg(os.getpgid(_active_process.pid), signal.SIGKILL)
        else:
          _active_process.kill()
        _active_process.wait(timeout=2)
      except (ProcessLookupError, subprocess.TimeoutExpired, OSError):
        pass
    _active_process = None


def _signal_handler(signum: int, frame) -> None:
  """Handle SIGINT/SIGTERM by cleaning up subprocesses and exiting."""
  print(f"\nReceived signal {signum}, cleaning up...")
  _cleanup_process()
  sys.exit(128 + signum)


# Register cleanup handlers
atexit.register(_cleanup_process)
if sys.platform != "win32":
  signal.signal(signal.SIGINT, _signal_handler)
  signal.signal(signal.SIGTERM, _signal_handler)


def generate_timestamp() -> str:
  """Generate ISO-style timestamp for output filenames."""
  return datetime.now().strftime("%Y-%m-%d_%H-%M-%S")


def run(cmd: Iterable[str], log_path: Path | None = None) -> subprocess.CompletedProcess:
  """Run a command with cleanup tracking for graceful termination on Ctrl+C.

  Args:
    cmd: Command and arguments to run.
    log_path: If provided, redirect stdout/stderr to this file.
  """
  global _active_process
  log_file = open(log_path, "w", encoding="utf-8") if log_path else None
  try:
    popen_kwargs = {
        "stdout": log_file if log_file else None,
        "stderr": subprocess.STDOUT if log_file else None,
        "text": True,
    }
    # Create new process group on Unix so we can kill the entire tree
    if sys.platform != "win32":
      popen_kwargs["start_new_session"] = True

    proc = subprocess.Popen(list(cmd), **popen_kwargs)
    _active_process = proc
    try:
      returncode = proc.wait()
    finally:
      _active_process = None

    if returncode != 0:
      raise subprocess.CalledProcessError(returncode, cmd)
    return subprocess.CompletedProcess(cmd, returncode)
  finally:
    if log_file:
      log_file.close()


def build_engine(source_dir: Path, build_dir: Path) -> Path:
  """Build c3 engine from source directory into build directory.

  Args:
    source_dir: Path to source tree containing CMakeLists.txt.
    build_dir: Path where build artifacts will be placed.

  Returns:
    Path to the built c3 binary.
  """
  build_dir.mkdir(parents=True, exist_ok=True)
  run(["cmake", "-S", str(source_dir), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release"])
  run(["cmake", "--build", str(build_dir), "--config", "Release", "--target", "c3"])
  return build_dir / "c3"


def ensure_fastchess_available(path: str) -> None:
  """Check that fastchess binary is available, exit with helpful message if not."""
  if shutil.which(path) is None:
    raise SystemExit(
        f"fastchess binary '{path}' not found. Install from https://github.com/Disservin/fastchess "
        "and ensure it is on PATH, or pass --fastchess /path/to/fastchess.")


# Statistical helpers for Elo calculations

def elo_from_score(score: float) -> float:
  """Convert win percentage (0-1) to Elo difference."""
  score = min(max(score, 1e-4), 1 - 1e-4)
  return 400 * math.log10(score / (1 - score))


def elo_error(score: float, games: int) -> float:
  """Calculate standard error of Elo estimate."""
  if games == 0:
    return float("inf")
  p = min(max(score, 1e-6), 1 - 1e-6)
  var = p * (1 - p) / games
  deriv = 400 / (math.log(10) * p * (1 - p))
  return deriv * math.sqrt(var)


def los(score: float, games: int) -> float:
  """Calculate Likelihood of Superiority (probability that true strength > 0)."""
  if games == 0:
    return 0.5
  p = score
  var = p * (1 - p) / games
  if var == 0:
    return 1.0 if p > 0.5 else 0.0
  z = (p - 0.5) / math.sqrt(var)
  return 0.5 * (1 + math.erf(z / math.sqrt(2)))
