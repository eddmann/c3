#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <random>
#include <ranges>
#include <string>
#include <string_view>

#include "c3/attacks.hpp"
#include "c3/castling.hpp"
#include "c3/movegen.hpp"
#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/square.hpp"
#include "fixtures.hpp"

using namespace c3;

namespace {

Position parse_fen(std::string_view fen) {
  return Position::from_fen(fen);
}

Bitboard squares_to_bitboard(std::initializer_list<std::string_view> squares) {
  Bitboard result_bitboard = 0;
  for (const auto square_string : squares) {
    const auto parsed_square = Square::parse(square_string);
    if (parsed_square.has_value()) {
      result_bitboard |= *parsed_square;
    }
  }
  return result_bitboard;
}

void assert_attacks_eq(const Position& pos, std::string_view attacker,
                       std::initializer_list<std::string_view> squares) {
  const auto attacker_sq = Square::parse(attacker);
  ASSERT_TRUE(attacker_sq.has_value());

  const auto piece = pos.board.piece_at(*attacker_sq);
  ASSERT_TRUE(piece.has_value());

  const Bitboard expected = squares_to_bitboard(squares);
  EXPECT_EQ(expected, attacks_for(*piece, *attacker_sq, pos.board)) << attacker;
}

void assert_pseudo_legal_move_count(std::string_view fen, std::size_t count) {
  EXPECT_EQ(pseudo_legal_moves(parse_fen(fen)).size(), count) << fen;
}

void assert_legal_move_count(std::string_view fen, std::size_t count) {
  EXPECT_EQ(legal_moves(parse_fen(fen)).size(), count) << fen;
}

// Whatever is_attacked does internally, it must answer exactly what the full
// attacker set says: callers pick between the two on cost, never on meaning.
void assert_attacked_matches_attackers(std::string_view fen) {
  const Position pos = parse_fen(fen);

  for (std::uint8_t index = 0; index < 64; ++index) {
    const Square square = Square::from_index(index);

    for (const Colour attacker : {Colour::White, Colour::Black}) {
      EXPECT_EQ(is_attacked(square, attacker, pos.board),
                get_attackers(square, attacker, pos.board) != 0)
          << fen << " " << square;
    }
  }
}

std::size_t moves_from_square(const MoveList& moves, Square from) {
  return static_cast<std::size_t>(
      std::ranges::count_if(moves, [from](const Move& mv) { return mv.from == from; }));
}

// The deepest fixtures are marked in the fixture file rather than hidden in a
// second file, so the whole suite stays in one place and one loader.
enum class PerftFixtureSet { Everyday, Slow };

bool is_slow_perft_record(const fixtures::PerftRecord& record) {
  return record.name.starts_with("slow-");
}

void assert_perft_fixtures_match(PerftFixtureSet set) {
  const auto records = fixtures::load_perft(fixtures::perft_path());
  std::size_t checked = 0;

  for (const auto& record : records) {
    if (is_slow_perft_record(record) != (set == PerftFixtureSet::Slow)) {
      continue;
    }

    Position pos = Position::from_fen(record.fen);
    EXPECT_EQ(record.nodes, perft(pos, static_cast<std::uint8_t>(record.depth))) << record.name;
    ++checked;
  }

  EXPECT_GT(checked, 0);
}

std::size_t castling_move_count(const MoveList& moves) {
  std::size_t count = 0;
  for (const auto& mv : moves) {
    if (mv.is_castling()) {
      ++count;
    }
  }
  return count;
}

// Support for MoveListCapacity.DISABLED_HillClimbStaysUnderCapacity, which
// re-derives the pseudo-legal move ceiling MoveList's capacity is sized from.
// A board is 64 squares of FEN piece letters, 0 meaning empty.
using CrowdedBoard = std::array<char, 64>;

constexpr std::string_view CROWDING_PIECE_TYPES = "QRBNP";

constexpr bool is_back_rank_index(std::size_t square) {
  return square < 8 || square >= 56;
}

std::size_t random_square(std::mt19937_64& rng) {
  return std::uniform_int_distribution<std::size_t>(0, 63)(rng);
}

char random_piece_type(std::mt19937_64& rng) {
  return CROWDING_PIECE_TYPES[rng() % CROWDING_PIECE_TYPES.size()];
}

std::string crowded_board_to_fen(const CrowdedBoard& board) {
  std::string fen;

  for (std::size_t rank = 8; rank-- > 0;) {
    int empty = 0;

    for (std::size_t file = 0; file < 8; ++file) {
      const char piece = board[(rank * 8) + file];
      if (piece == 0) {
        ++empty;
        continue;
      }
      if (empty != 0) {
        fen += std::to_string(empty);
        empty = 0;
      }
      fen += piece;
    }

    if (empty != 0) {
      fen += std::to_string(empty);
    }
    if (rank != 0) {
      fen += '/';
    }
  }

  return fen + " w - - 0 1";
}

// Scores an arrangement, and checks the invariant the experiment exists to
// test. Returns -1 for an arrangement the FEN parser rejects, so the climb
// skips it rather than treating it as a dead end.
int count_pseudo_legal(const CrowdedBoard& board) {
  const std::string fen = crowded_board_to_fen(board);

  try {
    Position pos = Position::from_fen(fen);
    const auto moves = pseudo_legal_moves(pos);
    EXPECT_LE(moves.size(), MoveList::CAPACITY) << fen;
    return static_cast<int>(moves.size());
  } catch (const std::exception&) {
    return -1;
  }
}

// A white king, a black king and fifteen more white pieces of any type: more
// material than any legal game can produce, which is the point.
CrowdedBoard random_crowded_board(std::mt19937_64& rng) {
  CrowdedBoard board{};
  board[random_square(rng)] = 'K';

  while (true) {
    const std::size_t square = random_square(rng);
    if (board[square] == 0) {
      board[square] = 'k';
      break;
    }
  }

  for (int placed = 0; placed < 15; ++placed) {
    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::size_t square = random_square(rng);
      if (board[square] != 0) {
        continue;
      }
      const char type = random_piece_type(rng);
      board[square] = (type == 'P' && is_back_rank_index(square)) ? 'Q' : type;
      break;
    }
  }

  return board;
}

// One mutation: either slide a white piece to an empty square or retype it.
// Returns false when the mutation was not applicable, leaving `board` alone.
bool mutate_crowded_board(CrowdedBoard& board, std::mt19937_64& rng) {
  const std::size_t from = random_square(rng);
  const char piece = board[from];

  if (piece == 0 || piece == 'k') {
    return false;
  }

  if (rng() % 2 == 0) {
    const std::size_t to = random_square(rng);
    if (board[to] != 0 || (piece == 'P' && is_back_rank_index(to))) {
      return false;
    }
    board[to] = piece;
    board[from] = 0;
    return true;
  }

  if (piece == 'K') {
    return false;
  }

  const char type = random_piece_type(rng);
  if (type == 'P' && is_back_rank_index(from)) {
    return false;
  }
  board[from] = type;
  return true;
}

// Accepts equal scores as well as better ones so the climb can walk along
// plateaux instead of stalling on the first one it reaches.
int hill_climb(CrowdedBoard& board, std::mt19937_64& rng, int steps) {
  int best = count_pseudo_legal(board);

  for (int step = 0; step < steps; ++step) {
    CrowdedBoard candidate = board;
    if (!mutate_crowded_board(candidate, rng)) {
      continue;
    }

    const int score = count_pseudo_legal(candidate);
    if (score >= best) {
      best = score;
      board = candidate;
    }
  }

  return best;
}

} // namespace

// Attack table coverage ---------------------------------------------------

TEST(Attacks, DetectCheck) {
  Board board = Board::empty();
  board.put_piece(Piece::BK, Square::E8);
  board.put_piece(Piece::WN, Square::D6);

  EXPECT_TRUE(is_in_check(Colour::Black, board));
}

TEST(Attacks, SideWithoutKingIsNotInCheck) {
  // Test and analysis positions are routinely set up without a king; there is
  // no king square to probe, so there is nothing that can be in check.
  Board board = Board::empty();
  board.put_piece(Piece::WQ, Square::D1);

  EXPECT_FALSE(is_in_check(Colour::Black, board));
}

TEST(Attacks, IsAttackedAgreesWithAttackerSet) {
  assert_attacked_matches_attackers("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  assert_attacked_matches_attackers(
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
  assert_attacked_matches_attackers("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
  assert_attacked_matches_attackers("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
}

TEST(Attacks, QueenAttacksHorizontal) {
  const Position pos = parse_fen("Q3k3/8/8/8/8/8/8/8 w - - 0 1");
  EXPECT_EQ(get_attackers(Square::E8, Colour::White, pos.board), Bitboard(Square::A8));
}

TEST(Attacks, QueenAttacksVertical) {
  const Position pos = parse_fen("4k3/8/8/8/4Q3/8/8/8 w - - 0 1");
  EXPECT_EQ(get_attackers(Square::E8, Colour::White, pos.board), Bitboard(Square::E4));
}

TEST(Attacks, QueenAttacksDiagonal) {
  const Position pos = parse_fen("4k3/8/8/8/Q7/8/8/8 w - - 0 1");
  EXPECT_EQ(get_attackers(Square::E8, Colour::White, pos.board), Bitboard(Square::A4));
}

TEST(Attacks, WhitePawnAttacksNone) {
  const Position pos = parse_fen("8/8/8/8/8/8/4P3/8 w - - 0 1");
  assert_attacks_eq(pos, "e2", {});
}

TEST(Attacks, WhitePawnAttacksLeft) {
  const Position pos = parse_fen("8/8/8/8/8/3p4/4P3/8 w - - 0 1");
  assert_attacks_eq(pos, "e2", {"d3"});
}

TEST(Attacks, WhitePawnAttacksRight) {
  const Position pos = parse_fen("8/8/8/8/8/5p2/4P3/8 w - - 0 1");
  assert_attacks_eq(pos, "e2", {"f3"});
}

TEST(Attacks, WhitePawnAttacksBothSides) {
  const Position pos = parse_fen("8/8/8/8/8/3p1p2/4P3/8 w - - 0 1");
  assert_attacks_eq(pos, "e2", {"d3", "f3"});
}

TEST(Attacks, BlackPawnAttacksNone) {
  const Position pos = parse_fen("8/4p3/8/8/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "e7", {});
}

TEST(Attacks, BlackPawnAttacksLeft) {
  const Position pos = parse_fen("8/4p3/3P4/8/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "e7", {"d6"});
}

TEST(Attacks, BlackPawnAttacksRight) {
  const Position pos = parse_fen("8/4p3/5P2/8/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "e7", {"f6"});
}

TEST(Attacks, BlackPawnAttacksBothSides) {
  const Position pos = parse_fen("8/4p3/3P1P2/8/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "e7", {"d6", "f6"});
}

TEST(Attacks, KnightAttacks) {
  const Position pos = parse_fen("8/8/8/8/3N4/8/8/8 w - - 0 1");
  assert_attacks_eq(pos, "d4", {"c2", "e2", "b3", "f3", "b5", "f5", "c6", "e6"});
}

// Attack tables are built with bit shifts, which happily carry a piece off one
// edge of the board and back on at the other. These four positions are the ones
// where a missing file mask would show up.
TEST(Attacks, KnightOnAFileDoesNotWrapAround) {
  const Position pos = parse_fen("8/8/8/8/8/8/N7/8 w - - 0 1");
  assert_attacks_eq(pos, "a2", {"c1", "c3", "b4"});
}

TEST(Attacks, KnightOnHFileDoesNotWrapAround) {
  const Position pos = parse_fen("8/8/8/7N/8/8/8/8 w - - 0 1");
  assert_attacks_eq(pos, "h5", {"g3", "f4", "f6", "g7"});
}

TEST(Attacks, KingOnAFileDoesNotWrapAround) {
  const Position pos = parse_fen("8/8/8/8/K7/8/8/8 w - - 0 1");
  assert_attacks_eq(pos, "a4", {"a3", "a5", "b3", "b4", "b5"});
}

TEST(Attacks, KingOnHFileDoesNotWrapAround) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/7K w - - 0 1");
  assert_attacks_eq(pos, "h1", {"g1", "g2", "h2"});
}

TEST(Attacks, BishopAttacksOnEmptyBoard) {
  const Position pos = parse_fen("8/8/8/8/3b4/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "d4",
                    {"a1", "g1", "b2", "f2", "c3", "e3", "c5", "e5", "b6", "f6", "a7", "g7", "h8"});
}

TEST(Attacks, BishopWithUpLeftBlocker) {
  const Position pos = parse_fen("8/8/2n5/8/8/8/6B1/8 w - - 0 1");
  assert_attacks_eq(pos, "g2", {"h1", "h3", "f1", "f3", "e4", "d5", "c6"});
}

TEST(Attacks, BishopWithUpRightBlocker) {
  const Position pos = parse_fen("8/8/5n2/8/8/8/1B6/8 w - - 0 1");
  assert_attacks_eq(pos, "b2", {"a1", "a3", "c1", "c3", "d4", "e5", "f6"});
}

TEST(Attacks, BishopWithDownLeftBlocker) {
  const Position pos = parse_fen("8/8/8/4B3/3n4/8/8/8 w - - 0 1");
  assert_attacks_eq(pos, "e5", {"h8", "g7", "f6", "d4", "f4", "g3", "h2", "d6", "c7", "b8"});
}

TEST(Attacks, BishopWithDownRightBlocker) {
  const Position pos = parse_fen("8/8/8/3b4/8/5N2/8/8 w - - 0 1");
  assert_attacks_eq(pos, "d5", {"a8", "b7", "c6", "e6", "f7", "g8", "c4", "b3", "a2", "e4", "f3"});
}

TEST(Attacks, RookAttacksOnEmptyBoard) {
  const Position pos = parse_fen("8/8/8/8/3r4/8/8/8 b - - 0 1");
  assert_attacks_eq(
      pos, "d4",
      {"d1", "d2", "d3", "d5", "d6", "d7", "d8", "a4", "b4", "c4", "e4", "f4", "g4", "h4"});
}

TEST(Attacks, RookWithUpBlocker) {
  const Position pos = parse_fen("8/8/8/3N4/8/8/8/3r4 b - - 0 1");
  assert_attacks_eq(pos, "d1", {"d2", "d3", "d4", "d5", "a1", "b1", "c1", "e1", "f1", "g1", "h1"});
}

TEST(Attacks, RookWithRightBlocker) {
  const Position pos = parse_fen("8/8/8/r2N4/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "a5", {"b5", "c5", "d5", "a6", "a7", "a8", "a4", "a3", "a2", "a1"});
}

TEST(Attacks, RookWithLeftBlocker) {
  const Position pos = parse_fen("8/8/8/3N3r/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "h5", {"g5", "f5", "e5", "d5", "h6", "h7", "h8", "h4", "h3", "h2", "h1"});
}

TEST(Attacks, RookWithDownBlocker) {
  const Position pos = parse_fen("3r4/8/8/3N4/8/8/8/8 b - - 0 1");
  assert_attacks_eq(pos, "d8", {"d7", "d6", "d5", "a8", "b8", "c8", "e8", "f8", "g8", "h8"});
}

TEST(Attacks, KingAttacks) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/4K3 w - - 0 1");
  assert_attacks_eq(pos, "e1", {"d1", "f1", "d2", "e2", "f2"});
}

// Move generation ---------------------------------------------------------

TEST(Movegen, LegalMoveCountInCheckmateIsZero) {
  assert_legal_move_count("rnb1kbnr/pppp1ppp/4p3/8/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 0 1", 0);
}

TEST(Movegen, LegalMoveCountInCheckIsLimited) {
  assert_legal_move_count("rnbqkbnr/1pp1p1pp/p2p1p2/1B6/8/4P3/PPPP1PPP/RNBQK1NR b KQq - 0 1", 7);
}

// The experiment MAX_PSEUDO_LEGAL_MOVES is quoted from, kept runnable so the
// figure can be re-derived rather than trusted. It is disabled by default
// because it searches rather than proves, and because it is only quick in an
// optimised build; run it with
//
//   build/tests/c3_tests --gtest_also_run_disabled_tests
//       --gtest_filter='*HillClimbStaysUnderCapacity*'
//
// It hill-climbs over placements of a full sixteen-piece white side—a superset
// of any material a real game can produce, since it will happily use fifteen
// queens—and asserts that no arrangement it reaches generates more pseudo-legal
// moves than a MoveList can hold.
TEST(MoveListCapacity, DISABLED_HillClimbStaysUnderCapacity) {
  // The budget the quoted 248 came from: a few seconds in a Release build.
  constexpr int RESTARTS = 400;
  constexpr int STEPS = 60000;

  std::mt19937_64 rng(20240905);
  int best_overall = 0;
  std::string best_fen;

  for (int restart = 0; restart < RESTARTS; ++restart) {
    CrowdedBoard board = random_crowded_board(rng);
    const int best = hill_climb(board, rng, STEPS);

    if (best > best_overall) {
      best_overall = best;
      best_fen = crowded_board_to_fen(board);
    }
  }

  // Informational: the figure MAX_PSEUDO_LEGAL_MOVES records. Beating it would
  // not fail here—only exceeding CAPACITY does, which count_pseudo_legal
  // checks on every arrangement it scores—but it would mean the margin
  // move_list.hpp keeps needs revisiting.
  std::cout << "best pseudo-legal count found: " << best_overall << " (" << best_fen << ")\n";
  EXPECT_GT(best_overall, 0);
  EXPECT_LE(static_cast<std::size_t>(best_overall), MoveList::CAPACITY);
}

TEST(Movegen, GeneratesTheMostCrowdedKnownPosition) {
  // 218 legal moves is the highest count found for a legal chess position, and
  // the figure MoveList's fixed capacity is sized from.
  const Position pos = parse_fen("R6R/3Q4/1Q4Q1/4Q3/2Q4Q/Q4Q2/pp1Q4/kBNN1KB1 w - - 0 1");
  const auto moves = legal_moves(pos);

  EXPECT_EQ(moves.size(), MAX_MOVES_IN_A_POSITION);
  EXPECT_LE(moves.size(), MoveList::CAPACITY);
}

TEST(Movegen, LegalMovesRejectMovesByAPinnedPiece) {
  // The knight on e2 is the only thing between the king on e1 and the rook on
  // e8, so every knight move is pseudo-legal yet none of them is legal.
  const Position pos = parse_fen("4r3/8/8/8/8/8/4N3/4K3 w - - 0 1");

  EXPECT_EQ(moves_from_square(pseudo_legal_moves(pos), Square::E2), 6);
  EXPECT_EQ(moves_from_square(legal_moves(pos), Square::E2), 0);
}

TEST(Movegen, WhitePawnMoves) {
  assert_pseudo_legal_move_count("8/8/8/8/8/8/4P3/8 w - - 0 1", 2);
}

TEST(Movegen, BlackPawnMoves) {
  assert_pseudo_legal_move_count("8/4p3/8/8/8/8/8/8 b - - 0 1", 2);
}

TEST(Movegen, SinglePawnAdvance) {
  assert_pseudo_legal_move_count("8/8/8/8/4p3/8/4P3/8 w - - 0 1", 1);
}

TEST(Movegen, DoublePawnAdvance) {
  assert_pseudo_legal_move_count("8/8/8/8/8/4p3/4P3/8 w - - 0 1", 0);
}

TEST(Movegen, KnightMoves) {
  assert_pseudo_legal_move_count("8/8/8/8/3N4/8/8/8 w - - 0 1", 8);
}

TEST(Movegen, BishopMoves) {
  assert_pseudo_legal_move_count("8/r7/5n2/8/3B4/8/8/8 w - - 0 1", 11);
}

TEST(Movegen, RookMoves) {
  assert_pseudo_legal_move_count("8/3b4/8/8/1n1R4/8/8/8 w - - 0 1", 12);
}

TEST(Movegen, KingMoves) {
  assert_pseudo_legal_move_count("8/8/8/8/8/8/8/4K3 w - - 0 1", 5);
}

TEST(Movegen, PawnPromotionWithAdvance) {
  assert_pseudo_legal_move_count("8/4P3/8/8/8/8/8/8 w - - 0 1", 4);
}

TEST(Movegen, PawnPromotionWithCapture) {
  assert_pseudo_legal_move_count("3qk3/4P3/8/8/8/8/8/8 w - - 0 1", 4);
}

TEST(Movegen, PawnPromotionWithAdvanceOrCapture) {
  assert_pseudo_legal_move_count("3q4/4P3/8/8/8/8/8/8 w - - 0 1", 8);
}

// The positions below cannot be reached through FEN any more: from_fen rejects
// pawns on the back ranks and castling rights without the king and rook at
// home. Movegen still guards against them because a Position can be built or
// mutated directly (its members are public), so the tests do exactly that.

Position with_piece_added(std::string_view fen, Piece piece, Square square) {
  Position pos = parse_fen(fen);
  pos.board.put_piece(piece, square);
  pos.key = pos.compute_key();
  return pos;
}

Position with_castling_rights(std::string_view fen, CastlingRights rights) {
  Position pos = parse_fen(fen);
  pos.castling_rights = rights;
  pos.key = pos.compute_key();
  return pos;
}

TEST(Movegen, WhitePawnOnPromotionRankHasNoAdvance) {
  // Ranks 1 and 8 are unreachable for a pawn in a real game. Advancing from
  // there would step off the 64-square board.
  const Position pos = with_piece_added("4k3/8/8/8/8/8/8/4K3 w - - 0 1", Piece::WP, Square::H8);
  EXPECT_EQ(moves_from_square(pseudo_legal_moves(pos), Square::H8), 0);
}

TEST(Movegen, BlackPawnOnPromotionRankHasNoAdvance) {
  const Position pos = with_piece_added("4k3/8/8/8/8/8/8/4K3 b - - 0 1", Piece::BP, Square::F1);
  EXPECT_EQ(moves_from_square(pseudo_legal_moves(pos), Square::F1), 0);
}

TEST(Movegen, NoCastlingWhenKingHasLeftItsHomeSquare) {
  // Castling rights can disagree with the pieces on the board; the king must
  // actually stand on e1/e8 for a castling move to make sense.
  const Position pos = with_castling_rights(
      "4k3/8/8/8/8/8/8/R2K3R w - - 0 1",
      CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::WhiteQueen}));
  EXPECT_EQ(castling_move_count(pseudo_legal_moves(pos)), 0);
}

TEST(Movegen, OnlyKingSideCastlingWhenQueenRookIsMissing) {
  const Position pos = with_castling_rights(
      "4k3/8/8/8/8/8/8/4K2R w - - 0 1",
      CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::WhiteQueen}));
  const auto moves = pseudo_legal_moves(pos);

  EXPECT_EQ(castling_move_count(moves), 1);

  const auto castling_move =
      *std::ranges::find_if(moves, [](const Move& mv) { return mv.is_castling(); });
  EXPECT_EQ(castling_move.to, Square::G1);
}

TEST(Movegen, OnlyQueenSideCastlingWhenKingRookIsMissing) {
  const Position pos = with_castling_rights(
      "r3k3/8/8/8/8/8/8/4K3 b - - 0 1",
      CastlingRights::from({CastlingRight::BlackKing, CastlingRight::BlackQueen}));
  const auto moves = pseudo_legal_moves(pos);

  EXPECT_EQ(castling_move_count(moves), 1);

  const auto castling_move =
      *std::ranges::find_if(moves, [](const Move& mv) { return mv.is_castling(); });
  EXPECT_EQ(castling_move.to, Square::C8);
}

TEST(Movegen, CastleKingSideOnly) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R3K2R w K - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 1);
}

TEST(Movegen, CastleQueenSideOnly) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R3K2R w Q - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 1);
}

TEST(Movegen, CastleKingAndQueenSide) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R3K2R w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 2);
}

TEST(Movegen, NoCastlingWhenFriendlyPieceOnTarget) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R1B1K1NR w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 0);
}

TEST(Movegen, NoCastlingWhenOpponentPieceOnTarget) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R1b1K1nR w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 0);
}

TEST(Movegen, NoCastlingWhenPieceBlocksPath) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/RN2KB1R w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 0);
}

TEST(Movegen, NoCastlingWhenKingPathAttacked) {
  const Position pos = parse_fen("8/8/8/8/8/4n3/8/R3K2R w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 0);
}

TEST(Movegen, NoCastlingWhenRightPreviouslyLost) {
  const Position pos = parse_fen("8/8/8/8/8/8/8/R3K2R w Q - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 1);

  const auto castling_move =
      *std::ranges::find_if(moves, [](const Move& mv) { return mv.is_castling(); });

  EXPECT_EQ(castling_move.from, Square::E1);
  EXPECT_EQ(castling_move.to, Square::C1);
}

TEST(Movegen, NoCastlingOutOfCheck) {
  const Position pos = parse_fen("8/8/8/8/8/3n4/8/R3K2R w KQ - 0 1");
  const auto moves = pseudo_legal_moves(pos);
  EXPECT_EQ(castling_move_count(moves), 0);
}

TEST(Movegen, EnPassantCaptureGeneratesBothCaptures) {
  const Position pos = parse_fen("8/8/8/3PpP2/8/8/8/8 w - e6 0 1");
  const auto moves = pseudo_legal_moves(pos);

  EXPECT_EQ(moves.size(), 4);
  EXPECT_EQ(
      std::count_if(moves.begin(), moves.end(), [](const Move& mv) { return mv.is_en_passant; }),
      2);
}

TEST(Movegen, RookCaptureOnCornerRemovesTheDefendersCastlingRight) {
  // Capturing the a8 rook ends black's queenside castling, and moving the a1
  // rook to get there ends white's.
  Position pos = parse_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  const auto moves = legal_moves(pos);

  const auto capture = std::ranges::find_if(
      moves, [](const Move& mv) { return mv.from == Square::A1 && mv.to == Square::A8; });
  ASSERT_NE(capture, moves.end());

  pos.make_move(*capture);

  EXPECT_FALSE(pos.castling_rights.has(CastlingRight::BlackQueen));
  EXPECT_FALSE(pos.castling_rights.has(CastlingRight::WhiteQueen));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::BlackKing));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::WhiteKing));
}

TEST(Movegen, PromotionCaptureOnCornerRemovesTheDefendersCastlingRight) {
  // The rook leaves a8 without any rook of ours ever standing there, so the
  // castling right has to be cleared from the destination square.
  Position pos = parse_fen("r3k2r/1P6/8/8/8/8/8/R3K2R w KQkq - 0 1");
  const auto moves = legal_moves(pos);

  const auto promotion_capture = std::ranges::find_if(moves, [](const Move& mv) {
    return mv.from == Square::B7 && mv.to == Square::A8 && mv.promotion_piece == Piece::WQ;
  });
  ASSERT_NE(promotion_capture, moves.end());

  pos.make_move(*promotion_capture);

  EXPECT_FALSE(pos.castling_rights.has(CastlingRight::BlackQueen));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::BlackKing));
}

TEST(Movegen, EnPassantCaptureExposingTheKingAlongTheRankIsIllegal) {
  // Capturing en passant is the only move that empties two squares on the rank
  // it leaves: both pawns disappear from the fifth rank, opening a line from
  // the black rook on h5 straight to the white king on a5.
  const Position pos = parse_fen("8/8/8/K2pP2r/8/8/8/7k w - d6 0 1");

  const auto is_en_passant = [](const Move& mv) { return mv.is_en_passant; };
  EXPECT_EQ(std::ranges::count_if(pseudo_legal_moves(pos), is_en_passant), 1);
  EXPECT_EQ(std::ranges::count_if(legal_moves(pos), is_en_passant), 0);
}

TEST(Movegen, IgnoreFriendlyPieceCaptures) {
  assert_pseudo_legal_move_count("8/8/5p2/5P2/3N4/8/8/8 w - - 0 1", 7);
}

// Perft -------------------------------------------------------------------

TEST(Perft, FixturesMatch) {
  assert_perft_fixtures_match(PerftFixtureSet::Everyday);
}

// The deepest positions count millions of nodes, which takes a minute or more
// in this build because every move is checked by ASan and UBSan. They stay
// opt-in so the everyday suite remains quick.
TEST(Perft, SlowFixturesMatch) {
  if (std::getenv("C3_SLOW_PERFT") == nullptr) {
    GTEST_SKIP() << "set C3_SLOW_PERFT=1 to run the deep perft fixtures";
  }

  assert_perft_fixtures_match(PerftFixtureSet::Slow);
}
