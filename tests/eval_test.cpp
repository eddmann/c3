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

// -----------------------------------------------------------------------------
// King Safety - Pawn Shield
// -----------------------------------------------------------------------------
// Tests include enemy queen to ensure non-zero game phase (king safety matters
// in middlegame, not endgame). This is realistic chess evaluation.

TEST(KingSafety, PawnShieldIntactIsBetter) {
  // Kingside castled with full pawn shield vs missing g-pawn
  // Enemy queen present to make king safety relevant
  const auto full_shield = parse("3qk3/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto missing_g_pawn = parse("3qk3/8/8/8/8/8/5P1P/6K1 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::White, full_shield.board),
            eval_king_safety(Colour::White, missing_g_pawn.board));
}

TEST(KingSafety, MissingFPawnIsWorstPenalty) {
  // Missing f-pawn should be worse than missing g-pawn (f-pawn guards critical squares)
  const auto missing_f_pawn = parse("3qk3/8/8/8/8/8/6PP/6K1 w - - 0 1");
  const auto missing_g_pawn = parse("3qk3/8/8/8/8/8/5P1P/6K1 w - - 0 1");

  EXPECT_LT(eval_king_safety(Colour::White, missing_f_pawn.board),
            eval_king_safety(Colour::White, missing_g_pawn.board));
}

TEST(KingSafety, AdvancedPawnShieldIsWeaker) {
  // Pawns on rank 2 provide better shield than pawns on rank 3
  const auto pawns_on_rank2 = parse("3qk3/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto pawns_on_rank3 = parse("3qk3/8/8/8/8/5PPP/8/6K1 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::White, pawns_on_rank2.board),
            eval_king_safety(Colour::White, pawns_on_rank3.board));
}

TEST(KingSafety, QueensidePawnShieldWorks) {
  // Queenside castled with full pawn shield
  const auto full_shield = parse("4k2q/8/8/8/8/8/PPP5/2K5 w - - 0 1");
  const auto missing_c_pawn = parse("4k2q/8/8/8/8/8/PP6/2K5 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::White, full_shield.board),
            eval_king_safety(Colour::White, missing_c_pawn.board));
}

TEST(KingSafety, BlackPawnShieldSymmetric) {
  // Black kingside castled with pawn shield works symmetrically
  const auto full_shield = parse("6k1/5ppp/8/8/8/8/8/3QK3 w - - 0 1");
  const auto missing_g_pawn = parse("6k1/5p1p/8/8/8/8/8/3QK3 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::Black, full_shield.board),
            eval_king_safety(Colour::Black, missing_g_pawn.board));
}

// -----------------------------------------------------------------------------
// King Safety - Open Files
// -----------------------------------------------------------------------------

TEST(KingSafety, OpenFileNearKingIsBad) {
  // King on g-file with open g-file is dangerous
  const auto closed_file = parse("3qk3/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto open_file = parse("3qk3/8/8/8/8/8/5P1P/6K1 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::White, closed_file.board),
            eval_king_safety(Colour::White, open_file.board));
}

TEST(KingSafety, SemiOpenFileIsBetterThanFullyOpen) {
  // Semi-open (enemy pawn) is less dangerous than fully open
  const auto semi_open = parse("3qk3/6p1/8/8/8/8/5P1P/6K1 w - - 0 1");
  const auto fully_open = parse("3qk3/8/8/8/8/8/5P1P/6K1 w - - 0 1");

  EXPECT_GT(eval_king_safety(Colour::White, semi_open.board),
            eval_king_safety(Colour::White, fully_open.board));
}

// -----------------------------------------------------------------------------
// King Safety - Attack Zone
// -----------------------------------------------------------------------------

TEST(KingSafety, EnemyPiecesNearKingIsDangerous) {
  // Enemy queen near king is much more dangerous than far away
  const auto queen_near = parse("4k3/8/8/8/8/5q2/5PPP/6K1 w - - 0 1");
  const auto queen_far = parse("q3k3/8/8/8/8/8/5PPP/6K1 w - - 0 1");

  EXPECT_LT(eval_king_safety(Colour::White, queen_near.board),
            eval_king_safety(Colour::White, queen_far.board));
}

TEST(KingSafety, QueenAttackWeightedHigherThanKnight) {
  // Queen attacking king zone is more dangerous than knight
  // Both positions have same game phase (queen present) for fair comparison
  const auto queen_attack = parse("4k3/8/8/8/8/5q2/5PPP/6K1 w - - 0 1");
  const auto knight_attack = parse("3qk3/8/8/8/8/5n2/5PPP/6K1 w - - 0 1");

  EXPECT_LT(eval_king_safety(Colour::White, queen_attack.board),
            eval_king_safety(Colour::White, knight_attack.board));
}

TEST(KingSafety, MultipleAttackersCompound) {
  // Multiple attackers near king is worse than single attacker
  const auto single_attacker = parse("4k3/8/8/8/8/5q2/5PPP/6K1 w - - 0 1");
  const auto multiple_attackers = parse("4k3/8/8/8/4r3/5q2/5PPP/6K1 w - - 0 1");

  EXPECT_LT(eval_king_safety(Colour::White, multiple_attackers.board),
            eval_king_safety(Colour::White, single_attacker.board));
}

// -----------------------------------------------------------------------------
// King Safety - Tropism (Distance-based)
// -----------------------------------------------------------------------------

TEST(KingSafety, EnemyPieceCloserToKingIsWorse) {
  // Enemy rook closer to king gives worse safety score
  // Include enemy queen to ensure non-zero game phase
  const auto rook_far = parse("3qk3/8/8/8/8/8/r4PPP/6K1 w - - 0 1");
  const auto rook_close = parse("3qk3/8/8/8/8/8/4rPPP/6K1 w - - 0 1");

  EXPECT_LT(eval_king_safety(Colour::White, rook_close.board),
            eval_king_safety(Colour::White, rook_far.board));
}

TEST(KingSafety, OwnPieceCloserToEnemyKingIsGood) {
  // Our piece closer to enemy king gives us attacking bonus
  const auto our_queen_near = parse("4k3/4Q3/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto our_queen_far = parse("4k3/8/8/8/8/8/5PPP/Q5K1 w - - 0 1");

  // Total king safety should favor position where we threaten enemy king
  const int near_score = eval_king_safety(Colour::White, our_queen_near.board) -
                         eval_king_safety(Colour::Black, our_queen_near.board);
  const int far_score = eval_king_safety(Colour::White, our_queen_far.board) -
                        eval_king_safety(Colour::Black, our_queen_far.board);

  EXPECT_GT(near_score, far_score);
}
