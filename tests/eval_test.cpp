#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "c3/board.hpp"
#include "c3/eval.hpp"
#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "fixtures.hpp"

using namespace c3;

namespace {

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

// Mirror a FEN's board across the d/e file boundary (file f becomes file 7-f).
// Chess is left-right symmetric apart from castling, so a mirrored position is
// strategically identical: our evaluation should score it identically too.
// Only the board field is mirrored; the caller supplies FENs without castling
// rights or en passant squares, the only file-dependent state that is left.
std::string mirror_files(std::string_view fen) {
  const auto board_end = fen.find(' ');
  const std::string_view board = fen.substr(0, board_end);
  const std::string_view rest = fen.substr(board_end);

  std::string mirrored;
  std::string rank;

  const auto flush_rank = [&] {
    std::string expanded;
    for (const char square : rank) {
      if (square >= '1' && square <= '8') {
        expanded.append(static_cast<std::size_t>(square - '0'), '1');
      } else {
        expanded.push_back(square);
      }
    }

    std::ranges::reverse(expanded);

    int empty_run = 0;
    for (const char square : expanded) {
      if (square == '1') {
        ++empty_run;
        continue;
      }
      if (empty_run > 0) {
        mirrored.push_back(static_cast<char>('0' + empty_run));
        empty_run = 0;
      }
      mirrored.push_back(square);
    }
    if (empty_run > 0) {
      mirrored.push_back(static_cast<char>('0' + empty_run));
    }
    rank.clear();
  };

  for (const char square : board) {
    if (square == '/') {
      flush_rank();
      mirrored.push_back('/');
      continue;
    }
    rank.push_back(square);
  }
  flush_rank();

  return mirrored + std::string(rest);
}

} // namespace

// Material --------------------------------------------------------------------

TEST(MaterialEval, MoreMaterialIsGood) {
  const auto pos = parse("4kbnr/8/8/8/8/8/4P3/4KBNR w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board, Phase::Middlegame),
            eval_material(Colour::Black, pos.board, Phase::Middlegame));
}

TEST(MaterialEval, MinorPiecesAreWorthMoreThanPawns) {
  const auto white_knight_black_pawn = parse("8/4p3/8/8/8/8/8/6N1 w - - 0 1");
  const auto black_bishop_white_pawn = parse("5b2/8/8/8/8/8/4P3/8 w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, white_knight_black_pawn.board, Phase::Middlegame),
            eval_material(Colour::Black, white_knight_black_pawn.board, Phase::Middlegame));
  EXPECT_GT(eval_material(Colour::Black, black_bishop_white_pawn.board, Phase::Middlegame),
            eval_material(Colour::White, black_bishop_white_pawn.board, Phase::Middlegame));
}

TEST(MaterialEval, RooksAreWorthMoreThanBishops) {
  const auto pos = parse("5b2/8/8/8/8/8/8/7R w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board, Phase::Middlegame),
            eval_material(Colour::Black, pos.board, Phase::Middlegame));
}

TEST(MaterialEval, QueensAreWorthMoreThanRooks) {
  const auto pos = parse("7r/8/8/8/8/8/8/3Q4 w - - 0 1");

  EXPECT_GT(eval_material(Colour::White, pos.board, Phase::Middlegame),
            eval_material(Colour::Black, pos.board, Phase::Middlegame));
}

// PSQT ------------------------------------------------------------------------

TEST(PsqEval, PawnReadyToPromoteIsBetter) {
  const auto ready = parse("8/4P3/8/8/8/8/8/8 w - - 0 1");
  const auto unmoved = parse("8/8/8/8/8/8/4P3/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, ready.board, Phase::Middlegame),
            eval_psqt(Colour::White, unmoved.board, Phase::Middlegame));
}

TEST(PsqEval, KnightOnEdgeBeatsCorner) {
  const auto edge = parse("8/8/8/8/N7/8/8/8 w - - 0 1");
  const auto corner = parse("8/8/8/8/8/8/8/N7 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, edge.board, Phase::Middlegame),
            eval_psqt(Colour::White, corner.board, Phase::Middlegame));
}

TEST(PsqEval, KnightInCentreBeatsEdge) {
  const auto centre = parse("8/8/8/8/3N4/8/8/8 w - - 0 1");
  const auto edge = parse("8/8/8/8/N7/8/8/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, centre.board, Phase::Middlegame),
            eval_psqt(Colour::White, edge.board, Phase::Middlegame));
}

TEST(PsqEval, BishopInCentreBeatsCorner) {
  const auto centre = parse("8/8/8/8/3B4/8/8/8 w - - 0 1");
  const auto corner = parse("8/8/8/8/8/8/8/B7 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, centre.board, Phase::Middlegame),
            eval_psqt(Colour::White, corner.board, Phase::Middlegame));
}

TEST(PsqEval, RookOn7thBeatsCentre) {
  const auto seventh = parse("8/3R4/8/8/8/8/8/8 w - - 0 1");
  const auto centre = parse("8/8/8/8/3R4/8/8/8 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, seventh.board, Phase::Middlegame),
            eval_psqt(Colour::White, centre.board, Phase::Middlegame));
}

TEST(PsqEval, CastledKingIsSafer) {
  const auto castled = parse("8/8/8/8/8/8/8/6K1 w - - 0 1");
  const auto uncastled = parse("8/8/8/8/8/8/8/4K3 w - - 0 1");

  EXPECT_GT(eval_psqt(Colour::White, castled.board, Phase::Middlegame),
            eval_psqt(Colour::White, uncastled.board, Phase::Middlegame));
}

TEST(PsqEval, KingTablesDisagreeBetweenPhases) {
  // The same two squares, judged by the two king tables: shelter wins in the
  // middlegame, activity wins in the endgame. This disagreement is the whole
  // reason the evaluation is tapered rather than computed once.
  const auto centre = parse("8/8/8/8/3K4/8/8/8 w - - 0 1");
  const auto castled = parse("8/8/8/8/8/8/8/6K1 w - - 0 1");

  EXPECT_LT(eval_psqt(Colour::White, centre.board, Phase::Middlegame),
            eval_psqt(Colour::White, castled.board, Phase::Middlegame));
  EXPECT_GT(eval_psqt(Colour::White, centre.board, Phase::Endgame),
            eval_psqt(Colour::White, castled.board, Phase::Endgame));
}

TEST(PsqEval, EveryTableIsFileSymmetric) {
  // Chess does not care which side of the board you play on, so no piece-square
  // table may prefer one file over its mirror image. Checking every square of
  // every table catches a mistyped row that a single test position would miss.
  for (const auto piece : pieces_for(Colour::White)) {
    for (std::uint8_t index = 0; index < 64; ++index) {
      const auto square = Square::from_index(index);
      const auto mirrored_square =
          Square::from_file_and_rank(static_cast<std::uint8_t>(7 - square.file()), square.rank());

      auto board = Board::empty();
      board.put_piece(piece, square);

      auto mirrored_board = Board::empty();
      mirrored_board.put_piece(piece, mirrored_square);

      EXPECT_EQ(eval_psqt(Colour::White, board, Phase::Middlegame),
                eval_psqt(Colour::White, mirrored_board, Phase::Middlegame))
          << piece << " on " << square.to_string();
      EXPECT_EQ(eval_psqt(Colour::White, board, Phase::Endgame),
                eval_psqt(Colour::White, mirrored_board, Phase::Endgame))
          << piece << " on " << square.to_string();
    }
  }
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
  // Same position but with side to move flipped should negate eval.
  // Position: White has a rook advantage (a lone knight would be scored as an
  // insufficient-material draw, which would make this test pass vacuously).
  const auto white_to_move = parse("4k3/8/8/8/4R3/8/8/4K3 w - - 0 1");
  const auto black_to_move = parse("4k3/8/8/8/4R3/8/8/4K3 b - - 0 1");

  const auto eval_white = eval(white_to_move);
  const auto eval_black = eval(black_to_move);

  ASSERT_NE(eval_white, 0);

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

TEST(Eval, TwoBishopsOutweighTwoKnights) {
  // Two bishops (330 + 330, plus the pair bonus) against two knights (320 + 320).
  // Note this comparison alone does not prove the pair bonus exists: a single
  // bishop already outscores a single knight. BishopPairNeedsBothSquareColours
  // below is the test that isolates the bonus itself.
  const auto bishops = parse("4k3/8/8/8/8/8/8/2B1KB2 w - - 0 1");
  const auto knights = parse("4k3/8/8/8/8/8/8/2N1KN2 w - - 0 1");

  EXPECT_GT(eval(bishops), eval(knights));
}

TEST(MaterialEval, BishopPairNeedsBothSquareColours) {
  // Pieces sit on the same squares throughout, so the piece-square tables
  // cancel and only the material term differs.
  const auto bishop_value = PIECE_VALUES[static_cast<std::size_t>(Piece::WB)];
  const auto knight_value = PIECE_VALUES[static_cast<std::size_t>(Piece::WN)];

  const auto middlegame_material = [](const Position& pos) {
    return eval_material(Colour::White, pos.board, Phase::Middlegame);
  };

  // c1 is dark and f1 is light, so this really is a pair: swapping the knight
  // for the second bishop is worth the piece difference PLUS the bonus.
  const auto pair = parse("4k3/8/8/8/8/8/8/2B1KB2 w - - 0 1");
  const auto mixed = parse("4k3/8/8/8/8/8/8/2B1KN2 w - - 0 1");

  EXPECT_EQ(middlegame_material(pair) - middlegame_material(mixed),
            bishop_value - knight_value + BISHOP_PAIR_MIDDLEGAME);

  // b1 and f1 are both light squares: two bishops, but half the board is still
  // out of reach, so there is no pair bonus to collect.
  const auto same_colour = parse("4k3/8/8/8/8/8/8/1B2KB2 w - - 0 1");
  const auto same_colour_mixed = parse("4k3/8/8/8/8/8/8/1B2KN2 w - - 0 1");

  EXPECT_EQ(middlegame_material(same_colour) - middlegame_material(same_colour_mixed),
            bishop_value - knight_value);

  // A side with one bishop collects nothing either.
  const auto single = parse("4k3/8/8/8/8/8/8/4KB2 w - - 0 1");
  const auto none = parse("4k3/8/8/8/8/8/8/4KN2 w - - 0 1");

  EXPECT_EQ(middlegame_material(single) - middlegame_material(none), bishop_value - knight_value);
}

// -----------------------------------------------------------------------------
// Game Phase: the king wants opposite things in the middlegame and the endgame
// -----------------------------------------------------------------------------

TEST(TaperedEval, MiddlegameKingPrefersShelterToCentre) {
  // Both armies are complete, so this is a middlegame: a king strolling to e4
  // is a target for every enemy piece, while g1 sits behind its own pawns.
  // The two positions hold identical material; only the white king moves.
  const auto castled = parse("rnbqkb1r/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1BKR w - - 0 1");
  const auto centralised = parse("rnbqkb1r/pppppppp/8/8/4K3/8/PPPPPPPP/RNBQ1B1R w - - 0 1");

  EXPECT_GT(eval(castled), eval(centralised));
}

TEST(TaperedEval, EndgameKingPrefersCentreToShelter) {
  // Only a rook is left, so this is an endgame: with no attackers left the king
  // becomes a fighting piece and belongs in the centre, not tucked away on g1.
  const auto centralised = parse("k7/8/8/8/3K4/8/8/7R w - - 0 1");
  const auto sheltered = parse("k7/8/8/8/8/8/8/6KR w - - 0 1");

  EXPECT_GT(eval(centralised), eval(sheltered));
}

// -----------------------------------------------------------------------------
// File Symmetry
// -----------------------------------------------------------------------------

TEST(Eval, MirroringFilesKeepsTheSameScore) {
  constexpr std::string_view fen =
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w - - 0 1";

  const auto pos = parse(fen);
  const auto mirrored = parse(mirror_files(fen));

  EXPECT_EQ(eval(pos), eval(mirrored));
}

// -----------------------------------------------------------------------------
// Static Eval Never Masquerades As A Mate Score
// -----------------------------------------------------------------------------

TEST(Eval, HugeMaterialLeadStaysBelowMateScores) {
  // An absurd promotion orgy: without clamping, the raw material count exceeds
  // CENTIPAWN_MATE_THRESHOLD and the search would report a mate that is not there.
  const auto queens = parse("QQQQkQQQ/QQQQQQQQ/8/8/8/8/8/4K3 w - - 0 1");

  EXPECT_LT(eval(queens), CENTIPAWN_MATE_THRESHOLD);
  EXPECT_GT(eval(queens), 5000);
}

// -----------------------------------------------------------------------------
// Game Phase
// -----------------------------------------------------------------------------

TEST(GamePhase, FullArmiesScoreTheMaximum) {
  EXPECT_EQ(game_phase(Position::startpos().board), PHASE_MAX);
}

TEST(GamePhase, BareKingsScoreZero) {
  EXPECT_EQ(game_phase(parse("4k3/8/8/8/8/8/8/4K3 w - - 0 1").board), 0);
}

TEST(GamePhase, PawnsDoNotDelayTheEndgame) {
  // Every pawn still on the board, but no pieces: a king-and-pawn endgame.
  EXPECT_EQ(game_phase(parse("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1").board), 0);
}

TEST(GamePhase, PromotionsCannotExceedTheMaximum) {
  // Promotions can put more material on the board than the game started with,
  // so the count is capped—otherwise the taper weights would leave [0, 1].
  EXPECT_EQ(game_phase(parse("QQQQkQQQ/QQQQQQQQ/8/8/8/8/8/4K3 w - - 0 1").board), PHASE_MAX);
}

// -----------------------------------------------------------------------------
// Tapering: the two ends of the blend
// -----------------------------------------------------------------------------

namespace {

int advantage(const Position& pos, Phase phase) {
  const int material = eval_material(Colour::White, pos.board, phase) -
                       eval_material(Colour::Black, pos.board, phase);
  const int placement =
      eval_psqt(Colour::White, pos.board, phase) - eval_psqt(Colour::Black, pos.board, phase);
  return material + placement;
}

} // namespace

TEST(TaperedEval, FullArmiesReturnTheMiddlegameScore) {
  const auto pos = parse("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
  ASSERT_EQ(game_phase(pos.board), PHASE_MAX);

  EXPECT_EQ(eval(pos), advantage(pos, Phase::Middlegame));
}

TEST(TaperedEval, PieceLessPositionsReturnTheEndgameScore) {
  const auto pos = parse("4k3/4p3/8/8/8/8/PP6/4K3 w - - 0 1");
  ASSERT_EQ(game_phase(pos.board), 0);

  EXPECT_EQ(eval(pos), advantage(pos, Phase::Endgame));
}

TEST(TaperedEval, PartialArmiesLandBetweenTheTwoScores) {
  // One rook each plus pawns: phase 4 of 24, so the blend sits much closer to
  // the endgame score but is not equal to either end.
  const auto pos = parse("r3k3/pppppppp/8/8/8/8/PPPPPPP1/4K2R w - - 0 1");
  const int phase = game_phase(pos.board);
  ASSERT_GT(phase, 0);
  ASSERT_LT(phase, PHASE_MAX);

  const int middlegame = advantage(pos, Phase::Middlegame);
  const int endgame = advantage(pos, Phase::Endgame);
  ASSERT_NE(middlegame, endgame);

  EXPECT_GE(eval(pos), std::min(middlegame, endgame));
  EXPECT_LE(eval(pos), std::max(middlegame, endgame));
}

// -----------------------------------------------------------------------------
// Insufficient Material
// -----------------------------------------------------------------------------

TEST(Eval, LoneBishopCannotMateSoScoresDrawn) {
  const auto bishop_versus_bare_king = parse("4k3/8/8/8/8/8/8/4KB2 w - - 0 1");

  EXPECT_EQ(eval(bishop_versus_bare_king), CENTIPAWN_DRAW);
}

TEST(Eval, LoneRookCanMateSoDoesNotScoreDrawn) {
  const auto rook_versus_bare_king = parse("4k3/8/8/8/8/8/8/4KR2 w - - 0 1");

  EXPECT_NE(eval(rook_versus_bare_king), CENTIPAWN_DRAW);
}

TEST(InsufficientMaterial, RecognisesDeadDraws) {
  EXPECT_TRUE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/4K3 w - - 0 1").board))
      << "king versus king";
  EXPECT_TRUE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/4KN2 w - - 0 1").board))
      << "lone knight";
  EXPECT_TRUE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/4KB2 w - - 0 1").board))
      << "lone bishop";
  EXPECT_TRUE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/2N1KN2 w - - 0 1").board))
      << "two knights cannot force mate";
  EXPECT_TRUE(has_insufficient_material(parse("4kb2/8/8/8/8/8/8/2B1K3 w - - 0 1").board))
      << "one bishop each, same colour squares (c1 and f8 are both dark)";
  EXPECT_TRUE(has_insufficient_material(parse("2b1k3/8/8/8/8/8/8/2B1K3 w - - 0 1").board))
      << "one bishop each, opposite colours (c8 is light, c1 is dark)";
  EXPECT_TRUE(has_insufficient_material(parse("4kb2/8/8/8/8/8/8/4KN2 w - - 0 1").board))
      << "knight against bishop";
  EXPECT_TRUE(has_insufficient_material(parse("4kn2/8/8/8/8/8/8/4KN2 w - - 0 1").board))
      << "knight against knight";
}

TEST(InsufficientMaterial, LeavesWinnableEndgamesAlone) {
  EXPECT_FALSE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/4KR2 w - - 0 1").board))
      << "a rook mates";
  EXPECT_FALSE(has_insufficient_material(parse("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1").board))
      << "a pawn can promote";
  EXPECT_FALSE(has_insufficient_material(parse("4k3/8/8/8/8/8/8/3BKN2 w - - 0 1").board))
      << "bishop and knight mate";
  EXPECT_FALSE(has_insufficient_material(parse("4kn2/8/8/8/8/8/8/2B1KB2 w - - 0 1").board))
      << "two bishops beat a lone knight";
}

TEST(Eval, MinorAgainstMinorScoresDrawn) {
  EXPECT_EQ(eval(parse("4kb2/8/8/8/8/8/8/4KN2 w - - 0 1")), CENTIPAWN_DRAW);
}

TEST(Eval, TwoKnightsAgainstABareKingScoreDrawn) {
  EXPECT_EQ(eval(parse("4k3/8/8/8/8/8/8/2N1KN2 w - - 0 1")), CENTIPAWN_DRAW);
}
