#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string_view>

#include "c3/bitboard.hpp"
#include "c3/colour.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

using namespace c3;

TEST(Square, CreateFromFileAndRank) {
  EXPECT_EQ(Square::from_file_and_rank(0, 0), Square::A1);
  EXPECT_EQ(Square::from_file_and_rank(7, 7), Square::H8);
  EXPECT_EQ(Square::from_file_and_rank(1, 4), Square::from_index(33));
}

TEST(Square, ParseAlgebraicNotation) {
  const auto a1 = Square::parse("a1");
  const auto h8 = Square::parse("h8");
  const auto b5 = Square::parse("b5");

  ASSERT_TRUE(a1.has_value());
  ASSERT_TRUE(h8.has_value());
  ASSERT_TRUE(b5.has_value());

  EXPECT_EQ(*a1, Square::A1);
  EXPECT_EQ(*h8, Square::H8);
  EXPECT_EQ(*b5, Square::from_index(33));
}

TEST(Square, RejectInvalidAlgebraicNotation) {
  for (std::string_view algebraic : {"", "a", "a1b", "a9", "i1"}) {
    EXPECT_FALSE(Square::parse(algebraic).has_value());
  }
}

TEST(Square, FirstOccupiedBitInBitboard) {
  Bitboard bitboard = Square::A1 | Square::A8;
  EXPECT_EQ(Square::first_occupied(bitboard), Square::A1);
}

TEST(Square, LastOccupiedBitInBitboard) {
  Bitboard bitboard = Square::A1 | Square::A8;
  EXPECT_EQ(Square::last_occupied(bitboard), Square::A8);
}

TEST(Square, PopFirstOccupiedConsumesBitboard) {
  Bitboard bitboard = Square::A1 | Square::A8;

  EXPECT_EQ(Square::pop_first_occupied(bitboard), Square::A1);
  EXPECT_EQ(bitboard, Bitboard(Square::A8));

  EXPECT_EQ(Square::pop_first_occupied(bitboard), Square::A8);
  EXPECT_EQ(bitboard, 0U);
}

TEST(Square, AdvanceGivenColour) {
  EXPECT_EQ(Square::E4.advance(Colour::White), Square::E5);
  EXPECT_EQ(Square::E4.advance(Colour::Black), Square::E3);
}

TEST(Square, BackRankAndCornerDetection) {
  EXPECT_TRUE(Square::A1.is_back_rank());
  EXPECT_TRUE(Square::H8.is_back_rank());
  EXPECT_FALSE(Square::E4.is_back_rank());

  EXPECT_TRUE(Square::A1.is_corner());
  EXPECT_TRUE(Square::H1.is_corner());
  EXPECT_TRUE(Square::A8.is_corner());
  EXPECT_TRUE(Square::H8.is_corner());
  EXPECT_FALSE(Square::B2.is_corner());
}

TEST(Bitboard, FileMasksMatchFiles) {
  EXPECT_NE(FILE_MASKS[0] & Bitboard(Square::A1), 0U);
  EXPECT_EQ(FILE_MASKS[0] & Bitboard(Square::B1), 0U);
  EXPECT_NE(FILE_MASKS[7] & Bitboard(Square::H8), 0U);
  EXPECT_EQ(FILE_MASKS[7] & Bitboard(Square::G8), 0U);
}

TEST(Piece, ColourMapping) {
  EXPECT_EQ(colour(Piece::WP), Colour::White);
  EXPECT_EQ(colour(Piece::WK), Colour::White);
  EXPECT_EQ(colour(Piece::BP), Colour::Black);
  EXPECT_EQ(colour(Piece::BQ), Colour::Black);
}

TEST(Piece, FactoriesByColour) {
  EXPECT_EQ(pawn(Colour::White), Piece::WP);
  EXPECT_EQ(pawn(Colour::Black), Piece::BP);
  EXPECT_EQ(queen(Colour::White), Piece::WQ);
  EXPECT_EQ(queen(Colour::Black), Piece::BQ);
  EXPECT_EQ(king(Colour::White), Piece::WK);
  EXPECT_EQ(king(Colour::Black), Piece::BK);
}

TEST(Piece, CollectionsMatchExpectedOrdering) {
  const auto& all = all_pieces();
  ASSERT_EQ(all.size(), 12U);
  EXPECT_EQ(all[0], Piece::WP);
  EXPECT_EQ(all[5], Piece::WK);
  EXPECT_EQ(all[6], Piece::BP);
  EXPECT_EQ(all[11], Piece::BK);

  const auto& white = pieces_for(Colour::White);
  ASSERT_EQ(white.size(), 6U);
  EXPECT_EQ(white[0], Piece::WP);
  EXPECT_EQ(white[5], Piece::WK);

  const auto& black = pieces_for(Colour::Black);
  ASSERT_EQ(black.size(), 6U);
  EXPECT_EQ(black[0], Piece::BP);
  EXPECT_EQ(black[5], Piece::BK);

  const auto& white_promos = promotions_for(Colour::White);
  const auto& black_promos = promotions_for(Colour::Black);
  ASSERT_EQ(white_promos.size(), 4U);
  ASSERT_EQ(black_promos.size(), 4U);
  EXPECT_EQ(white_promos[0], Piece::WN);
  EXPECT_EQ(white_promos[3], Piece::WQ);
  EXPECT_EQ(black_promos[0], Piece::BN);
  EXPECT_EQ(black_promos[3], Piece::BQ);
}
