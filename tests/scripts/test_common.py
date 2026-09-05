#!/usr/bin/env python3
"""Unit tests for the statistics helpers behind the gauntlet/SPRT scripts.

Run directly with `python3 -m unittest discover -s tests/scripts`, or through
ctest as the `Scripts.CommonUnitTests` case.
"""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from common import (  # noqa: E402 - path setup has to happen first
    elo_error,
    expected_score,
    llr,
    los,
    score_variance,
    sprt_bounds,
)
from compare_branches import classify_sprt, parse_fastchess_sprt  # noqa: E402


class ExpectedScoreTest(unittest.TestCase):

  def test_zero_elo_is_an_even_score(self) -> None:
    self.assertAlmostEqual(expected_score(0.0), 0.5)

  def test_positive_elo_scores_above_half(self) -> None:
    self.assertGreater(expected_score(5.0), 0.5)
    self.assertAlmostEqual(expected_score(5.0), 0.5071951, places=6)


class ScoreVarianceTest(unittest.TestCase):

  def test_all_draws_have_no_variance(self) -> None:
    self.assertEqual(score_variance(0, 100, 0), 0.0)

  def test_hand_calculated_distribution(self) -> None:
    # 30 wins / 50 draws / 20 losses over 100 games, mean score 0.55:
    #   0.3*(1-0.55)^2 + 0.5*(0.5-0.55)^2 + 0.2*(0-0.55)^2 = 0.1225
    self.assertAlmostEqual(score_variance(30, 50, 20), 0.1225, places=6)

  def test_draws_shrink_the_variance(self) -> None:
    # Same score (0.55) with every game decisive is far noisier.
    self.assertLess(score_variance(30, 50, 20), score_variance(55, 0, 45))


class EloErrorTest(unittest.TestCase):

  def test_hand_calculated_error_bar(self) -> None:
    # deriv = 400 / (ln(10) * 0.55 * 0.45) = 701.9
    # error = deriv * sqrt(0.1225 / 100) = 701.9 * 0.035 = 24.6 Elo
    self.assertAlmostEqual(elo_error(30, 50, 20), 24.57, places=2)

  def test_tighter_than_the_binomial_approximation(self) -> None:
    # The binomial variance p*(1-p)/n ignores draws and reports 34.9 Elo here.
    binomial = 400 / (math.log(10) * 0.55 * 0.45) * math.sqrt(0.55 * 0.45 / 100)
    self.assertAlmostEqual(binomial, 34.92, places=2)
    self.assertLess(elo_error(30, 50, 20), binomial)

  def test_no_games_is_unbounded(self) -> None:
    self.assertEqual(elo_error(0, 0, 0), float("inf"))

  def test_all_draws_still_report_an_error_bar(self) -> None:
    # Nothing was observed about the spread, so the variance is floored at one
    # decisive game shared over the sample:
    #   400 / (ln(10) * 0.25) * sqrt(0.25 / 100 / 100) = 3.47 Elo
    self.assertAlmostEqual(elo_error(0, 100, 0), 3.47, places=2)

  def test_unbeaten_run_is_not_reported_as_certain(self) -> None:
    self.assertGreater(elo_error(100, 0, 0), 100.0)


class LosTest(unittest.TestCase):

  def test_hand_calculated_probability(self) -> None:
    # 30/50/20: mean 0.55, standard error sqrt(0.1225 / 100) = 0.035,
    # z = 0.05 / 0.035 = 1.4286, and Phi(1.4286) = 0.9234.
    self.assertAlmostEqual(los(30, 50, 20), 0.9234, places=4)

  def test_draws_sharpen_the_estimate(self) -> None:
    # Same 0.55 score, but an all-decisive run is noisier and less convincing.
    self.assertGreater(los(30, 50, 20), los(55, 0, 45))

  def test_all_draws_are_a_coin_flip(self) -> None:
    self.assertEqual(los(0, 100, 0), 0.5)

  def test_unbeaten_run_is_superior(self) -> None:
    self.assertEqual(los(100, 0, 0), 1.0)

  def test_no_games_is_a_coin_flip(self) -> None:
    self.assertEqual(los(0, 0, 0), 0.5)


class LlrTest(unittest.TestCase):

  def test_clear_win_favours_h1(self) -> None:
    # 100 wins, 100 draws, no losses: a 0.75 score is well past elo1 = 5.
    self.assertGreater(llr(100, 100, 0, elo0=0.0, elo1=5.0), sprt_bounds()[1])

  def test_clear_win_matches_hand_calculation(self) -> None:
    # N = 200, mean = 0.75, var = 0.5*(0.25)^2 + 0.5*(0.25)^2 = 0.0625,
    # s0 = 0.5 and s1 = 0.5071951, so
    #   LLR = 200 * (s1 - s0) * (2*0.75 - s0 - s1) / (2 * 0.0625) = 5.6732
    self.assertAlmostEqual(llr(100, 100, 0, elo0=0.0, elo1=5.0), 5.6732, places=3)

  def test_equal_wins_and_losses_is_neutral(self) -> None:
    # Exactly on H0, so the ratio sits at (just below) zero.
    self.assertAlmostEqual(llr(50, 100, 50, elo0=0.0, elo1=5.0), 0.0, places=1)
    self.assertLess(llr(50, 100, 50, elo0=0.0, elo1=5.0), 0.0)

  def test_losing_result_favours_h0(self) -> None:
    self.assertLess(llr(20, 100, 80, elo0=0.0, elo1=5.0), 0.0)

  def test_no_games_yields_no_evidence(self) -> None:
    self.assertEqual(llr(0, 0, 0, elo0=0.0, elo1=5.0), 0.0)

  def test_all_draws_never_accept_h0(self) -> None:
    # A drawn run says nothing about a 5 Elo difference. With a fallback
    # variance that shrank with the sample the ratio grew as N^2, so 200
    # straight draws crossed the H0 bound (-4.14) and 1000 reached -103.
    self.assertGreater(llr(0, 200, 0, elo0=0.0, elo1=5.0), sprt_bounds()[0])
    self.assertGreater(llr(0, 1000, 0, elo0=0.0, elo1=5.0), sprt_bounds()[0])

  def test_degenerate_evidence_grows_linearly(self) -> None:
    self.assertAlmostEqual(
        llr(0, 1000, 0, elo0=0.0, elo1=5.0),
        5 * llr(0, 200, 0, elo0=0.0, elo1=5.0),
        places=9,
    )

  def test_evidence_accumulates_with_games(self) -> None:
    few = llr(30, 50, 20, elo0=0.0, elo1=5.0)
    many = llr(300, 500, 200, elo0=0.0, elo1=5.0)
    self.assertAlmostEqual(many, 10 * few, places=6)


class SprtBoundsTest(unittest.TestCase):

  def test_symmetric_error_rates(self) -> None:
    lower, upper = sprt_bounds(0.05, 0.05)
    self.assertAlmostEqual(lower, -2.9444, places=4)
    self.assertAlmostEqual(upper, 2.9444, places=4)


class FastchessOutputTest(unittest.TestCase):

  LOG = (
      "Started game 1 of 200\n"
      "LLR: 0.42 (-2.94, 2.94) [0.00, 5.00]\n"
      "Started game 2 of 200\n"
      "LLR: 2.98 (-2.94, 2.94) [0.00, 5.00]\n"
      "H1 was accepted\n"
  )

  def test_reads_the_latest_llr_and_verdict(self) -> None:
    self.assertEqual(parse_fastchess_sprt(self.LOG), (2.98, "H1"))

  def test_missing_output_is_reported_as_unknown(self) -> None:
    self.assertEqual(parse_fastchess_sprt("no games were played\n"), (None, None))

  def test_negative_llr_is_parsed(self) -> None:
    ratio, hypothesis = parse_fastchess_sprt("LLR: -3.01 (-2.94, 2.94) [0.00, 5.00]\n")
    self.assertAlmostEqual(ratio, -3.01)
    self.assertIsNone(hypothesis)


class ClassifySprtTest(unittest.TestCase):

  def setUp(self) -> None:
    self.lower, self.upper = sprt_bounds()

  def test_upper_bound_accepts_h1(self) -> None:
    self.assertEqual(classify_sprt(3.0, self.lower, self.upper, None)[1], 0)

  def test_lower_bound_accepts_h0(self) -> None:
    self.assertEqual(classify_sprt(-3.0, self.lower, self.upper, None)[1], 1)

  def test_between_bounds_is_inconclusive(self) -> None:
    self.assertEqual(classify_sprt(0.5, self.lower, self.upper, None)[1], 2)

  def test_fastchess_verdict_wins_over_the_raw_ratio(self) -> None:
    self.assertEqual(classify_sprt(0.5, self.lower, self.upper, "H1")[1], 0)
    self.assertEqual(classify_sprt(0.5, self.lower, self.upper, "H0")[1], 1)


if __name__ == "__main__":
  unittest.main()
