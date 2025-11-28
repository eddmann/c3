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

  Raises:
    ValueError: If cmd is empty.
    subprocess.CalledProcessError: If the command returns non-zero exit code.
  """
  cmd_list = list(cmd)
  if not cmd_list:
    raise ValueError("Command cannot be empty")

  global _active_process
  log_file = None
  try:
    if log_path:
      log_file = open(log_path, "w", encoding="utf-8")

    popen_kwargs = {
        "stdout": log_file if log_file else None,
        "stderr": subprocess.STDOUT if log_file else None,
        "text": True,
    }
    # Create new process group on Unix so we can kill the entire tree
    if sys.platform != "win32":
      popen_kwargs["start_new_session"] = True

    proc = subprocess.Popen(cmd_list, **popen_kwargs)
    _active_process = proc
    try:
      returncode = proc.wait()
    finally:
      _active_process = None

    if returncode != 0:
      raise subprocess.CalledProcessError(returncode, cmd_list)
    return subprocess.CompletedProcess(cmd_list, returncode)
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
  """Convert win percentage (0-1) to Elo difference.

  Uses the standard Elo formula: Elo = 400 * log10(score / (1 - score))
  """
  score = min(max(score, 1e-4), 1 - 1e-4)
  return 400 * math.log10(score / (1 - score))


def elo_error(score: float, games: int) -> float:
  """Calculate standard error of Elo estimate using delta method."""
  if games == 0:
    return float("inf")
  p = min(max(score, 1e-6), 1 - 1e-6)
  var = p * (1 - p) / games
  deriv = 400 / (math.log(10) * p * (1 - p))
  return deriv * math.sqrt(var)


def los(score: float, games: int) -> float:
  """Calculate Likelihood of Superiority.

  Returns the probability that the true strength difference is > 0,
  using the cumulative normal distribution.
  """
  if games == 0:
    return 0.5
  p = score
  var = p * (1 - p) / games
  if var == 0:
    return 1.0 if p > 0.5 else 0.0
  z = (p - 0.5) / math.sqrt(var)
  return 0.5 * (1 + math.erf(z / math.sqrt(2)))


# Git utilities for branch comparison scripts

def is_git_ref(ref: str) -> bool:
  """Check if ref is a git reference (branch/tag/commit) vs external path."""
  if Path(ref).exists():
    return False
  result = subprocess.run(
      ["git", "rev-parse", "--verify", ref],
      cwd=ROOT,
      capture_output=True,
      timeout=30,
  )
  return result.returncode == 0


def get_commit_hash(ref: str) -> str:
  """Get the short commit hash for a git ref."""
  result = subprocess.run(
      ["git", "rev-parse", "--short", ref],
      cwd=ROOT,
      capture_output=True,
      text=True,
      timeout=30,
  )
  return result.stdout.strip() if result.returncode == 0 else ref


def checkout_and_build(ref: str, build_dir: Path) -> Path:
  """Checkout ref to worktree and build release binary.

  Args:
    ref: Git reference (branch, tag, or commit hash).
    build_dir: Directory to place worktree and build artifacts.

  Returns:
    Path to the built engine binary.
  """
  worktree_dir = build_dir / "src"
  cmake_build_dir = build_dir / "build"

  print(f"  Checking out {ref}...")
  result = subprocess.run(
      ["git", "worktree", "add", "--detach", str(worktree_dir), ref],
      cwd=ROOT,
      capture_output=True,
      timeout=60,
  )
  if result.returncode != 0:
    raise RuntimeError(f"Failed to checkout {ref}: {result.stderr.decode()}")

  try:
    print(f"  Building {ref}...")
    binary = build_engine(worktree_dir, cmake_build_dir)
    return binary
  except subprocess.CalledProcessError as e:
    raise RuntimeError(f"Build failed for {ref}: {e}") from e


def cleanup_worktree(worktree_dir: Path) -> None:
  """Remove a git worktree, warning on failure."""
  if not worktree_dir.exists():
    return
  result = subprocess.run(
      ["git", "worktree", "remove", "--force", str(worktree_dir)],
      cwd=ROOT,
      capture_output=True,
      timeout=30,
  )
  if result.returncode != 0:
    print(f"Warning: Failed to clean up worktree {worktree_dir}", file=sys.stderr)


# PGN parsing

def parse_pgn_results(
    pgn_path: Path,
    name_a: str,
    name_b: str | None = None,
) -> tuple[int, int, int]:
  """Parse PGN file and return win/draw/loss counts.

  Args:
    pgn_path: Path to PGN file.
    name_a: Name of engine A (counts as wins).
    name_b: Name of engine B (counts as losses). If None, any non-name_a opponent.

  Returns:
    Tuple of (wins_a, wins_b, draws).

  Raises:
    FileNotFoundError: If PGN file doesn't exist.
  """
  if not pgn_path.exists():
    raise FileNotFoundError(f"PGN file not found: {pgn_path}")

  wins_a = wins_b = draws = 0
  white = black = result = None

  def flush():
    nonlocal wins_a, wins_b, draws, white, black, result
    if white is None or black is None or result is None:
      return
    if result == "1-0":
      if white == name_a:
        wins_a += 1
      else:
        wins_b += 1
    elif result == "0-1":
      if black == name_a:
        wins_a += 1
      else:
        wins_b += 1
    else:
      draws += 1
    white = black = result = None

  for line in pgn_path.read_text(encoding="utf-8", errors="replace").splitlines():
    line = line.strip()
    if line.startswith("[White "):
      parts = line.split('"')
      if len(parts) >= 2:
        white = parts[1]
    elif line.startswith("[Black "):
      parts = line.split('"')
      if len(parts) >= 2:
        black = parts[1]
    elif line.startswith("[Result "):
      parts = line.split('"')
      if len(parts) >= 2:
        result = parts[1]
    elif line == "":
      flush()

  flush()
  return wins_a, wins_b, draws
