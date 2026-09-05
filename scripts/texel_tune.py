#!/usr/bin/env python3
"""Texel tuning skeleton for the c3 evaluation.

WHAT TEXEL TUNING IS
--------------------
Every number in include/c3/eval.hpp (the passed-pawn table, the king-shield
bonus, the mobility weights, ...) was picked by hand from published tables and
chess intuition. Texel tuning, named after the engine whose author described it,
replaces the intuition with arithmetic: take a large set of positions whose
GAME RESULT is known, and choose the numbers that make the evaluation's opinion
of those positions agree with how the games actually finished.

The trick is turning a centipawn score into something comparable with a result.
A game result is 1.0, 0.5 or 0.0; an evaluation is an unbounded integer. The
logistic (sigmoid) function bridges them:

    sigma(K * score) = 1 / (1 + 10 ** (-K * score / 400))

which maps a score in centipawns onto an expected score between 0 and 1, using
the same 400-point scale as the Elo formula. K is a single scaling constant that
says how confidently this particular engine's centipawns predict results, and it
is fitted first, before anything else is tuned.

The objective is then the mean squared error between prediction and outcome:

    E = (1 / N) * sum over positions of (result - sigma(K * score)) ** 2

Tuning is just minimising E over the evaluation's parameters. This script
implements the minimisation as COORDINATE DESCENT: take one parameter, try it a
step up and a step down, keep whichever lowered E, move to the next parameter,
and repeat until a whole sweep produces no improvement. It is the slowest
respectable optimiser there is, and it is what almost every hobby engine uses,
because it needs no gradients and no libraries.

WHAT THIS SCRIPT ACTUALLY DOES, AND WHAT IT DOES NOT
----------------------------------------------------
NO LABELLED DATASET SHIPS WITH THIS REPOSITORY. Texel tuning needs hundreds of
thousands of quiet positions with game results attached, which is far more data
than belongs in a source tree, and generating it means self-play games this
environment cannot run. You have to bring your own EPD file. Everything below
is written so that the machinery can be read, understood and unit-tested
without one.

More importantly: THE C++ CONSTANTS ARE NOT TUNABLE FROM OUTSIDE THE BINARY
YET. They are `inline constexpr` values compiled into the engine, so this
script cannot change them between evaluations. The coordinate-descent loop here
is therefore ILLUSTRATIVE: it optimises a Python dictionary of parameters
against a scoring function you hand it, which is exactly the shape the real
thing takes, but the only scoring function that exists today is one that ignores
the parameters (`engine_scorer`) or one you write yourself for a test.

Wiring it up for real is a small, well-understood change to the engine, and it
is worth spelling out because it is the natural next step:

  1. Move the tunable constants out of `inline constexpr` into a struct of
     mutable globals (`EvalParams`), with the current values as defaults.
  2. Register each field as a UCI option in src/uci.cpp, so a GUI or a script
     can send `setoption name PassedPawnMg5 value 62`.
  3. Replace `engine_scorer` below with one that sends those `setoption` lines
     before asking for evaluations, so a candidate parameter set can be scored
     without recompiling.

Steps 1 and 2 make the engine marginally slower (a global read instead of a
compile-time constant) and are usually put behind a build flag for that reason.

INPUT FORMATS
-------------
EPD, one position per line, with the result attached in either of the two forms
that are common in the wild:

    4k3/8/8/8/8/8/4P3/4K3 w - - c9 "1-0";
    4k3/8/8/8/8/8/4P3/4K3 w - - ; [1-0]

Results may be written `1-0`, `0-1`, `1/2-1/2`, or directly as `1.0`/`0.5`/`0.0`.
They are always read from WHITE's point of view, and so are the scores this
script works with. Two things happen to each reading on the way in: the engine's
tempo bonus is subtracted (it is paid to whoever is on move, after the
side-to-move flip, so it does not negate with the rest of the score), and the
result is negated for a position with Black to move. See to_white_relative.

A CSV of precomputed evaluations (`fen,score`) can be supplied instead with
--evals, for fitting K without an engine to hand. Those numbers are fixed, so
they cannot be used with --tune: the sweep needs scores that respond to the
parameters it is changing.

WHAT TO LEAVE OUT OF A TUNING SET. Positions the engine short-circuits carry no
information about any weight and only dilute the objective. The two to watch for
are dead draws (the engine returns a flat zero whenever neither side can mate,
whatever else is on the board) and positions where a capture is hanging—Texel
tuning conventionally uses QUIET positions, scored by a quiescence search,
because a static evaluation has nothing sensible to say about a position with a
queen en prise.

USAGE
-----
    # Fit K only, using the engine to evaluate each position:
    python3 scripts/texel_tune.py --epd quiet.epd --engine build-release/c3

    # Fit K from a CSV of precomputed scores, no engine needed:
    python3 scripts/texel_tune.py --epd quiet.epd --evals scores.csv

    # Fit K and then run the illustrative sweep:
    python3 scripts/texel_tune.py --epd quiet.epd --engine build-release/c3 \\
        --params params.json --tune

`params.json` is a list of tunable parameters, each with a name, a starting
value and a step size:

    [{"name": "PassedPawnMg5", "value": 60, "step": 8},
     {"name": "KingShieldMg",  "value": 10, "step": 4}]
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence

ROOT = Path(__file__).resolve().parent.parent

# The Elo scale's denominator. Sharing it with the sigmoid is what makes K
# interpretable: it is "how many of this engine's centipawns are worth one
# rating point's worth of expected score".
ELO_SCALE = 400.0

# TEMPO_BONUS in include/c3/eval.hpp: the engine pays this to whoever is on move,
# after the side-to-move flip, so it has to come off before a reading can be
# turned into a White-relative score. Override it with --tempo if the engine's
# value ever changes; this script has no way to ask the binary what it is.
TEMPO_BONUS = 10.0

# The result of a finished game, from White's point of view.
RESULT_TOKENS = {
    "1-0": 1.0,
    "0-1": 0.0,
    "1/2-1/2": 0.5,
    "1/2": 0.5,
    "0.5": 0.5,
    "1.0": 1.0,
    "0.0": 0.0,
    "1": 1.0,
    "0": 0.0,
}


@dataclass(frozen=True)
class Sample:
  """One labelled position: a FEN and the result of the game it came from."""

  fen: str
  result: float  # 1.0 White won, 0.5 drawn, 0.0 Black won


@dataclass
class Parameter:
  """One tunable evaluation number, as coordinate descent sees it."""

  name: str
  value: float
  step: float


def sigmoid(score: float, k: float) -> float:
  """Map a centipawn score onto an expected game score in [0, 1].

  A score of zero always predicts a draw, whatever K is. Larger K makes the
  curve steeper, meaning "this engine's small advantages already decide games";
  smaller K flattens it. Fitting K before tuning anything else is what stops the
  optimiser from trying to fix a badly scaled prediction by inflating every
  weight in the evaluation.
  """
  return 1.0 / (1.0 + math.pow(10.0, -k * score / ELO_SCALE))


def mean_squared_error(results: Sequence[float], scores: Sequence[float], k: float) -> float:
  """The Texel objective: how badly sigma(K * score) predicts the results.

  Squared error rather than absolute error because it punishes confident wrong
  answers much harder than uncertain ones, which is exactly the behaviour an
  evaluation should be pushed towards.
  """
  if len(results) != len(scores):
    raise ValueError("results and scores must be the same length")
  if not results:
    raise ValueError("cannot compute an error over no positions")

  total = 0.0
  for result, score in zip(results, scores):
    error = result - sigmoid(score, k)
    total += error * error
  return total / len(results)


def find_best_k(
    results: Sequence[float],
    scores: Sequence[float],
    low: float = 0.0,
    high: float = 4.0,
    iterations: int = 12,
) -> float:
  """Fit K by TERNARY SEARCH: repeatedly discard the third that cannot hold it.

  The error curve in K is smooth and has a single minimum, so comparing the
  error at the two points that cut the interval into thirds is enough to say
  which outer third the minimum is not in—no derivatives needed. Each pass keeps
  two thirds of the interval, so `iterations` passes shrink it by (2/3) **
  iterations: twelve passes take [0, 4] down to a bracket of about 0.031, and
  the midpoint of that bracket is what comes back. That is coarse next to the
  0.001 you might assume from bisection, and it is fine—K only has to be roughly
  right for the tuning that follows, and asking for another decimal place means
  another six passes over the whole dataset.

  A result that lands against either end of [low, high] means the minimum is
  probably OUTSIDE the interval and the search has simply run into the wall, so
  that case gets a warning rather than a silent answer.
  """
  original_low, original_high = low, high

  for _ in range(iterations):
    third = (high - low) / 3.0
    left = low + third
    right = high - third
    if mean_squared_error(results, scores, left) < mean_squared_error(results, scores, right):
      high = right
    else:
      low = left

  best = (low + high) / 2.0

  # The final bracket is the resolution of the answer, so "within one bracket of
  # an end" is the same statement as "indistinguishable from the boundary".
  bracket = high - low
  if best - original_low <= bracket or original_high - best <= bracket:
    print(
        f"warning: fitted K = {best:.4f} sits at the edge of the search range "
        f"[{original_low:g}, {original_high:g}]; the real minimum is probably outside it, "
        f"so widen the range or check that the scores are White-relative centipawns",
        file=sys.stderr)

  return best


def parse_epd_line(line: str) -> Sample | None:
  """Read one EPD line into a Sample, or None if it carries no result.

  Two labelling conventions are accepted: the `c9 "1-0";` opcode written by most
  position-extraction tools, and a bare `[1-0]` comment. Blank lines and lines
  starting with `#` are skipped.
  """
  text = line.strip()
  if not text or text.startswith("#"):
    return None

  result = _extract_result(text)
  if result is None:
    return None

  # The FEN is everything before the first EPD opcode or comment. EPD omits the
  # halfmove and fullmove counters, so pad them: the engine wants a full FEN and
  # neither counter changes a static evaluation.
  head = text.split(";")[0].split("[")[0].split(" c9 ")[0].strip()
  fields = head.split()
  if len(fields) < 4:
    return None
  fen = " ".join(fields[:4]) + " 0 1"

  return Sample(fen=fen, result=result)


def _extract_result(text: str) -> float | None:
  """Pull the game result out of an EPD line, whichever way it was written."""
  for opener, closer in (('c9 "', '"'), ("[", "]")):
    start = text.find(opener)
    if start == -1:
      continue
    start += len(opener)
    end = text.find(closer, start)
    if end == -1:
      continue
    token = text[start:end].strip()
    if token in RESULT_TOKENS:
      return RESULT_TOKENS[token]
  return None


def load_epd(path: Path) -> list[Sample]:
  """Read every labelled position out of an EPD file."""
  samples = []
  for line in path.read_text(encoding="utf-8").splitlines():
    sample = parse_epd_line(line)
    if sample is not None:
      samples.append(sample)
  return samples


def load_eval_csv(path: Path) -> dict[str, float]:
  """Read a `fen,score` CSV of precomputed White-relative evaluations."""
  scores: dict[str, float] = {}
  with path.open(newline="", encoding="utf-8") as handle:
    for row in csv.reader(handle):
      if len(row) < 2 or row[0].startswith("#"):
        continue
      scores[row[0].strip()] = float(row[1])
  return scores


def to_white_relative(score: float, side_to_move: str, tempo: float = TEMPO_BONUS) -> float:
  """Turn one `eval` reading into a White-relative score.

  Two corrections, in this order, and the order matters.

  FIRST, remove the tempo bonus. The engine pays it to whoever is on move, so it
  is added AFTER the side-to-move flip and does not negate with the rest of the
  score. Leaving it in would make every White-to-move position look `tempo`
  centipawns better than it is and every Black-to-move position `tempo` worse—a
  constant bias correlated with whose turn it is, which is exactly the sort of
  thing a fitted K will happily absorb and then quietly distort everything else
  to accommodate.

  SECOND, flip the sign for Black. `eval` reports from the side to move; EPD
  results are from White.
  """
  white_relative = score - tempo
  return white_relative if side_to_move == "w" else -white_relative


def engine_scorer(
    engine: Path,
    tempo: float = TEMPO_BONUS,
) -> Callable[[Iterable[str], Sequence[Parameter]], list[float]]:
  """Score positions by asking the engine binary, one batch per call.

  BEWARE OF DEAD DRAWS. The engine short-circuits positions where neither side
  has enough material to mate and returns a flat zero for them, tempo bonus and
  all. Those positions carry no information about any evaluation weight—the
  score does not depend on a single one of them—so they only dilute the
  objective. A tuning set should have them filtered out before it gets here;
  this scorer cannot tell them apart from genuinely equal positions.

  The `params` argument is accepted and ignored, and that is the honest state of
  affairs today: the evaluation's constants are compiled in, so no parameter
  set can change what this returns. See the module docstring for the three steps
  that would make it real.
  """

  def score(fens: Iterable[str], params: Sequence[Parameter]) -> list[float]:
    del params  # Not yet settable from outside the binary.

    fen_list = list(fens)
    commands = "".join(f"position fen {fen}\neval\n" for fen in fen_list) + "quit\n"
    completed = subprocess.run(
        [str(engine)], input=commands, capture_output=True, text=True, check=True)

    scores = [
        float(line.split("eval:")[1]) for line in completed.stdout.splitlines() if "eval:" in line
    ]
    if len(scores) != len(fen_list):
      raise RuntimeError(f"engine returned {len(scores)} scores for {len(fen_list)} positions")

    return [
        to_white_relative(value, fen.split()[1], tempo)
        for fen, value in zip(fen_list, scores)
    ]

  return score


def tune(
    samples: Sequence[Sample],
    params: list[Parameter],
    scorer: Callable[[Iterable[str], Sequence[Parameter]], list[float]],
    k: float,
    max_sweeps: int = 10,
) -> tuple[list[Parameter], float]:
  """Coordinate descent over `params`, minimising the Texel objective.

  One sweep tries every parameter a step up and a step down, keeping any change
  that lowers the error. Sweeps repeat until one of them changes nothing, which
  is the standard stopping rule: at that point no single parameter can be moved
  by one step in either direction without making the fit worse.

  This is the illustrative half of the script. With the scorer that talks to the
  engine binary the error never moves, because the engine cannot yet be told to
  use different numbers, and the first sweep therefore ends immediately. Hand it
  a scorer that does respond to `params` (as the unit tests do) and it converges
  on the parameters that fit the data.
  """
  fens = [sample.fen for sample in samples]
  results = [sample.result for sample in samples]

  best_error = mean_squared_error(results, scorer(fens, params), k)

  for _ in range(max_sweeps):
    improved = False

    for parameter in params:
      original = parameter.value

      for candidate in (original + parameter.step, original - parameter.step):
        parameter.value = candidate
        error = mean_squared_error(results, scorer(fens, params), k)
        if error < best_error:
          best_error = error
          improved = True
          break
        parameter.value = original

    if not improved:
      break

  return params, best_error


def load_params(path: Path) -> list[Parameter]:
  """Read the JSON list of tunable parameters."""
  entries = json.loads(path.read_text(encoding="utf-8"))
  return [
      Parameter(name=entry["name"], value=float(entry["value"]), step=float(entry["step"]))
      for entry in entries
  ]


def main() -> int:
  parser = argparse.ArgumentParser(
      description="Texel tuning skeleton for the c3 evaluation.",
      epilog="No labelled dataset ships with this repository; bring your own EPD file.")
  parser.add_argument("--epd", type=Path, required=True,
                      help="EPD file of positions labelled with game results")
  parser.add_argument("--engine", type=Path, default=ROOT / "build-release" / "c3",
                      help="engine binary to ask for evaluations")
  parser.add_argument("--evals", type=Path,
                      help="CSV of precomputed 'fen,score' pairs, used instead of the engine")
  parser.add_argument("--params", type=Path,
                      help="JSON list of tunable parameters (see the module docstring)")
  parser.add_argument("--tune", action="store_true",
                      help="run the illustrative coordinate-descent sweep after fitting K")
  parser.add_argument("--tempo", type=float, default=TEMPO_BONUS,
                      help="the engine's tempo bonus, removed from each reading "
                           f"(default {TEMPO_BONUS:g}, matching TEMPO_BONUS in eval.hpp)")
  args = parser.parse_args()

  # A CSV of precomputed scores cannot respond to a change in the parameters, so
  # a sweep over it would try every candidate against identical numbers, find no
  # improvement anywhere and stop after one pass. That is a confusing way to
  # report "this combination makes no sense", so refuse it outright.
  if args.tune and args.evals is not None:
    print("--tune cannot be used with --evals: precomputed scores cannot respond to a change "
          "in the parameters", file=sys.stderr)
    return 1

  samples = load_epd(args.epd)
  if not samples:
    print(f"no labelled positions found in {args.epd}", file=sys.stderr)
    return 1

  if args.evals is not None:
    table = load_eval_csv(args.evals)
    missing = [sample.fen for sample in samples if sample.fen not in table]
    if missing:
      print(f"{len(missing)} positions are missing from {args.evals}", file=sys.stderr)
      return 1

    def scorer(fens: Iterable[str], params: Sequence[Parameter]) -> list[float]:
      del params
      return [table[fen] for fen in fens]
  else:
    if not args.engine.exists():
      print(f"engine binary not found: {args.engine}", file=sys.stderr)
      return 1
    scorer = engine_scorer(args.engine, args.tempo)

  params = load_params(args.params) if args.params else []

  fens = [sample.fen for sample in samples]
  results = [sample.result for sample in samples]
  scores = scorer(fens, params)

  best_k = find_best_k(results, scores)
  print(f"positions: {len(samples)}")
  print(f"best K:    {best_k:.4f}")
  print(f"error:     {mean_squared_error(results, scores, best_k):.6f}")

  if args.tune:
    if not params:
      print("--tune needs --params", file=sys.stderr)
      return 1

    tuned, error = tune(samples, params, scorer, best_k)
    print(f"error after tuning: {error:.6f}")
    for parameter in tuned:
      print(f"  {parameter.name} = {parameter.value:g}")
    print("\nNote: the engine's evaluation constants are compiled in, so this sweep")
    print("cannot change what the engine reports. See the module docstring for the")
    print("three steps that would make these parameters settable at run time.")

  return 0


if __name__ == "__main__":
  sys.exit(main())
