#include <gtest/gtest.h>

#include <string_view>

#include "c3/eval.hpp"
#include "c3/position.hpp"
#include "fixtures.hpp"

using namespace c3;

namespace {

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

} // namespace

// Material --------------------------------------------------------------------

TEST(MaterialEval, MoreMaterialIsGood) {
  const auto pos = parse("4kbnr/8/8/8/8/8/4P3/4KBNR w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board), eval_material(Colour::Black, pos.board));
}

TEST(MaterialEval, MinorPiecesAreWorthMoreThanPawns) {
  const auto white_knight_black_pawn = parse("8/4p3/8/8/8/8/8/6N1 w - - 0 1");
  const auto black_bishop_white_pawn = parse("5b2/8/8/8/8/8/4P3/8 w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, white_knight_black_pawn.board),
            eval_material(Colour::Black, white_knight_black_pawn.board));
  EXPECT_GT(eval_material(Colour::Black, black_bishop_white_pawn.board),
            eval_material(Colour::White, black_bishop_white_pawn.board));
}

TEST(MaterialEval, RooksAreWorthMoreThanBishops) {
  const auto pos = parse("5b2/8/8/8/8/8/8/7R w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board), eval_material(Colour::Black, pos.board));
}

TEST(MaterialEval, QueensAreWorthMoreThanRooks) {
  const auto pos = parse("7r/8/8/8/8/8/8/3Q4 w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board), eval_material(Colour::Black, pos.board));
}

// PSQT ------------------------------------------------------------------------

TEST(PsqEval, PawnReadyToPromoteIsBetter) {
  const auto ready = parse("8/4P3/8/8/8/8/8/8 w - - 0 1");
  const auto unmoved = parse("8/8/8/8/8/8/4P3/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, ready.board), eval_psqt(Colour::White, unmoved.board));
}

TEST(PsqEval, KnightOnEdgeBeatsCorner) {
  const auto edge = parse("8/8/8/8/N7/8/8/8 w - - 0 1");
  const auto corner = parse("8/8/8/8/8/8/8/N7 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, edge.board), eval_psqt(Colour::White, corner.board));
}

TEST(PsqEval, KnightInCentreBeatsEdge) {
  const auto centre = parse("8/8/8/8/3N4/8/8/8 w - - 0 1");
  const auto edge = parse("8/8/8/8/N7/8/8/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, centre.board), eval_psqt(Colour::White, edge.board));
}

TEST(PsqEval, BishopInCentreBeatsCorner) {
  const auto centre = parse("8/8/8/8/3B4/8/8/8 w - - 0 1");
  const auto corner = parse("8/8/8/8/8/8/8/B7 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, centre.board), eval_psqt(Colour::White, corner.board));
}

TEST(PsqEval, RookOn7thBeatsCentre) {
  const auto seventh = parse("8/3R4/8/8/8/8/8/8 w - - 0 1");
  const auto centre = parse("8/8/8/8/3R4/8/8/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, seventh.board), eval_psqt(Colour::White, centre.board));
}

TEST(PsqEval, CastledKingIsSafer) {
  const auto castled = parse("8/8/8/8/8/8/8/6K1 w - - 0 1");
  const auto uncastled = parse("8/8/8/8/8/8/8/4K3 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, castled.board), eval_psqt(Colour::White, uncastled.board));
}

// Combined eval ---------------------------------------------------------------

TEST(Eval, MatchesFixtureEvals) {
  const auto records = fixtures::load_eval(fixtures::eval_path());
  ASSERT_FALSE(records.empty());

  for (const auto& rec : records) {
    const auto pos = parse(rec.fen);
    EXPECT_EQ(rec.score, eval(pos)) << rec.name;
  }
}

// -----------------------------------------------------------------------------
// Symmetry Tests
// -----------------------------------------------------------------------------

TEST(Eval, SymmetricPositionIsZero) {
  // Perfectly symmetric position with equal material
  const auto pos = parse("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");

  EXPECT_EQ(eval(pos), 0);
}

TEST(Eval, ColourFlipNegates) {
  // Same position but with side to move flipped should negate eval
  // Position: White has a knight advantage
  const auto white_to_move = parse("4k3/8/8/8/4N3/8/8/4K3 w - - 0 1");
  const auto black_to_move = parse("4k3/8/8/8/4N3/8/8/4K3 b - - 0 1");

  // White knight vs nothing - white advantage
  const auto eval_white = eval(white_to_move);
  const auto eval_black = eval(black_to_move);

  // Eval from white's perspective should be negated when black moves
  EXPECT_EQ(eval_white, -eval_black);
}

// -----------------------------------------------------------------------------
// Material Imbalances
// -----------------------------------------------------------------------------

TEST(Eval, QueenVsTwoRooks) {
  // Queen (900) vs two Rooks (500 + 500 = 1000)
  // Two rooks are slightly better materialwise
  const auto queen_side = parse("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");   // White has Q
  const auto rooks_side = parse("4k3/8/8/8/8/8/8/2R1KR2 w - - 0 1"); // White has 2R

  const auto queen_eval = eval(queen_side);
  const auto rooks_eval = eval(rooks_side);

  // Two rooks should be worth slightly more than queen
  EXPECT_GT(rooks_eval, queen_eval);
}

TEST(Eval, BishopPairVsTwoKnights) {
  // Two Bishops (350 + 350 = 700) vs Two Knights (300 + 300 = 600)
  // Bishop pair should be worth more
  const auto bishops = parse("4k3/8/8/8/8/8/8/1B2KB2 w - - 0 1");
  const auto knights = parse("4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1");

  const auto bishop_eval = eval(bishops);
  const auto knight_eval = eval(knights);

  // Bishops should be worth more than knights
  EXPECT_GT(bishop_eval, knight_eval);
}
