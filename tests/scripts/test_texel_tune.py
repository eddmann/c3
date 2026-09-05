#!/usr/bin/env python3
"""Unit tests for the pure functions behind the Texel tuning skeleton.

Nothing here starts an engine or reads a dataset: the sigmoid, the objective,
the K search, the EPD parser and the coordinate-descent loop are all testable on
synthetic numbers, and testing them that way is the only way to test them at all
given that no labelled dataset ships with the repository.

Run directly with `python3 -m unittest discover -s tests/scripts`, or through
ctest as the `Scripts.TexelTuneUnitTests` case.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from texel_tune import (  # noqa: E402 - path setup has to happen first
    TEMPO_BONUS,
    Parameter,
    Sample,
    find_best_k,
    load_epd,
    mean_squared_error,
    parse_epd_line,
    sigmoid,
    to_white_relative,
    tune,
)


class SigmoidTest(unittest.TestCase):

  def test_an_equal_position_predicts_a_draw(self) -> None:
    self.assertAlmostEqual(sigmoid(0.0, 1.0), 0.5)

  def test_an_advantage_predicts_more_than_a_draw(self) -> None:
    self.assertGreater(sigmoid(200.0, 1.0), 0.5)
    self.assertLess(sigmoid(-200.0, 1.0), 0.5)

  def test_the_curve_is_symmetric_about_zero(self) -> None:
    self.assertAlmostEqual(sigmoid(150.0, 1.0) + sigmoid(-150.0, 1.0), 1.0)

  def test_a_larger_k_makes_the_curve_steeper(self) -> None:
    self.assertGreater(sigmoid(100.0, 2.0), sigmoid(100.0, 1.0))

  def test_a_crushing_score_predicts_a_near_certain_win(self) -> None:
    self.assertGreater(sigmoid(5000.0, 1.0), 0.999)


class ToWhiteRelativeTest(unittest.TestCase):

  def test_strips_the_tempo_bonus_before_flipping(self) -> None:
    self.assertAlmostEqual(to_white_relative(60.0, "w", tempo=10.0), 50.0)
    self.assertAlmostEqual(to_white_relative(60.0, "b", tempo=10.0), -50.0)

  def test_a_dead_equal_position_reads_as_zero_from_both_sides(self) -> None:
    # The engine's own symmetry: a balanced position scores exactly the tempo
    # bonus for whoever is on move, so stripping it must leave zero either way.
    self.assertAlmostEqual(to_white_relative(TEMPO_BONUS, "w"), 0.0)
    self.assertAlmostEqual(to_white_relative(TEMPO_BONUS, "b"), 0.0)

  def test_the_same_position_reads_the_same_from_either_side(self) -> None:
    # eval(pos) + eval(pos with the other side to move) == 2 * TEMPO_BONUS, so a
    # White-to-move reading of x and the matching Black-to-move reading of
    # 2 * TEMPO_BONUS - x have to convert to the same White-relative number.
    white_reading = 137.0
    black_reading = (2.0 * TEMPO_BONUS) - white_reading

    self.assertAlmostEqual(
        to_white_relative(white_reading, "w"), to_white_relative(black_reading, "b"))

  def test_leaving_the_tempo_bonus_in_would_bias_the_side_to_move(self) -> None:
    # The bug this guards against: without the subtraction the two readings above
    # disagree by twice the bonus, a constant bias correlated with whose turn it
    # is rather than with anything on the board.
    self.assertAlmostEqual(
        to_white_relative(137.0, "w", tempo=0.0) - to_white_relative(-117.0, "b", tempo=0.0),
        2.0 * TEMPO_BONUS)


class MeanSquaredErrorTest(unittest.TestCase):

  def test_perfect_predictions_score_zero(self) -> None:
    # sigma(0) is exactly 0.5, so a drawn game predicted as equal is free.
    self.assertAlmostEqual(mean_squared_error([0.5], [0.0], 1.0), 0.0)

  def test_a_confident_wrong_answer_costs_nearly_one(self) -> None:
    # White is said to be winning by 50 pawns and lost the game.
    self.assertGreater(mean_squared_error([0.0], [5000.0], 1.0), 0.99)

  def test_predictions_closer_to_the_result_score_lower(self) -> None:
    close = mean_squared_error([1.0, 0.0], [300.0, -300.0], 1.0)
    backwards = mean_squared_error([1.0, 0.0], [-300.0, 300.0], 1.0)
    self.assertLess(close, backwards)

  def test_mismatched_lengths_are_rejected(self) -> None:
    with self.assertRaises(ValueError):
      mean_squared_error([1.0], [1.0, 2.0], 1.0)

  def test_an_empty_dataset_is_rejected(self) -> None:
    with self.assertRaises(ValueError):
      mean_squared_error([], [], 1.0)


class FindBestKTest(unittest.TestCase):

  def test_recovers_the_k_that_generated_the_data(self) -> None:
    # Build a dataset whose results ARE sigma(0.9 * score); the search should
    # find its way back to 0.9.
    scores = [float(cp) for cp in range(-600, 601, 25)]
    results = [sigmoid(score, 0.9) for score in scores]

    self.assertAlmostEqual(find_best_k(results, scores), 0.9, places=2)

  def test_the_fitted_k_beats_its_neighbours(self) -> None:
    scores = [-400.0, -100.0, 0.0, 100.0, 400.0]
    results = [0.0, 0.5, 0.5, 0.5, 1.0]

    best = find_best_k(results, scores)
    at_best = mean_squared_error(results, scores, best)

    self.assertLess(at_best, mean_squared_error(results, scores, best + 0.2))
    self.assertLess(at_best, mean_squared_error(results, scores, best - 0.2))


class FindBestKBoundsTest(unittest.TestCase):

  @staticmethod
  def _fit_capturing_stderr(results, scores):
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
      best = find_best_k(results, scores)
    return best, stderr.getvalue()

  def test_warns_when_the_minimum_sits_against_a_bound(self) -> None:
    # Every game was drawn from a position the engine thought decisive, so the
    # fit wants K as close to zero as it can get—which is the search's own lower
    # bound, not a minimum it found.
    best, warning = self._fit_capturing_stderr([0.5, 0.5], [400.0, -400.0])

    self.assertLess(best, 0.05)
    self.assertIn("edge of the search range", warning)

  def test_says_nothing_when_the_minimum_is_interior(self) -> None:
    scores = [float(cp) for cp in range(-600, 601, 25)]
    results = [sigmoid(score, 0.9) for score in scores]

    best, warning = self._fit_capturing_stderr(results, scores)

    self.assertAlmostEqual(best, 0.9, places=2)
    self.assertEqual(warning, "")

  def test_the_bracket_matches_what_the_docstring_promises(self) -> None:
    # Twelve passes keeping two thirds each time: (2/3) ** 12 of [0, 4] is about
    # 0.031, so the answer is good to roughly half of that.
    scores = [float(cp) for cp in range(-600, 601, 25)]
    results = [sigmoid(score, 1.7) for score in scores]

    self.assertAlmostEqual(find_best_k(results, scores), 1.7, delta=0.016)


class ParseEpdTest(unittest.TestCase):

  def test_reads_the_c9_opcode_form(self) -> None:
    sample = parse_epd_line('4k3/8/8/8/8/8/4P3/4K3 w - - c9 "1-0";')

    assert sample is not None
    self.assertEqual(sample.fen, "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1")
    self.assertEqual(sample.result, 1.0)

  def test_reads_the_bracketed_comment_form(self) -> None:
    sample = parse_epd_line("4k3/8/8/8/8/8/4P3/4K3 b - - ; [0-1]")

    assert sample is not None
    self.assertEqual(sample.fen, "4k3/8/8/8/8/8/4P3/4K3 b - - 0 1")
    self.assertEqual(sample.result, 0.0)

  def test_reads_a_draw(self) -> None:
    sample = parse_epd_line('8/8/8/4k3/8/4K3/8/8 w - - c9 "1/2-1/2";')

    assert sample is not None
    self.assertEqual(sample.result, 0.5)

  def test_skips_blank_lines_comments_and_unlabelled_positions(self) -> None:
    self.assertIsNone(parse_epd_line(""))
    self.assertIsNone(parse_epd_line("# a note about the dataset"))
    self.assertIsNone(parse_epd_line("4k3/8/8/8/8/8/4P3/4K3 w - -"))

  def test_load_epd_keeps_only_labelled_positions(self) -> None:
    text = "\n".join([
        "# header",
        '4k3/8/8/8/8/8/4P3/4K3 w - - c9 "1-0";',
        "4k3/8/8/8/8/8/4P3/4K3 w - -",
        '8/8/8/4k3/8/4K3/8/8 w - - c9 "1/2-1/2";',
    ])

    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "positions.epd"
      path.write_text(text, encoding="utf-8")
      samples = load_epd(path)

    self.assertEqual([sample.result for sample in samples], [1.0, 0.5])


class TuneTest(unittest.TestCase):
  """Coordinate descent, driven by a scorer that DOES respond to parameters.

  The engine cannot yet be told to use different evaluation constants, so the
  optimiser is exercised here against a stand-in: a scorer that says "White is
  ahead by `weight` centipawns for every white pawn on the board". The optimiser
  has no idea it is not talking to a chess engine, which is the point—it only
  ever sees an error that goes up or down.
  """

  @staticmethod
  def _scorer(fens, params):
    weight = params[0].value
    return [float(fen.split()[0].count("P")) * weight for fen in fens]

  def test_moves_a_parameter_towards_a_lower_error(self) -> None:
    # Two one-pawn positions that White won, and one that Black won: the data
    # says a pawn is worth something, so the weight should climb from zero.
    samples = [
        Sample("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1", 1.0),
        Sample("4k3/8/8/8/8/8/3P4/4K3 w - - 0 1", 1.0),
        Sample("4k3/8/8/8/8/8/4P3/4K3 b - - 0 1", 1.0),
    ]
    params = [Parameter(name="PawnValue", value=0.0, step=25.0)]

    tuned, error = tune(samples, params, self._scorer, k=1.0)

    self.assertGreater(tuned[0].value, 0.0)
    self.assertLess(error, mean_squared_error([s.result for s in samples], [0.0, 0.0, 0.0], 1.0))

  def test_stops_when_no_single_step_helps(self) -> None:
    # A dataset that is already perfectly predicted: sigma(0) = 0.5 for a drawn
    # game, so every step away from zero makes the fit worse.
    samples = [Sample("4k3/8/8/8/8/8/8/4K3 w - - 0 1", 0.5)]
    params = [Parameter(name="PawnValue", value=0.0, step=25.0)]

    tuned, error = tune(samples, params, self._scorer, k=1.0)

    self.assertEqual(tuned[0].value, 0.0)
    self.assertAlmostEqual(error, 0.0)


if __name__ == "__main__":
  unittest.main()
