#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "c3/attacks.hpp"
#include "c3/bitboard.hpp"
#include "c3/board.hpp"
#include "c3/colour.hpp"
#include "c3/eval.hpp"
#include "c3/move.hpp"
#include "c3/movegen.hpp"
#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/rng.hpp"
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

// Mirror a position through the horizontal axis AND swap the colours: rank 1
// becomes rank 8, every white piece becomes a black one, and the other side
// gets the move.
//
// The result is the same position seen from the other chair. Chess treats the
// two colours identically, so the score from the side to move's point of view
// must come out exactly the same—and that is the single property most at risk
// when a term is written per colour, because it only holds if EVERY forward
// direction, rank table, mask and shield zone was flipped consistently. Castling
// rights and the en passant square are dropped from both sides of the
// comparison: neither reaches the evaluation, and mirroring them correctly would
// only add a way for the test itself to be wrong.
std::string mirror_ranks_and_colours(std::string_view fen) {
  const auto board_end = fen.find(' ');
  const std::string_view board = fen.substr(0, board_end);
  const char side_to_move = fen[board_end + 1];

  std::vector<std::string> ranks;
  std::string rank;
  for (const char square : board) {
    if (square == '/') {
      ranks.push_back(rank);
      rank.clear();
      continue;
    }
    // Case identifies the owner, so swapping it swaps the colours.
    rank.push_back(static_cast<char>(std::islower(static_cast<unsigned char>(square)) != 0
                                         ? std::toupper(static_cast<unsigned char>(square))
                                         : std::tolower(static_cast<unsigned char>(square))));
  }
  ranks.push_back(rank);

  std::ranges::reverse(ranks);

  std::string mirrored;
  for (std::size_t index = 0; index < ranks.size(); ++index) {
    if (index > 0) {
      mirrored.push_back('/');
    }
    mirrored += ranks[index];
  }

  return mirrored + (side_to_move == 'w' ? " b - - 0 1" : " w - - 0 1");
}

// The same board with castling rights and the en passant square stripped, so
// that a position and its mirror are compared on equal footing.
std::string without_move_state(std::string_view fen) {
  const auto board_end = fen.find(' ');
  return std::string(fen.substr(0, board_end)) + ' ' + fen[board_end + 1] + " - - 0 1";
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

TEST(Eval, SymmetricPositionScoresOnlyTheTempoBonus) {
  // Perfectly symmetric position with equal material: every term computed for
  // White is computed identically for Black and cancels, so the only thing left
  // is what the side to move is paid for holding the move.
  const auto pos = parse("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");

  EXPECT_EQ(eval(pos), TEMPO_BONUS);
  EXPECT_EQ(eval_material_and_psqt(pos), 0);
}

TEST(Eval, ColourFlipNegatesEverythingButTempo) {
  // Same position with the side to move flipped. The board-derived half of the
  // evaluation negates exactly; the tempo bonus does not, because it is paid to
  // whoever is on move rather than to a colour. So the two scores do not cancel
  // to zero—they cancel to two tempo bonuses.
  //
  // Position: White has a rook advantage (a lone knight would be scored as an
  // insufficient-material draw, which would make this test pass vacuously).
  const auto white_to_move = parse("4k3/8/8/8/4R3/8/8/4K3 w - - 0 1");
  const auto black_to_move = parse("4k3/8/8/8/4R3/8/8/4K3 b - - 0 1");

  ASSERT_NE(eval_material_and_psqt(white_to_move), 0);

  EXPECT_EQ(eval(white_to_move) + eval(black_to_move), 2 * TEMPO_BONUS);
  EXPECT_EQ(eval_material_and_psqt(white_to_move), -eval_material_and_psqt(black_to_move));
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

// -----------------------------------------------------------------------------
// Colour Symmetry
// -----------------------------------------------------------------------------

namespace {

// A board and the same board turned upside down with the colours swapped must
// score identically from the side to move's point of view.
void expect_colour_symmetric(std::string_view fen) {
  const auto pos = parse(without_move_state(fen));
  const auto mirrored = parse(mirror_ranks_and_colours(fen));

  EXPECT_EQ(eval(pos), eval(mirrored)) << fen << "  vs  " << mirror_ranks_and_colours(fen);
}

} // namespace

TEST(Eval, TheRankAndColourMirrorIsItsOwnInverse) {
  // Pins the test helper itself. Without this, a mirror function that quietly
  // returned its input unchanged would make every symmetry assertion below pass
  // for no reason at all.
  constexpr std::string_view fen = "4k3/1p6/8/3P4/8/8/1P6/4K3 w - - 0 1";

  const std::string mirrored = mirror_ranks_and_colours(fen);

  EXPECT_EQ(mirrored, "4k3/1p6/8/8/3p4/8/1P6/4K3 b - - 0 1");
  EXPECT_NE(mirrored, without_move_state(fen));
  EXPECT_EQ(mirror_ranks_and_colours(mirrored), without_move_state(fen));
}

TEST(Eval, MirroringRanksAndColoursKeepsTheSameScore) {
  // Hand-picked positions covering every term at once: pawn chains and passers,
  // a castled king with a shield, a king on an open wing, rooks on open and
  // seventh ranks, and pieces with wildly different amounts of room.
  constexpr std::array<std::string_view, 10> positions = {
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
      "r2q1rk1/pp1bppbp/2np1np1/8/3NP3/2N1BP2/PPPQB1PP/2KR3R w - - 0 1",
      "4k3/1p6/8/3P4/8/8/1P6/4K3 w - - 0 1",
      "3qk3/8/8/8/3Q4/8/5PPP/1K6 w - - 0 1",
      "4k1r1/8/8/8/5n1q/8/PPP2PPP/6K1 b - - 0 1",
      "4k3/R7/8/8/8/8/8/7K w - - 0 1",
      "7k/8/8/8/8/2P5/2P5/7K w - - 0 1",
      "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 b - - 0 1",
  };

  for (const auto fen : positions) {
    expect_colour_symmetric(fen);
  }
}

TEST(Eval, RandomPositionsAreColourSymmetric) {
  // The hand-picked list above covers the cases somebody thought of. This one
  // does not: several hundred seeded random boards, each with two kings and a
  // handful of pieces scattered anywhere they are legal. A forward direction
  // flipped the wrong way for one colour survives a curated list far more easily
  // than it survives this. Seeded, so any failure is reproducible.
  constexpr int POSITIONS = 400;
  constexpr std::string_view PLACEABLE = "PNBRQpnbrq";

  HashRng rng(0xC0FFEE'1234'5678ULL);

  const auto random_index = [&rng](std::size_t bound) {
    return static_cast<std::size_t>(rng.next() % bound);
  };

  for (int attempt = 0; attempt < POSITIONS; ++attempt) {
    std::array<char, 64> squares{};
    squares.fill('\0');

    // Exactly one king per side: the FEN parser rejects a second one, and a
    // board with no king at all would skip the king-safety term entirely.
    for (const char king_char : {'K', 'k'}) {
      std::size_t square = random_index(64);
      while (squares[square] != '\0') {
        square = random_index(64);
      }
      squares[square] = king_char;
    }

    const std::size_t extra_pieces = 1 + random_index(11);
    for (std::size_t piece = 0; piece < extra_pieces; ++piece) {
      const char piece_char = PLACEABLE[random_index(PLACEABLE.size())];

      for (int tries = 0; tries < 8; ++tries) {
        const std::size_t square = random_index(64);
        const bool back_rank = square < 8 || square >= 56;
        const bool is_pawn = piece_char == 'P' || piece_char == 'p';
        if (squares[square] != '\0' || (is_pawn && back_rank)) {
          continue; // Occupied, or a pawn on a rank it can never stand on.
        }
        squares[square] = piece_char;
        break;
      }
    }

    // Squares run A1..H8, but a FEN is written from rank 8 downwards.
    std::string board;
    for (int rank = 7; rank >= 0; --rank) {
      int empty_run = 0;
      for (int file = 0; file < 8; ++file) {
        const char square = squares[static_cast<std::size_t>((rank * 8) + file)];
        if (square == '\0') {
          ++empty_run;
          continue;
        }
        if (empty_run > 0) {
          board.push_back(static_cast<char>('0' + empty_run));
          empty_run = 0;
        }
        board.push_back(square);
      }
      if (empty_run > 0) {
        board.push_back(static_cast<char>('0' + empty_run));
      }
      if (rank > 0) {
        board.push_back('/');
      }
    }

    const std::string fen = board + (random_index(2) == 0 ? " w - - 0 1" : " b - - 0 1");
    expect_colour_symmetric(fen);
  }
}

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

// These three tests are about the BLEND, not about the terms being blended, so
// they compare against eval_material_and_psqt: the same taper arithmetic with
// nothing else mixed in. Adding the positional terms here would only make the
// expected values harder to derive without testing the taper any harder.

TEST(TaperedEval, FullArmiesReturnTheMiddlegameScore) {
  const auto pos = parse("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
  ASSERT_EQ(game_phase(pos.board), PHASE_MAX);

  EXPECT_EQ(eval_material_and_psqt(pos), advantage(pos, Phase::Middlegame));
}

TEST(TaperedEval, PieceLessPositionsReturnTheEndgameScore) {
  const auto pos = parse("4k3/4p3/8/8/8/8/PP6/4K3 w - - 0 1");
  ASSERT_EQ(game_phase(pos.board), 0);

  EXPECT_EQ(eval_material_and_psqt(pos), advantage(pos, Phase::Endgame));
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

  EXPECT_GE(eval_material_and_psqt(pos), std::min(middlegame, endgame));
  EXPECT_LE(eval_material_and_psqt(pos), std::max(middlegame, endgame));
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

// -----------------------------------------------------------------------------
// Incremental accumulator: the running totals must always agree with the slow,
// obviously-correct computation they replace
// -----------------------------------------------------------------------------

namespace {

// The bishop-pair bonus is the one material term the accumulator does not
// carry, so a test comparing the two has to add it back by hand.
bool test_has_bishop_pair(const Board& board, Colour side) {
  constexpr Bitboard dark = 0xAA55'AA55'AA55'AA55ULL;
  const Bitboard bishops = board.pieces(bishop(side));
  return (bishops & dark) != 0 && (bishops & ~dark) != 0;
}

// Material and piece squares the slow way: walk both piece lists, twice, and
// taper the result. This is the reference the incremental evaluation has to
// reproduce exactly, and its counterpart in the engine is
// eval_material_and_psqt() rather than eval()—the positional terms in eval() do
// not come from the accumulator at all, so folding them in here would test the
// terms instead of the running totals these tests exist to check.
//
// The taper, clamp and side-to-move flip below deliberately mirror the engine's
// own arithmetic rather than deriving it independently: only the accumulator is
// under test here, so everything downstream of it is held constant on purpose.
// A change to the taper formula has to be made in both places.
int reference_material_and_psqt(const Position& pos) {
  if (has_insufficient_material(pos.board)) {
    return CENTIPAWN_DRAW;
  }

  const auto white_advantage = [&pos](Phase phase) {
    return eval_material(Colour::White, pos.board, phase) -
           eval_material(Colour::Black, pos.board, phase) +
           eval_psqt(Colour::White, pos.board, phase) - eval_psqt(Colour::Black, pos.board, phase);
  };

  const int phase = game_phase(pos.board);
  const int score = (white_advantage(Phase::Middlegame) * phase +
                     white_advantage(Phase::Endgame) * (PHASE_MAX - phase)) /
                    PHASE_MAX;
  const int bounded = std::clamp(score, -CENTIPAWN_EVAL_MAX, CENTIPAWN_EVAL_MAX);

  return pos.colour_to_move == Colour::White ? bounded : -bounded;
}

std::string_view name_of(Colour side) {
  return side == Colour::White ? "white" : "black";
}

void expect_side_totals_match(const EvalAccumulator& live, const EvalAccumulator& rebuilt,
                              Colour side, std::string_view context) {
  EXPECT_EQ(live.middlegame(side), rebuilt.middlegame(side))
      << context << " (middlegame, " << name_of(side) << ")";
  EXPECT_EQ(live.endgame(side), rebuilt.endgame(side))
      << context << " (endgame, " << name_of(side) << ")";
}

// Both halves of the invariant in one place: the accumulator matches a rebuild
// from the pieces on the board, and the fast eval matches the slow one.
void expect_accumulator_is_sound(const Position& pos, std::string_view context) {
  const auto& live = pos.board.accumulator();
  const auto rebuilt = pos.board.compute_accumulator();

  expect_side_totals_match(live, rebuilt, Colour::White, context);
  expect_side_totals_match(live, rebuilt, Colour::Black, context);

  EXPECT_EQ(live.phase(), rebuilt.phase()) << context << " (phase)";
  EXPECT_EQ(eval_material_and_psqt(pos), reference_material_and_psqt(pos))
      << context << " (material and piece squares)";
}

constexpr std::array<std::string_view, 8> ACCUMULATOR_FENS = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "4k3/8/8/8/8/8/8/4KB2 w - - 0 1",
    "8/8/8/3k4/8/8/3K4/8 w - - 0 1",
    "QQQQQQQQ/QQQQQQQK/8/8/8/8/8/7k w - - 0 1",
    "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
};

} // namespace

TEST(EvalAccumulator, FenLoadInitialisesTheTotals) {
  // Positions arrive from FEN, not from a sequence of moves, so the very first
  // thing the accumulator has to get right is a board it never saw built.
  for (const auto fen : ACCUMULATOR_FENS) {
    const auto pos = parse(fen);
    expect_accumulator_is_sound(pos, fen);
  }
}

TEST(EvalAccumulator, TotalsAreMaterialPlusPieceSquareBonuses) {
  // Pins WHAT the accumulator holds, not just that it is self-consistent: one
  // side's running total is its material (minus the bishop-pair bonus, which
  // belongs to no single piece) plus its piece-square bonuses.
  for (const auto fen : ACCUMULATOR_FENS) {
    const auto pos = parse(fen);
    const auto& accumulator = pos.board.accumulator();

    for (const auto side : {Colour::White, Colour::Black}) {
      const bool pair = test_has_bishop_pair(pos.board, side);

      const int expected_middlegame = eval_material(side, pos.board, Phase::Middlegame) -
                                      (pair ? BISHOP_PAIR_MIDDLEGAME : 0) +
                                      eval_psqt(side, pos.board, Phase::Middlegame);
      const int expected_endgame = eval_material(side, pos.board, Phase::Endgame) -
                                   (pair ? BISHOP_PAIR_ENDGAME : 0) +
                                   eval_psqt(side, pos.board, Phase::Endgame);

      EXPECT_EQ(accumulator.middlegame(side), expected_middlegame) << fen;
      EXPECT_EQ(accumulator.endgame(side), expected_endgame) << fen;
    }
  }
}

TEST(EvalAccumulator, PhaseTotalIsUncappedButGamePhaseIsNot) {
  // The accumulator deliberately stores the raw phase count so that add() and
  // remove() stay exact inverses; game_phase() applies the 24-point cap. A
  // board full of promoted queens is where the two readings part company.
  const auto normal = parse("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  EXPECT_EQ(normal.board.accumulator().phase(), PHASE_MAX);
  EXPECT_EQ(game_phase(normal.board), PHASE_MAX);

  const auto promoted = parse("QQQQQQQQ/QQQQQQQK/8/8/8/8/8/7k w - - 0 1");
  EXPECT_GT(promoted.board.accumulator().phase(), PHASE_MAX);
  EXPECT_EQ(game_phase(promoted.board), PHASE_MAX);
}

TEST(EvalAccumulator, EveryMoveKindKeepsTheTotalsInSync) {
  // One case per way a move can change the board, because each one reaches
  // put_piece/remove_piece by a different route: a promotion swaps a pawn for a
  // queen, en passant clears a square the move never names, and castling moves
  // two pieces for one move.
  struct Case {
    std::string_view name;
    std::string_view fen;
    Move move;
  };

  const std::array<Case, 6> cases = {{
      {"quiet", "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
       Move{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false}},
      {"capture", "n3k3/8/8/8/8/8/8/R3K3 w - - 0 1",
       Move{Piece::WR, Square::A1, Square::A8, Piece::BN, std::nullopt, false}},
      {"promotion", "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1",
       Move{Piece::WP, Square::C7, Square::C8, std::nullopt, Piece::WQ, false}},
      {"promotion-capture", "1n2k3/2P5/8/8/8/8/8/4K3 w - - 0 1",
       Move{Piece::WP, Square::C7, Square::B8, Piece::BN, Piece::WQ, false}},
      {"en-passant", "4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 1",
       Move{Piece::WP, Square::D5, Square::E6, Piece::BP, std::nullopt, true}},
      {"castling", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
       Move{Piece::WK, Square::E1, Square::G1, std::nullopt, std::nullopt, false}},
  }};

  for (const auto& test_case : cases) {
    Position pos = parse(test_case.fen);
    const auto before = pos.board.accumulator();

    expect_accumulator_is_sound(pos, test_case.name);
    pos.make_move(test_case.move);
    expect_accumulator_is_sound(pos, test_case.name);
    pos.unmake_move(test_case.move);
    expect_accumulator_is_sound(pos, test_case.name);

    EXPECT_EQ(pos.board.accumulator(), before) << test_case.name << " (unmake did not restore)";
  }
}

TEST(EvalAccumulator, QueenSideCastlingKeepsTheTotalsInSync) {
  // The two castles move the rook different distances, so they are separate
  // paths through make_move and each needs its own case.
  Position pos = parse("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  const Move mv{Piece::WK, Square::E1, Square::C1, std::nullopt, std::nullopt, false};
  const auto before = pos.board.accumulator();

  pos.make_move(mv);
  expect_accumulator_is_sound(pos, "queenside castling");
  pos.unmake_move(mv);
  expect_accumulator_is_sound(pos, "queenside castling undone");

  EXPECT_EQ(pos.board.accumulator(), before);
}

TEST(EvalAccumulator, SurvivesRandomPlayouts) {
  // The per-move-kind cases above are hand-picked; this one is not. Several
  // seeded games of random legal moves, checking the totals after EVERY make
  // and EVERY unmake, is what catches the combination nobody thought to write
  // down—an under-promotion that also captures a rook and removes a castling
  // right, say. Seeded, so a failure is reproducible.
  constexpr std::array<std::string_view, 3> openings = {
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
  };
  constexpr int MAX_PLIES = 120;

  HashRng rng(0x5EED'5EED'5EED'5EEDULL);
  int plies_played = 0;

  for (const auto opening : openings) {
    Position pos = parse(opening);
    expect_accumulator_is_sound(pos, opening);

    for (int ply = 0; ply < MAX_PLIES; ++ply) {
      // Filter the pseudo-legal list down to moves that do not leave our own
      // king in check—make it, look, and take it back if it was illegal. Every
      // one of those trial unmakes is itself a check of the invariant.
      const Colour mover = pos.colour_to_move;
      MoveList legal;
      for (const auto& candidate : pseudo_legal_moves(pos)) {
        pos.make_move(candidate);
        const bool leaves_king_in_check = is_in_check(mover, pos.board);
        pos.unmake_move(candidate);
        if (!leaves_king_in_check) {
          legal.push_back(candidate);
        }
      }

      if (legal.empty()) {
        break; // Checkmate or stalemate: this game is over.
      }

      const auto& chosen = legal[static_cast<std::size_t>(rng.next() % legal.size())];

      pos.make_move(chosen);
      expect_accumulator_is_sound(pos, "after make");
      ++plies_played;

      pos.unmake_move(chosen);
      expect_accumulator_is_sound(pos, "after unmake");

      pos.make_move(chosen); // Play it for real and carry on.
      expect_accumulator_is_sound(pos, "after replay");
    }
  }

  // Guard against the loop silently doing nothing (an empty move list on the
  // very first ply would otherwise leave this test vacuously green).
  EXPECT_GT(plies_played, 200);
}

// -----------------------------------------------------------------------------
// Pawn structure
// -----------------------------------------------------------------------------
// Every position below is a PAIR of boards that differ in exactly one
// structural fact. The squares are chosen so that the piece-square tables, the
// material count and the game phase come out identical on both boards, which is
// what makes each assertion a statement about the term under test rather than
// about the tables underneath it.

TEST(PawnStructure, PassedPawnBeatsBlockedPawn) {
  // White's d5 pawn is passed in the first position (no black pawn on the c, d
  // or e files ahead of it) and stopped in the second. b7 and c7 are worth the
  // same on the pawn table, and White's b2 pawn denies Black a passer of its own
  // either way, so who owns a passed pawn is the only difference.
  const auto passed = parse("4k3/1p6/8/3P4/8/8/1P6/4K3 w - - 0 1");
  const auto blocked = parse("4k3/2p5/8/3P4/8/8/1P6/4K3 w - - 0 1");

  EXPECT_GT(eval(passed), eval(blocked));
}

TEST(PawnStructure, DoubledPawnsAreWorseThanSpreadPawns) {
  // c2+c3 against c2+f3. Both boards hold two pawns, both pawns are isolated on
  // both boards, and c3 and f3 are worth the same on the pawn table—so the pawns
  // getting in each other's way on the c-file is the whole difference.
  const auto doubled = parse("7k/8/8/8/8/2P5/2P5/7K w - - 0 1");
  const auto spread = parse("7k/8/8/8/8/5P2/2P5/7K w - - 0 1");

  EXPECT_GT(eval(spread), eval(doubled));
}

TEST(PawnStructure, IsolatedPawnsAreWorseThanConnectedPawns) {
  // a2+c2 can never defend each other; a2+b2 can. The pawn table gives 5+10
  // either way, so the boards differ only in whether the pawns are neighbours.
  const auto isolated = parse("7k/8/8/8/8/8/P1P5/7K w - - 0 1");
  const auto connected = parse("7k/8/8/8/8/8/PP6/7K w - - 0 1");

  EXPECT_GT(eval(connected), eval(isolated));
}

// -----------------------------------------------------------------------------
// King safety
// -----------------------------------------------------------------------------

TEST(KingSafety, PawnShieldBeatsAnOpenWing) {
  // The same king and the same pawns, two different corners: on g1 the king sits
  // behind f2/g2/h2, on b1 it has walked away from its shelter and the a, b and
  // c files are wide open. b1 and g1 are worth the same on both king tables, and
  // White's queen on d4 sees the same squares either way, so the king's cover is
  // the only thing that changes.
  const auto sheltered = parse("3qk3/8/8/8/3Q4/8/5PPP/6K1 w - - 0 1");
  const auto exposed = parse("3qk3/8/8/8/3Q4/8/5PPP/1K6 w - - 0 1");

  EXPECT_GT(eval(sheltered), eval(exposed));
}

TEST(KingSafety, PiecesAimedAtTheKingAreAPenalty) {
  // White has pawns on both wings, so the shield and open-file terms score the
  // same whichever corner the king picks. Black's rook, knight and queen all
  // bear down on the squares around g1 and none of them touch b1: the difference
  // is purely how many enemy pieces are looking at the king.
  const auto attacked = parse("4k1r1/8/8/8/5n1q/8/PPP2PPP/6K1 w - - 0 1");
  const auto safe = parse("4k1r1/8/8/8/5n1q/8/PPP2PPP/1K6 w - - 0 1");

  EXPECT_GT(eval(safe), eval(attacked));
}

// -----------------------------------------------------------------------------
// Rooks
// -----------------------------------------------------------------------------

TEST(RookEval, OpenFileBeatsAFileBlockedByItsOwnPawn) {
  // a1 and b1 are worth the same on the rook table, and White's b2 pawn stands on
  // both boards. From a1 the rook owns an open file; from b1 it stares at the
  // back of its own pawn. Mobility pushes the same way here, which is precisely
  // why an open file is worth having.
  const auto open_file = parse("4k3/8/8/8/8/8/1P6/R6K w - - 0 1");
  const auto blocked_file = parse("4k3/8/8/8/8/8/1P6/1R5K w - - 0 1");

  EXPECT_GT(eval(open_file), eval(blocked_file));
}

TEST(RookEval, SeventhRankIsWorthMoreThanThePieceSquareTableAlone) {
  // A rook has fourteen moves from anywhere on an otherwise empty board, so a7
  // and a6 score the same for mobility and both sit on an open file. The
  // piece-square table already prefers a7; the seventh-rank term says the
  // preference should be BIGGER than the table alone, because a rook on the
  // seventh rank cages a king stuck on the eighth.
  const auto seventh = parse("4k3/R7/8/8/8/8/8/7K w - - 0 1");
  const auto sixth = parse("4k3/8/R7/8/8/8/8/7K w - - 0 1");

  const int piece_square_gain = eval_psqt(Colour::White, seventh.board, Phase::Middlegame) -
                                eval_psqt(Colour::White, sixth.board, Phase::Middlegame);

  EXPECT_GT(eval(seventh) - eval(sixth), piece_square_gain);
}

// -----------------------------------------------------------------------------
// Mobility
// -----------------------------------------------------------------------------

TEST(Mobility, FreeBishopBeatsABishopBehindItsOwnPawn) {
  // c5+d5 and e5+f5 score identically on the pawn table, are connected passers
  // in both cases, and neither pair changes any other term. The one thing that
  // does change is that e5 stands on the bishop's only diagonal.
  const auto free_bishop = parse("k7/8/8/2PP4/8/8/8/B6K w - - 0 1");
  const auto blocked_bishop = parse("k7/8/8/4PP2/8/8/8/B6K w - - 0 1");

  EXPECT_GT(eval(free_bishop), eval(blocked_bishop));
}

// -----------------------------------------------------------------------------
// Tempo
// -----------------------------------------------------------------------------

TEST(Tempo, HavingTheMoveIsWorthSomething) {
  // The starting position is perfectly symmetric, so every other term cancels.
  // Whatever is left over is the value of being the side that gets to move.
  EXPECT_GT(eval(Position::startpos()), 0);
}

// -----------------------------------------------------------------------------
// The terms on their own
// -----------------------------------------------------------------------------
// The tests above ask whether eval() as a whole prefers the right positions,
// which is what actually matters. These ask the individual term functions
// directly, where nothing else can cancel out or reinforce them—so they can pin
// down details the whole-evaluation tests can only bound, such as an open file
// beating a semi-open one or the attacker penalty running into its cap.

TEST(PawnStructureTerm, PassedPawnsGrowWithTheRankAndCountDoubleInTheEndgame) {
  // Both pawns are isolated, so that penalty cancels out of the comparison and
  // what is left is the passed-pawn bonus at two different distances from home.
  const auto near_promotion = parse("4k3/3P4/8/8/8/8/8/4K3 w - - 0 1");
  const auto just_started = parse("4k3/8/8/8/8/8/3P4/4K3 w - - 0 1");

  const auto advanced = eval_pawn_structure(Colour::White, near_promotion.board);
  const auto at_home = eval_pawn_structure(Colour::White, just_started.board);

  EXPECT_GT(advanced.middlegame, at_home.middlegame);
  EXPECT_GT(advanced.endgame, at_home.endgame);

  // The whole point of the endgame column: with no pieces left to blockade it, a
  // runner is worth far more than the same runner in a crowded middlegame.
  EXPECT_GT(advanced.endgame, advanced.middlegame);
}

TEST(PawnStructureTerm, StackedPawnsAreChargedOncePerPawnInFrontOfThem) {
  // Three pawns on the c-file against the same three pawns fanned out over the
  // b, c and d files at the same ranks. The stack collects two doubled penalties
  // (c2 and c3 each have a friendly pawn ahead of them) and three isolated ones
  // (nothing on the b- or d-file to defend any of them). It also collects only
  // ONE passed-pawn bonus, for the pawn in front, where the healthy trio
  // collects three—so the ranks the two rear pawns would have been paid for come
  // off as well.
  const auto tripled = parse("4k3/8/8/8/2P5/2P5/2P5/4K3 w - - 0 1");
  const auto spread = parse("4k3/8/8/8/3P4/2P5/1P6/4K3 w - - 0 1");

  const auto stacked = eval_pawn_structure(Colour::White, tripled.board);
  const auto healthy = eval_pawn_structure(Colour::White, spread.board);

  EXPECT_EQ(stacked.middlegame - healthy.middlegame,
            (2 * DOUBLED_PAWN_PENALTY.middlegame) + (3 * ISOLATED_PAWN_PENALTY.middlegame) -
                PASSED_PAWN_MIDDLEGAME[1] - PASSED_PAWN_MIDDLEGAME[2]);
  EXPECT_EQ(stacked.endgame - healthy.endgame, (2 * DOUBLED_PAWN_PENALTY.endgame) +
                                                   (3 * ISOLATED_PAWN_PENALTY.endgame) -
                                                   PASSED_PAWN_ENDGAME[1] - PASSED_PAWN_ENDGAME[2]);
}

TEST(PawnStructureTerm, OnlyTheFrontPawnOfAFileCountsAsPassed) {
  // Two white pawns on c4 and c5 with the board otherwise bare of pawns. Both
  // have an empty road ahead by the letter of the passed-pawn mask, but they are
  // one promotion threat, not two: c4 can never get past c5. The stack is
  // therefore worth exactly one passer—the front one—and the rear pawn is
  // charged the doubled penalty on top.
  const auto stacked = parse("4k3/8/8/2P5/2P5/8/8/4K3 w - - 0 1");
  const auto single = parse("4k3/8/8/2P5/8/8/8/4K3 w - - 0 1");

  const auto two_pawns = eval_pawn_structure(Colour::White, stacked.board);
  const auto one_pawn = eval_pawn_structure(Colour::White, single.board);

  // The extra pawn brings a doubled penalty and an isolated penalty of its own,
  // and no passed-pawn bonus whatsoever.
  EXPECT_EQ(two_pawns.middlegame - one_pawn.middlegame,
            DOUBLED_PAWN_PENALTY.middlegame + ISOLATED_PAWN_PENALTY.middlegame);
  EXPECT_EQ(two_pawns.endgame - one_pawn.endgame,
            DOUBLED_PAWN_PENALTY.endgame + ISOLATED_PAWN_PENALTY.endgame);

  // Put the same second pawn on a file of its own instead and it becomes a
  // passer in its own right. f4 and c4 are worth the same on the pawn table, so
  // this is the doubled pair's lost bonus made visible.
  const auto separate = parse("4k3/8/8/2P5/5P2/8/8/4K3 w - - 0 1");
  EXPECT_GT(eval(separate), eval(stacked));
}

TEST(RookTerm, OpenFileBeatsSemiOpenBeatsBlocked) {
  // The same rook on the same square, with the a-file empty, holding an enemy
  // pawn, and holding one of our own.
  const auto open = parse("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
  const auto semi_open = parse("4k3/p7/8/8/8/8/8/R3K3 w - - 0 1");
  const auto blocked = parse("4k3/8/8/8/8/8/P7/R3K3 w - - 0 1");

  EXPECT_EQ(eval_rooks(Colour::White, open.board), ROOK_OPEN_FILE);
  EXPECT_EQ(eval_rooks(Colour::White, semi_open.board), ROOK_SEMI_OPEN_FILE);
  EXPECT_EQ(eval_rooks(Colour::White, blocked.board), PhaseScore{});
}

TEST(RookTerm, SeventhRankNeedsSomethingToAttack) {
  // Both rooks stand on the seventh rank of an open file. The bonus is only paid
  // in the first position, where the enemy king is stuck on the eighth rank with
  // the rook cutting it off; in the second the king has walked out and the
  // seventh rank is just another empty row.
  const auto king_cut_off = parse("4k3/R7/8/8/8/8/8/4K3 w - - 0 1");
  const auto king_in_the_open = parse("8/R7/4k3/8/8/8/8/4K3 w - - 0 1");

  const auto cutting = eval_rooks(Colour::White, king_cut_off.board);
  const auto merely_placed = eval_rooks(Colour::White, king_in_the_open.board);

  EXPECT_EQ(cutting.middlegame - merely_placed.middlegame, ROOK_ON_SEVENTH.middlegame);
  EXPECT_EQ(cutting.endgame - merely_placed.endgame, ROOK_ON_SEVENTH.endgame);

  // The one rook bonus that is bigger in the endgame, because in an ending a
  // rook on the seventh is frequently the whole win.
  EXPECT_GT(ROOK_ON_SEVENTH.endgame, ROOK_ON_SEVENTH.middlegame);
}

TEST(KingSafetyTerm, AttackersHurtUntilTheCap) {
  const auto pos = parse("4k3/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto safety = [&pos](int attackers) {
    return eval_king_safety(Colour::White, pos.board, attackers).middlegame;
  };

  EXPECT_GT(safety(0), safety(1));
  EXPECT_GT(safety(1), safety(2));

  // Beyond the cap the term stops responding. Attacks really do compound, but
  // this evaluation has no tuning data behind it, and an unbounded king-safety
  // term is how an engine talks itself into sacrificing a rook for a fantasy.
  EXPECT_EQ(safety(KING_ZONE_MAX_ATTACKERS), safety(KING_ZONE_MAX_ATTACKERS + 10));

  // King safety has no opinion at all about endgames; the taper switches it off.
  EXPECT_EQ(eval_king_safety(Colour::White, pos.board, 3).endgame, 0);
}

TEST(KingSafetyTerm, ShieldIsCappedAtThreePawns) {
  // The cap is on the TOTAL number of pawns in the shield zone, not on how many
  // may stand on each file. Three pawns in front of a castled king is the most
  // shelter there is to have; a fourth pawn crammed in behind them is a pawn
  // standing behind another pawn, and it earns nothing.
  const auto three_pawns = parse("4k3/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  const auto four_pawns = parse("4k3/8/8/8/8/6P1/5PPP/6K1 w - - 0 1");

  EXPECT_EQ(eval_king_safety(Colour::White, three_pawns.board, 0),
            eval_king_safety(Colour::White, four_pawns.board, 0));

  // Two pawns really are less shelter than three, so the cap is a ceiling rather
  // than a constant.
  const auto two_pawns = parse("4k3/8/8/8/8/8/6PP/6K1 w - - 0 1");
  EXPECT_GT(eval_king_safety(Colour::White, three_pawns.board, 0).middlegame,
            eval_king_safety(Colour::White, two_pawns.board, 0).middlegame);
}

TEST(PieceActivityTerm, MinorsIgnoreSquaresEnemyPawnsCover) {
  // A knight on d4 reaches eight squares. Black pawns on c6 and e6 cover b5, d5
  // and f5; two of those are knight moves, and a knight that lands on one is
  // simply lost, so they do not count as mobility.
  const auto unopposed = parse("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
  const auto hemmed_in = parse("4k3/8/2p1p3/8/3N4/8/8/4K3 w - - 0 1");

  const auto free_knight = eval_piece_activity(Colour::White, unopposed.board);
  const auto watched_knight = eval_piece_activity(Colour::White, hemmed_in.board);

  EXPECT_GT(free_knight.mobility.middlegame, watched_knight.mobility.middlegame);
}

TEST(PieceActivityTerm, CountsThePiecesLookingAtTheEnemyKing) {
  // The f-file rook sees f7 and f8, both next to the black king on e8. The
  // a-file rook sees neither, and is the same piece on the same rank.
  const auto aimed_at_the_king = parse("4k3/8/8/8/8/8/8/4KR2 w - - 0 1");
  const auto pointed_elsewhere = parse("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");

  EXPECT_EQ(eval_piece_activity(Colour::White, aimed_at_the_king.board).king_zone_attackers, 1);
  EXPECT_EQ(eval_piece_activity(Colour::White, pointed_elsewhere.board).king_zone_attackers, 0);
}
