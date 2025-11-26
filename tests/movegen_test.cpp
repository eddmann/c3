#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <string_view>

#include "c3/attacks.hpp"
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
  Position pos = parse_fen(fen);
  std::size_t legal = 0;

  for (const auto& mv : pseudo_legal_moves(pos)) {
    pos.make_move(mv);

    if (!is_in_check(pos.opponent_colour(), pos.board)) {
      ++legal;
    }

    pos.unmake_move(mv);
  }

  EXPECT_EQ(legal, count) << fen;
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

} // namespace

// Attack table coverage ---------------------------------------------------

TEST(Attacks, DetectCheck) {
  Board board = Board::empty();
  board.put_piece(Piece::BK, Square::E8);
  board.put_piece(Piece::WN, Square::D6);

  EXPECT_TRUE(is_in_check(Colour::Black, board));
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

TEST(Movegen, IgnoreFriendlyPieceCaptures) {
  assert_pseudo_legal_move_count("8/8/5p2/5P2/3N4/8/8/8 w - - 0 1", 7);
}

// Perft -------------------------------------------------------------------

TEST(Perft, FixturesMatch) {
  const auto records = fixtures::load_perft(fixtures::perft_path());

  for (const auto& record : records) {
    Position pos = Position::from_fen(record.fen);
    const auto nodes = perft(pos, static_cast<std::uint8_t>(record.depth));
    EXPECT_EQ(record.nodes, nodes) << record.name;
  }
}
