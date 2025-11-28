#!/usr/bin/env python3
"""
Compare engine strength between two git branches via SPRT self-play.

Builds both base and test versions, then runs fastchess between them
to measure Elo difference with confidence intervals.

Examples:
  # Compare HEAD against main branch
  python3 scripts/compare_branches.py --base main --test HEAD

  # Compare against external engine
  python3 scripts/compare_branches.py --test HEAD --external /path/to/stockfish

  # With options
  python3 scripts/compare_branches.py --base main --test HEAD \\
    --games 200 --depth 5 --concurrency 4

  # CI mode (structured output)
  python3 scripts/compare_branches.py --base main --test HEAD --ci
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from common import (
    ROOT,
    FASTCHESS_DIR,
    generate_timestamp,
    build_engine,
    run,
    elo_from_score,
    elo_error,
    los,
    ensure_fastchess_available,
)


def is_git_ref(ref: str) -> bool:
  """Check if ref is a git reference (branch/tag/commit) vs external path."""
  if Path(ref).exists():
    return False
  result = subprocess.run(
      ["git", "rev-parse", "--verify", ref],
      cwd=ROOT,
      capture_output=True,
  )
  return result.returncode == 0


def get_commit_hash(ref: str) -> str:
  """Get the short commit hash for a git ref."""
  result = subprocess.run(
      ["git", "rev-parse", "--short", ref],
      cwd=ROOT,
      capture_output=True,
      text=True,
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
  subprocess.run(
      ["git", "worktree", "add", "--detach", str(worktree_dir), ref],
      cwd=ROOT,
      check=True,
      capture_output=True,
  )

  try:
    print(f"  Building {ref}...")
    binary = build_engine(worktree_dir, cmake_build_dir)
    return binary
  except subprocess.CalledProcessError as e:
    raise RuntimeError(f"Build failed for {ref}: {e}") from e


def cleanup_worktree(worktree_dir: Path) -> None:
  """Remove a git worktree."""
  if worktree_dir.exists():
    subprocess.run(
        ["git", "worktree", "remove", "--force", str(worktree_dir)],
        cwd=ROOT,
        capture_output=True,
    )


def parse_pgn_results_generic(pgn_path: Path, name_a: str, name_b: str) -> tuple[int, int, int]:
  """Parse PGN results for arbitrary engine names.

  Args:
    pgn_path: Path to PGN file.
    name_a: Name of engine A (test).
    name_b: Name of engine B (base).

  Returns:
    Tuple of (wins_a, wins_b, draws).
  """
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


def build_comparison_command(
    engine_a: Path,
    engine_b: Path,
    name_a: str,
    name_b: str,
    args: argparse.Namespace,
    pgn_path: Path,
) -> list[str]:
  """Build fastchess command for head-to-head comparison."""
  time_conf = (
      [f"depth={args.depth}"]
      if args.mode == "depth"
      else [f"st={args.movetime_ms / 1000:.3f}"]
  )

  return [
      args.fastchess,
      "-tournament", "gauntlet",
      "-seeds", "1",
      "-engine", f"cmd={engine_a}", f"name={name_a}",
      "-engine", f"cmd={engine_b}", f"name={name_b}",
      "-rounds", str(args.games),
      "-games", "1",
      "-concurrency", str(args.concurrency),
      "-pgnout", f"file={pgn_path}",
      "-each", "proto=uci", *time_conf,
      "-recover",
  ]


def write_comparison_summary(
    pgn_path: Path,
    name_test: str,
    name_base: str,
    base_ref: str,
    test_ref: str,
    ci_mode: bool = False,
) -> tuple[str, int]:
  """Write comparison summary and return (summary_text, exit_code)."""
  wins_test, wins_base, draws = parse_pgn_results_generic(pgn_path, name_test, name_base)
  games = wins_test + wins_base + draws

  if games == 0:
    return "No games completed.", 3

  score = (wins_test + 0.5 * draws) / games
  elo = elo_from_score(score)
  err = elo_error(score, games)
  likelihood = los(score, games)

  # Determine result
  if likelihood >= 0.95:
    result = "Test appears stronger"
    exit_code = 0
  elif likelihood <= 0.05:
    result = "Test appears weaker"
    exit_code = 1
  else:
    result = "Inconclusive"
    exit_code = 2

  if ci_mode:
    summary = (
        f"base={base_ref}\n"
        f"test={test_ref}\n"
        f"games={games}\n"
        f"wins={wins_test}\n"
        f"losses={wins_base}\n"
        f"draws={draws}\n"
        f"score={score:.3f}\n"
        f"elo={elo:+.1f}\n"
        f"elo_error={err:.1f}\n"
        f"los={likelihood * 100:.1f}\n"
        f"result={result}\n"
    )
  else:
    summary = (
        f"=== Branch Comparison ===\n"
        f"Base: {base_ref}\n"
        f"Test: {test_ref}\n"
        f"\n"
        f"Games: {games}\n"
        f"W/D/L (test vs base): {wins_test}/{draws}/{wins_base}\n"
        f"Score: {score:.3f}\n"
        f"Elo diff: {elo:+.1f} +/- {err:.1f}\n"
        f"LOS: {likelihood * 100:.1f}%\n"
        f"\n"
        f"Result: {result}\n"
    )

  return summary, exit_code


def main() -> None:
  parser = argparse.ArgumentParser(
      description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter,
  )
  parser.add_argument(
      "--base",
      default="main",
      help="Base git ref to compare against (default: main)",
  )
  parser.add_argument(
      "--test",
      default="HEAD",
      help="Test git ref (default: HEAD)",
  )
  parser.add_argument(
      "--external",
      type=Path,
      help="External engine path (alternative to --base for comparing against non-c3 engine)",
  )
  parser.add_argument(
      "--games",
      type=int,
      default=200,
      help="Number of games to play (default: 200)",
  )
  parser.add_argument(
      "--depth",
      type=int,
      default=5,
      help="Fixed search depth for depth mode (default: 5)",
  )
  parser.add_argument(
      "--movetime-ms",
      type=int,
      default=50,
      help="Per-move time for movetime mode in ms (default: 50)",
  )
  parser.add_argument(
      "--mode",
      choices=["depth", "movetime"],
      default="depth",
      help="Time control type (default: depth)",
  )
  parser.add_argument(
      "--concurrency",
      type=int,
      default=4,
      help="Parallel games (default: 4)",
  )
  parser.add_argument(
      "--fastchess",
      default="fastchess",
      help="Path to fastchess executable",
  )
  parser.add_argument(
      "--ci",
      action="store_true",
      help="Enable CI mode (structured output, exit codes)",
  )
  args = parser.parse_args()

  ensure_fastchess_available(args.fastchess)

  timestamp = generate_timestamp()
  pgn_path = FASTCHESS_DIR / f"compare_{timestamp}.pgn"
  log_path = FASTCHESS_DIR / f"compare_{timestamp}.log"
  FASTCHESS_DIR.mkdir(parents=True, exist_ok=True)

  worktrees_to_clean: list[Path] = []

  try:
    with tempfile.TemporaryDirectory() as tmpdir:
      tmp = Path(tmpdir)

      # Build or locate test engine
      if is_git_ref(args.test):
        print(f"Building test engine ({args.test})...")
        test_worktree = tmp / "test" / "src"
        test_engine = checkout_and_build(args.test, tmp / "test")
        worktrees_to_clean.append(test_worktree)
        test_name = "test"
        test_ref = f"{args.test} ({get_commit_hash(args.test)})"
      else:
        test_engine = Path(args.test)
        test_name = test_engine.stem
        test_ref = str(test_engine)

      # Build or locate base engine
      if args.external:
        base_engine = args.external
        base_name = base_engine.stem
        base_ref = str(base_engine)
      elif is_git_ref(args.base):
        print(f"Building base engine ({args.base})...")
        base_worktree = tmp / "base" / "src"
        base_engine = checkout_and_build(args.base, tmp / "base")
        worktrees_to_clean.append(base_worktree)
        base_name = "base"
        base_ref = f"{args.base} ({get_commit_hash(args.base)})"
      else:
        base_engine = Path(args.base)
        base_name = base_engine.stem
        base_ref = str(base_engine)

      # Run comparison
      print(f"Running {args.games} games at depth={args.depth}...")
      cmd = build_comparison_command(
          test_engine, base_engine, test_name, base_name, args, pgn_path
      )
      run(cmd, log_path=log_path)

      # Generate summary
      summary, exit_code = write_comparison_summary(
          pgn_path, test_name, base_name, base_ref, test_ref, args.ci
      )
      print(summary)

      if not args.ci:
        print(f"PGN: {pgn_path}")
        print(f"Log: {log_path}")

      sys.exit(exit_code)

  finally:
    # Cleanup worktrees
    for worktree in worktrees_to_clean:
      cleanup_worktree(worktree)


if __name__ == "__main__":
  main()
