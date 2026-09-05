#!/usr/bin/env python3
"""
Compare engine strength between two git branches with a real SPRT self-play run.

Builds both base and test versions, then hands fastchess a sequential
probability ratio test: it keeps playing until the log-likelihood ratio (LLR)
crosses one of Wald's bounds, or until --max-games games have been played.

The hypotheses are expressed in Elo:
  * H0 (--elo0, default 0): the test engine is elo0 stronger, i.e. no gain.
  * H1 (--elo1, default 5): the test engine is at least elo1 stronger.
With alpha = beta = 0.05 the LLR bounds are (-2.94, +2.94); crossing the upper
bound accepts H1, crossing the lower bound accepts H0.

Exit codes:
  0  H1 accepted (test is stronger)
  1  H0 accepted (test is not stronger)
  2  inconclusive when the game cap was reached
  3  error (build failure, no games played, fastchess missing)

Examples:
  # SPRT HEAD against main branch
  python3 scripts/compare_branches.py --base main --test HEAD

  # Compare against external engine
  python3 scripts/compare_branches.py --test HEAD --external /path/to/stockfish

  # With options
  python3 scripts/compare_branches.py --base main --test HEAD \\
    --max-games 2000 --elo0 0 --elo1 5 --depth 5 --concurrency 4

  # CI mode (structured output)
  python3 scripts/compare_branches.py --base main --test HEAD --ci
"""

from __future__ import annotations

import argparse
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from common import (
    FASTCHESS_DIR,
    generate_timestamp,
    run,
    elo_from_score,
    elo_error,
    llr,
    los,
    sprt_bounds,
    ensure_fastchess_available,
    is_git_ref,
    get_commit_hash,
    checkout_and_build,
    cleanup_worktree,
    parse_pgn_results,
)

# Error rates of the test. Fixed rather than exposed as flags: they belong to
# the testing policy, not to an individual run.
ALPHA = 0.05
BETA = 0.05

# fastchess prints one of these lines after every completed round, e.g.
#   LLR: 1.16 (-2.94, 2.94) [0.00, 5.00]
# and appends the verdict once a bound is crossed.
LLR_PATTERN = re.compile(r"LLR:\s*(-?\d+(?:\.\d+)?)")
HYPOTHESIS_PATTERN = re.compile(r"(H[01])\s+was\s+accepted")


def build_comparison_command(
    engine_a: Path,
    engine_b: Path,
    name_a: str,
    name_b: str,
    args: argparse.Namespace,
    pgn_path: Path,
) -> list[str]:
  """Build the fastchess SPRT command for a head-to-head comparison."""
  time_conf = (
      [f"depth={args.depth}"]
      if args.mode == "depth"
      else [f"st={args.movetime_ms / 1000:.3f}"]
  )

  # Games are played in pairs (same opening with colours reversed), which is
  # what fastchess's SPRT statistics expect, so the cap is halved into rounds.
  rounds = max(1, math.ceil(args.max_games / 2))

  cmd = [
      args.fastchess,
      "-tournament", "gauntlet",
      "-seeds", "1",
      "-engine", f"cmd={engine_a}", f"name={name_a}",
      "-engine", f"cmd={engine_b}", f"name={name_b}",
      "-rounds", str(rounds),
      "-games", "2",
      "-repeat",
      "-sprt",
      f"elo0={args.elo0}", f"elo1={args.elo1}", f"alpha={ALPHA}", f"beta={BETA}",
      "model=normalized",
      "-concurrency", str(args.concurrency),
      "-pgnout", f"file={pgn_path}",
      "-each", "proto=uci", *time_conf,
      "-recover",
  ]

  # Add opening book if specified
  if args.openings and args.openings.exists():
    openings_abs = args.openings.resolve()
    cmd.extend(["-openings", f"file={openings_abs}", "format=epd", "order=random"])

  return cmd


def parse_fastchess_sprt(log_text: str) -> tuple[float | None, str | None]:
  """Pull the last reported LLR and verdict out of a fastchess log.

  Returns (llr, hypothesis) where either may be None if fastchess did not print
  it (for example when the run was cut short).
  """
  values = LLR_PATTERN.findall(log_text)
  verdicts = HYPOTHESIS_PATTERN.findall(log_text)
  return (float(values[-1]) if values else None), (verdicts[-1] if verdicts else None)


def classify_sprt(
    ratio: float,
    lower: float,
    upper: float,
    hypothesis: str | None,
) -> tuple[str, int]:
  """Turn an LLR into a human verdict and an exit code."""
  if hypothesis == "H1" or ratio >= upper:
    return "H1 accepted: test is stronger", 0
  if hypothesis == "H0" or ratio <= lower:
    return "H0 accepted: test is not stronger", 1
  return "Inconclusive: game cap reached before a bound was crossed", 2


def write_comparison_summary(
    pgn_path: Path,
    log_path: Path,
    name_test: str,
    name_base: str,
    base_ref: str,
    test_ref: str,
    elo0: float,
    elo1: float,
    ci_mode: bool = False,
) -> tuple[str, int]:
  """Write comparison summary and return (summary_text, exit_code)."""
  wins_test, wins_base, draws = parse_pgn_results(pgn_path, name_test, name_base)
  games = wins_test + wins_base + draws

  if games == 0:
    return "No games completed.", 3

  score = (wins_test + 0.5 * draws) / games
  elo = elo_from_score(score)
  err = elo_error(wins_test, draws, wins_base)
  likelihood = los(wins_test, draws, wins_base)
  lower, upper = sprt_bounds(ALPHA, BETA)

  # Prefer fastchess's own running LLR; recompute it from the PGN when the log
  # is unavailable (e.g. an interrupted run that still produced games).
  reported, hypothesis = parse_fastchess_sprt(
      log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
  )
  ratio = reported if reported is not None else llr(wins_test, draws, wins_base, elo0, elo1)
  result, exit_code = classify_sprt(ratio, lower, upper, hypothesis)

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
        f"elo0={elo0}\n"
        f"elo1={elo1}\n"
        f"llr={ratio:.2f}\n"
        f"llr_bounds={lower:.2f},{upper:.2f}\n"
        f"result={result}\n"
    )
  else:
    summary = (
        f"=== Branch Comparison (SPRT) ===\n"
        f"Base: {base_ref}\n"
        f"Test: {test_ref}\n"
        f"\n"
        f"Games: {games}\n"
        f"W/D/L (test vs base): {wins_test}/{draws}/{wins_base}\n"
        f"Score: {score:.3f}\n"
        f"Elo diff: {elo:+.1f} +/- {err:.1f}\n"
        f"LOS: {likelihood * 100:.1f}%\n"
        f"LLR: {ratio:.2f} ({lower:.2f}, {upper:.2f}) [{elo0:.2f}, {elo1:.2f}]\n"
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
      "--max-games",
      "--games",
      dest="max_games",
      type=int,
      default=2000,
      help="Stop the SPRT after this many games if no bound is crossed (default: 2000)",
  )
  parser.add_argument(
      "--elo0",
      type=float,
      default=0.0,
      help="Null hypothesis in Elo: no improvement (default: 0)",
  )
  parser.add_argument(
      "--elo1",
      type=float,
      default=5.0,
      help="Alternative hypothesis in Elo: the gain worth detecting (default: 5)",
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
      "--openings",
      type=Path,
      help="Path to opening book file (EPD format)",
  )
  parser.add_argument(
      "--ci",
      action="store_true",
      help="Enable CI mode (structured output, exit codes)",
  )
  args = parser.parse_args()

  # Validate arguments
  if args.max_games <= 0:
    parser.error("--max-games must be positive")
  if args.elo1 <= args.elo0:
    parser.error("--elo1 must be greater than --elo0")
  if args.depth <= 0:
    parser.error("--depth must be positive")
  if args.concurrency <= 0:
    parser.error("--concurrency must be positive")
  if args.movetime_ms <= 0:
    parser.error("--movetime-ms must be positive")
  if args.external and not args.external.exists():
    parser.error(f"External engine not found: {args.external}")

  try:
    ensure_fastchess_available(args.fastchess)
  except SystemExit as error:
    # Report a missing fastchess as an error (3) rather than a lost test (1).
    print(f"Error: {error}", file=sys.stderr)
    sys.exit(3)

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
      print(
          f"Running SPRT [{args.elo0}, {args.elo1}] at depth={args.depth}, "
          f"up to {args.max_games} games..."
      )
      cmd = build_comparison_command(
          test_engine, base_engine, test_name, base_name, args, pgn_path
      )
      try:
        run(cmd, log_path=log_path)
      except subprocess.CalledProcessError as error:
        # Do not throw away a decided test because the last game crashed: show
        # the log, then summarise whatever games reached the PGN. If none did,
        # write_comparison_summary() reports the error exit code itself.
        print(f"Warning: fastchess exited with code {error.returncode}", file=sys.stderr)
        if log_path.exists():
          print("=== Fastchess log ===")
          print(log_path.read_text())
          print("=== End log ===")

      # Generate summary
      summary, exit_code = write_comparison_summary(
          pgn_path, log_path, test_name, base_name, base_ref, test_ref,
          args.elo0, args.elo1, args.ci,
      )
      print(summary)

      if not args.ci:
        print(f"PGN: {pgn_path}")
        print(f"Log: {log_path}")

      sys.exit(exit_code)

  except Exception as error:  # noqa: BLE001 - report any failure as exit code 3
    print(f"Error: {error}", file=sys.stderr)
    sys.exit(3)
  finally:
    # Cleanup worktrees
    for worktree in worktrees_to_clean:
      cleanup_worktree(worktree)


if __name__ == "__main__":
  main()
