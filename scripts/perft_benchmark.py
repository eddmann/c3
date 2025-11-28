#!/usr/bin/env python3
"""
Benchmark perft performance between two git branches to detect regressions.

Runs perft on standard positions and compares NPS (nodes per second)
between base and test versions.

Examples:
  # Compare HEAD against main branch
  python3 scripts/perft_benchmark.py --base main --test HEAD

  # With threshold
  python3 scripts/perft_benchmark.py --base main --test HEAD --threshold 5.0

  # CI mode (structured output)
  python3 scripts/perft_benchmark.py --base main --test HEAD --ci
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from common import (
    ROOT,
    build_engine,
)


# Standard benchmark positions: (name, fen, depth)
BENCHMARK_POSITIONS = [
    (
        "startpos",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        5,
    ),
    (
        "kiwipete",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        4,
    ),
    (
        "tricky",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        5,
    ),
]


@dataclass
class PerftResult:
  """Result of a single perft run."""

  name: str
  depth: int
  nodes: int
  time_ms: int
  nps: int


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
  """Checkout ref to worktree and build release binary."""
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


def run_perft(engine_path: Path, name: str, fen: str, depth: int) -> PerftResult:
  """Run perft via UCI and parse results.

  The engine outputs:
    nodes: <count>
    time: <ms> ms
    nps: <nps>
  """
  commands = f"position fen {fen}\nperft {depth}\nquit\n"

  result = subprocess.run(
      [str(engine_path)],
      input=commands,
      capture_output=True,
      text=True,
      timeout=300,
  )

  output = result.stdout

  nodes_match = re.search(r"nodes:\s*(\d+)", output)
  time_match = re.search(r"time:\s*(\d+)", output)
  nps_match = re.search(r"nps:\s*(\d+)", output)

  if not all([nodes_match, time_match, nps_match]):
    raise RuntimeError(f"Failed to parse perft output: {output}")

  return PerftResult(
      name=name,
      depth=depth,
      nodes=int(nodes_match.group(1)),
      time_ms=int(time_match.group(1)),
      nps=int(nps_match.group(1)),
  )


def benchmark_engine(engine_path: Path, positions: list[tuple[str, str, int]]) -> list[PerftResult]:
  """Run full benchmark suite on engine."""
  results = []
  for name, fen, depth in positions:
    result = run_perft(engine_path, name, fen, depth)
    results.append(result)
    nps_str = format_nps(result.nps)
    print(f"  {name} (d{depth}): {nps_str} ({result.time_ms}ms)")
  return results


def format_nps(nps: int) -> str:
  """Format NPS with appropriate suffix."""
  if nps >= 1_000_000:
    return f"{nps / 1_000_000:.1f}M NPS"
  elif nps >= 1_000:
    return f"{nps / 1_000:.1f}K NPS"
  else:
    return f"{nps} NPS"


def compare_results(
    base_results: list[PerftResult],
    test_results: list[PerftResult],
    threshold: float,
) -> tuple[bool, list[tuple[str, float]]]:
  """Compare results and return (passed, diffs).

  Args:
    base_results: Results from base engine.
    test_results: Results from test engine.
    threshold: Regression threshold percentage.

  Returns:
    Tuple of (passed, list of (name, diff_pct)).
  """
  passed = True
  diffs: list[tuple[str, float]] = []

  for base, test in zip(base_results, test_results):
    if base.nps == 0:
      continue
    diff_pct = ((test.nps - base.nps) / base.nps) * 100
    diffs.append((base.name, diff_pct))
    if diff_pct < -threshold:
      passed = False

  return passed, diffs


def write_summary(
    base_ref: str,
    test_ref: str,
    base_results: list[PerftResult],
    test_results: list[PerftResult],
    threshold: float,
    ci_mode: bool = False,
) -> tuple[str, int]:
  """Write benchmark summary and return (summary_text, exit_code)."""
  passed, diffs = compare_results(base_results, test_results, threshold)

  if ci_mode:
    lines = [
        f"base={base_ref}",
        f"test={test_ref}",
        f"threshold={threshold}",
    ]
    for base, test, (name, diff_pct) in zip(base_results, test_results, diffs):
      status = "pass" if diff_pct >= -threshold else "regression"
      lines.append(f"{name}_base_nps={base.nps}")
      lines.append(f"{name}_test_nps={test.nps}")
      lines.append(f"{name}_diff={diff_pct:+.1f}")
      lines.append(f"{name}_status={status}")
    lines.append(f"result={'pass' if passed else 'regression'}")
    return "\n".join(lines), 0 if passed else 1

  # Human-readable format
  lines = [
      "=== Perft Benchmark ===",
      f"Base: {base_ref}",
      f"Test: {test_ref}",
      f"Threshold: {threshold}%",
      "",
      f"{'Position':<18} {'Base NPS':>12} {'Test NPS':>12} {'Diff':>10}",
      "-" * 54,
  ]

  for base, test, (name, diff_pct) in zip(base_results, test_results, diffs):
    status = "ok" if diff_pct >= -threshold else "REGRESSION"
    base_nps_str = format_nps(base.nps).rjust(12)
    test_nps_str = format_nps(test.nps).rjust(12)
    lines.append(
        f"{name} (d{base.depth}){'':<{12-len(name)}} {base_nps_str} {test_nps_str} {diff_pct:>+8.1f}% {status}"
    )

  lines.append("")
  if passed:
    lines.append("Result: No regressions detected")
  else:
    lines.append(f"Result: REGRESSION detected (>{threshold}% slower)")

  return "\n".join(lines), 0 if passed else 1


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
      "--threshold",
      type=float,
      default=5.0,
      help="Regression threshold percentage (default: 5.0)",
  )
  parser.add_argument(
      "--ci",
      action="store_true",
      help="Enable CI mode (structured output)",
  )
  args = parser.parse_args()

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
        test_ref = f"{args.test} ({get_commit_hash(args.test)})"
      else:
        test_engine = Path(args.test)
        test_ref = str(test_engine)

      # Build or locate base engine
      if is_git_ref(args.base):
        print(f"Building base engine ({args.base})...")
        base_worktree = tmp / "base" / "src"
        base_engine = checkout_and_build(args.base, tmp / "base")
        worktrees_to_clean.append(base_worktree)
        base_ref = f"{args.base} ({get_commit_hash(args.base)})"
      else:
        base_engine = Path(args.base)
        base_ref = str(base_engine)

      # Run benchmarks
      print(f"\nBenchmarking base ({base_ref})...")
      base_results = benchmark_engine(base_engine, BENCHMARK_POSITIONS)

      print(f"\nBenchmarking test ({test_ref})...")
      test_results = benchmark_engine(test_engine, BENCHMARK_POSITIONS)

      # Generate summary
      print()
      summary, exit_code = write_summary(
          base_ref, test_ref, base_results, test_results, args.threshold, args.ci
      )
      print(summary)

      sys.exit(exit_code)

  finally:
    # Cleanup worktrees
    for worktree in worktrees_to_clean:
      cleanup_worktree(worktree)


if __name__ == "__main__":
  main()
