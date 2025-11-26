#include <gtest/gtest.h>

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
