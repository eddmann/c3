#!/usr/bin/env python3
"""
Run a fastchess gauntlet between c3 and another engine,
and produce a compact SPRT-style summary.

Two modes are supported:
  * depth: fixed search depth (default depth=3)
  * movetime: fixed per-move time in milliseconds (default 50ms)

Outputs (timestamped by default, e.g., 2025-01-15_14-30-45):
  - PGN of all games: Testing/fastchess/games_<timestamp>.pgn
  - fastchess stdout log: Testing/fastchess/fastchess_<timestamp>.log
  - summary text: Testing/fastchess/summary_<timestamp>.txt

If fastchess is not available, the script aborts with a clear hint.

You can also run purely in parsing mode with --summarize-only to turn an
existing PGN into a summary (used by tests, no fastchess required).
"""

from __future__ import annotations

import argparse
import atexit
import math
import os
import signal
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional, Tuple

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


ROOT = Path(__file__).resolve().parent.parent
FASTCHESS_DIR = ROOT / "Testing" / "fastchess"


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


def ensure_c3_built() -> Path:
  """Build c3 from current source (backwards compatible wrapper)."""
  return build_engine(ROOT, ROOT / "build-release")


def build_fastchess_command(args: argparse.Namespace, mode: str, pgn_path: Path) -> list[str]:
  c3 = ensure_c3_built()
  opponent = Path(args.opponent)
  opponent_name = args.opponent_name or opponent.stem

  engines = [
      (f"cmd={c3}", "name=c3"),
      (f"cmd={opponent}", f"name={opponent_name}"),
  ]

  time_conf = [f"depth={args.depth}"] if mode == "depth" else [f"st={args.movetime_ms/1000:.3f}"]

  cmd = [
      args.fastchess,
      "-tournament", "gauntlet",
      "-seeds", "1",
      "-engine", engines[0][0], engines[0][1],
      "-engine", engines[1][0], engines[1][1],
      "-rounds", str(args.games),
      "-games", "1",
      "-concurrency", str(args.concurrency),
      "-pgnout", f"file={pgn_path}",
      "-each", "proto=uci", *time_conf,
      "-recover",
  ]

  return cmd


def parse_pgn_results(pgn_path: Path) -> Tuple[int, int, int]:
  wins_a = wins_b = draws = 0
  white = black = result = None

  def flush():
    nonlocal wins_a, wins_b, draws, white, black, result
    if white is None or black is None or result is None:
      return
    if result == "1-0":
      if white == "c3":
        wins_a += 1
      else:
        wins_b += 1
    elif result == "0-1":
      if black == "c3":
        wins_a += 1
      else:
        wins_b += 1
    else:
      draws += 1
    white = black = result = None

  for line in pgn_path.read_text(encoding="utf-8").splitlines():
    line = line.strip()
    if line.startswith("[White "):
      white = line.split('"')[1]
    elif line.startswith("[Black "):
      black = line.split('"')[1]
    elif line.startswith("[Result "):
      result = line.split('"')[1]
    elif line == "":
      flush()

  flush()
  return wins_a, wins_b, draws


def elo_from_score(score: float) -> float:
  score = min(max(score, 1e-4), 1 - 1e-4)
  return 400 * math.log10(score / (1 - score))


def elo_error(score: float, games: int) -> float:
  if games == 0:
    return float("inf")
  p = min(max(score, 1e-6), 1 - 1e-6)
  var = p * (1 - p) / games
  deriv = 400 / (math.log(10) * p * (1 - p))
  return deriv * math.sqrt(var)


def los(score: float, games: int) -> float:
  if games == 0:
    return 0.5
  p = score
  var = p * (1 - p) / games
  if var == 0:
    return 1.0 if p > 0.5 else 0.0
  z = (p - 0.5) / math.sqrt(var)
  return 0.5 * (1 + math.erf(z / math.sqrt(2)))


def write_summary(pgn_path: Path, summary_path: Path, label: str, opponent_name: str) -> str:
  wins_a, wins_b, draws = parse_pgn_results(pgn_path)
  games = wins_a + wins_b + draws
  score = (wins_a + 0.5 * draws) / games if games else 0.5
  elo = elo_from_score(score)
  err = elo_error(score, games)
  likelihood = los(score, games)

  summary = (
      f"=== {label} ===\n"
      f"PGN: {pgn_path}\n"
      f"Games: {games}\n"
      f"W/D/L (c3 vs {opponent_name}): {wins_a}/{draws}/{wins_b}\n"
      f"Score: {score:.3f}\n"
      f"Elo diff: {elo:+.1f} +/- {err:.1f}\n"
      f"LOS: {likelihood*100:.1f}%\n"
  )

  summary_path.parent.mkdir(parents=True, exist_ok=True)
  summary_path.write_text(summary, encoding="utf-8")
  return summary


def ensure_fastchess_available(path: str) -> None:
  if shutil.which(path) is None:
    raise SystemExit(
        f"fastchess binary '{path}' not found. Install from https://github.com/Disservin/fastchess "
        "and ensure it is on PATH, or pass --fastchess /path/to/fastchess.")


def main() -> None:
  timestamp = generate_timestamp()

  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--opponent", type=str, help="path to opponent engine binary (required for gauntlet)")
  parser.add_argument("--opponent-name", type=str, default=None, help="name for opponent engine (default: filename)")
  parser.add_argument("--fastchess", default="fastchess", help="fastchess executable")
  parser.add_argument("--games", type=int, default=200, help="number of games (rounds)")
  parser.add_argument("--concurrency", type=int, default=4, help="parallel games")
  parser.add_argument("--depth", type=int, default=3, help="fixed depth for depth mode")
  parser.add_argument("--movetime-ms", type=int, default=50, help="per-move time for movetime mode")
  parser.add_argument("--mode", choices=["depth", "movetime"], default="depth",
                      help="gauntlet time control type")
  parser.add_argument("--pgn", type=Path, default=None,
                      help="output PGN path (default: timestamped in Testing/fastchess/)")
  parser.add_argument("--log", type=Path, default=None,
                      help="fastchess stdout log (default: timestamped in Testing/fastchess/)")
  parser.add_argument("--summary", type=Path, default=None,
                      help="summary output path (default: timestamped in Testing/fastchess/)")
  parser.add_argument("--summarize-only", type=Path, help="skip fastchess run, just summarize given PGN")

  args = parser.parse_args()

  # Apply timestamped defaults if not explicitly provided
  if args.pgn is None:
    args.pgn = FASTCHESS_DIR / f"games_{timestamp}.pgn"
  if args.log is None:
    args.log = FASTCHESS_DIR / f"fastchess_{timestamp}.log"
  if args.summary is None:
    args.summary = FASTCHESS_DIR / f"summary_{timestamp}.txt"

  FASTCHESS_DIR.mkdir(parents=True, exist_ok=True)

  if args.summarize_only:
    opponent_name = args.opponent_name or "opponent"
    print(write_summary(args.summarize_only, args.summary, label="summary", opponent_name=opponent_name))
    return

  if not args.opponent:
    parser.error("--opponent is required when running a gauntlet")

  ensure_fastchess_available(args.fastchess)

  opponent_name = args.opponent_name or Path(args.opponent).stem
  cmd = build_fastchess_command(args, args.mode, args.pgn)
  print("Running fastchess...")
  run(cmd, log_path=args.log)

  summary = write_summary(args.pgn, args.summary, label=f"fastchess ({args.mode})", opponent_name=opponent_name)
  print(summary)


if __name__ == "__main__":
  main()
