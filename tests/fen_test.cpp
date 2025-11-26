#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/square.hpp"
#include "fixtures.hpp"

using namespace c3;

namespace {

void expect_parse_error(const std::string& fen, const std::string& message) {
  try {
    (void)Position::from_fen(fen);
    FAIL() << "Expected parse error";
  } catch (const std::runtime_error& err) {
    EXPECT_EQ(err.what(), message);
  }
}

} // namespace

TEST(Fen, ParseAValidFen) {
  const auto pos = Position::from_fen(std::string(Position::START_POS_FEN));

  EXPECT_EQ(pos.board.piece_at(Square::E2), Piece::WP);
}

TEST(Fen, ParseErrorWithWrongNumberOfParts) {
  expect_parse_error("w - - 0 1", "FEN must contain 6 parts, got 5");
  expect_parse_error("8/8/8/8/8/8/8/8 w - - 0 1 extra", "FEN must contain 6 parts, got 7");
}

TEST(Fen, ParseErrorWithWrongNumberOfRows) {
  expect_parse_error("8/8 w - - 0 1", "board must contain 8 rows, got 2");
  expect_parse_error("8/8/8/8/8/8/8/8/1 w - - 0 1", "board must contain 8 rows, got 9");
}

TEST(Fen, ParseErrorWithWrongNumberOfSquares) {
  expect_parse_error("8/8/8/8/8/8/8/7 w - - 0 1", "board must contain 64 squares");
  expect_parse_error("8/8/8/8/8/8/8/9 w - - 0 1", "board must contain 64 squares");
}

TEST(Fen, ParseErrorWithInvalidPiece) {
  expect_parse_error("8/8/8/8/8/8/8/4a3 w - - 0 1", "invalid piece 'a'");
}

TEST(Fen, ParseWithWhiteToMove) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1");

  EXPECT_EQ(pos.colour_to_move, Colour::White);
}

TEST(Fen, ParseWithBlackToMove) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 b - - 0 1");

  EXPECT_EQ(pos.colour_to_move, Colour::Black);
}

TEST(Fen, ParseErrorWithInvalidColourToMove) {
  expect_parse_error("8/8/8/8/8/8/8/8 W - - 0 1", "invalid colour to move 'W'");
}

TEST(Fen, ParseWithNoCastlingRights) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1");

  EXPECT_EQ(pos.castling_rights, CastlingRights::none());
}

TEST(Fen, ParseWithPartialCastlingRights) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w Kq - 0 1");

  EXPECT_EQ(pos.castling_rights,
            CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::BlackQueen}));
}

TEST(Fen, ParseWithAllCastlingRights) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w KQkq - 0 1");

  EXPECT_EQ(pos.castling_rights, CastlingRights::all());
}

TEST(Fen, ParseErrorWithInvalidCastlingRights) {
  expect_parse_error("8/8/8/8/8/8/8/8 w K- - 0 1", "invalid castling rights");
}

TEST(Fen, ParseWithNoEnPassantSquare) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1");

  EXPECT_EQ(pos.en_passant_square, std::nullopt);
}

TEST(Fen, ParseWithEnPassantSquares) {
  const auto rank3 = Position::from_fen("8/8/8/8/8/8/8/8 w - f3 0 1");
  EXPECT_EQ(rank3.en_passant_square, Square::F3);

  const auto rank6 = Position::from_fen("8/8/8/8/8/8/8/8 w - f6 0 1");
  EXPECT_EQ(rank6.en_passant_square, Square::F6);
}

TEST(Fen, ParseErrorWithInvalidEnPassantSquare) {
  expect_parse_error("8/8/8/8/8/8/8/8 w - f4 0 1", "invalid en passant square");
}

TEST(Fen, ParseWithMoveCounters) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 10 20");

  EXPECT_EQ(pos.half_move_clock, 10);
  EXPECT_EQ(pos.full_move_counter, 20);
}

TEST(Fen, RoundTripFixtures) {
  const auto records = c3::fixtures::load_perft(c3::fixtures::perft_path());
  ASSERT_FALSE(records.empty());

  for (const auto& record : records) {
    const auto pos = Position::from_fen(record.fen);
    EXPECT_EQ(pos.to_fen(), record.fen);
  }
}
