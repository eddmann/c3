#include <gtest/gtest.h>

#include "c3/bitboard.hpp"
#include "c3/board.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

using namespace c3;

TEST(Board, PutAPieceOnTheBoard) {
  auto board = Board::empty();
  const Piece piece = Piece::WK;
  const Square square = Square::E1;

  board.put_piece(piece, square);

  const auto occupant = board.piece_at(square);
  ASSERT_TRUE(occupant.has_value());
  EXPECT_EQ(*occupant, piece);
  EXPECT_EQ(board.count_pieces(piece), 1U);
  EXPECT_EQ(board.pieces_by_colour(colour(piece)) & Bitboard(square), Bitboard(square));
}

TEST(Board, RemoveAPieceFromTheBoard) {
  auto board = Board::empty();
  const Square square = Square::E1;
  board.put_piece(Piece::WK, square);

  ASSERT_TRUE(board.has_piece_at(square));

  board.remove_piece(square);

  EXPECT_FALSE(board.has_piece_at(square));
}

TEST(Board, OccupancyReflectsEveryPieceOnTheBoard) {
  auto board = Board::empty();

  EXPECT_EQ(board.occupancy(), 0U);

  board.put_piece(Piece::WK, Square::E1);
  board.put_piece(Piece::BK, Square::E8);
  EXPECT_EQ(board.occupancy(), Bitboard(Square::E1) | Bitboard(Square::E8));
  EXPECT_TRUE(board.has_occupancy_at(Square::E8));

  board.remove_piece(Square::E1);
  EXPECT_EQ(board.occupancy(), Bitboard(Square::E8));
  EXPECT_FALSE(board.has_occupancy_at(Square::E1));

  board.remove_piece(Square::E8);
  EXPECT_EQ(board.occupancy(), 0U);
}

// Overwriting an occupant would leave the vacated piece's bitboard bit set: the
// mailbox would report the new piece while the bitboards still list the old one,
// producing a "phantom" piece that only surfaces plies later. Debug builds trip
// an assertion instead so the offending call site is the one in the backtrace.
TEST(BoardDeathTest, PutPieceRefusesAnOccupiedSquare) {
  auto board = Board::empty();
  board.put_piece(Piece::WP, Square::E4);

  EXPECT_DEBUG_DEATH(board.put_piece(Piece::BQ, Square::E4), "put_piece expects an empty square");
}

TEST(BoardDeathTest, RemovePieceRefusesAnEmptySquare) {
  auto board = Board::empty();

  EXPECT_DEBUG_DEATH(board.remove_piece(Square::E4), "remove_piece expects an occupied square");
}
