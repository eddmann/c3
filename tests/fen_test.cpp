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
  expect_parse_error("w - - 0 1", "FEN must contain 4 or 6 parts, got 5");
  expect_parse_error("8/8/8/8/8/8/8/8 w - - 0 1 extra", "FEN must contain 4 or 6 parts, got 7");
}

// EPD opening books (and many puzzle collections) omit the two clocks, so a
// four-field FEN is accepted with the conventional defaults.
TEST(Fen, ParseAFourFieldEpdStylePosition) {
  const auto pos = Position::from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq -");

  EXPECT_EQ(pos.half_move_clock, 0);
  EXPECT_EQ(pos.full_move_counter, 1);
  EXPECT_EQ(pos.castling_rights, CastlingRights::all());
}

TEST(Fen, ParseErrorWithWrongNumberOfRows) {
  expect_parse_error("8/8 w - - 0 1", "board must contain 8 rows, got 2");
  expect_parse_error("8/8/8/8/8/8/8/8/1 w - - 0 1", "board must contain 8 rows, got 9");
}

TEST(Fen, ParseErrorWithWrongNumberOfSquares) {
  expect_parse_error("8/8/8/8/8/8/8/7 w - - 0 1", "rank must contain exactly 8 files");
  expect_parse_error("8/8/8/8/8/8/8/9 w - - 0 1", "rank must contain exactly 8 files");
}

// "0" is not a shorter way of writing no empty squares; allowing it would let
// one position be spelled several ways.
TEST(Fen, ParseErrorWithAZeroEmptySquareCount) {
  expect_parse_error("8/8/8/8/8/8/8/08 w - - 0 1", "empty square count must be between 1 and 8");
}

// A rank that overruns its eight files used to keep writing past square 63,
// shifting a bitboard by 64 (undefined behaviour) before the total-square check
// ever ran. Each rank is now bounded as it is filled.
TEST(Fen, ParseErrorWithARankThatOverrunsItsFiles) {
  expect_parse_error("PPPPPPPPP/8/8/8/8/8/8/8 w - - 0 1", "rank must contain exactly 8 files");
  expect_parse_error("44P/8/8/8/8/8/8/8 w - - 0 1", "rank must contain exactly 8 files");
  expect_parse_error("8/8/8/8/8/8/8/44P w - - 0 1", "rank must contain exactly 8 files");
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
  const auto pos = Position::from_fen("r3k2r/8/8/8/8/8/8/R3K2R w Kq - 0 1");

  EXPECT_EQ(pos.castling_rights,
            CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::BlackQueen}));
}

TEST(Fen, ParseWithAllCastlingRights) {
  const auto pos = Position::from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

  EXPECT_EQ(pos.castling_rights, CastlingRights::all());
}

TEST(Fen, ParseErrorWithInvalidCastlingRights) {
  expect_parse_error("8/8/8/8/8/8/8/8 w K- - 0 1", "invalid castling rights");
}

// Castling rights are a claim about where the king and rook still stand. A FEN
// that claims them without the pieces to back it up is rejected rather than
// silently trimmed, because a silently trimmed position no longer round-trips.
TEST(Fen, ParseErrorWithUnsupportedCastlingRights) {
  expect_parse_error("r3k2r/8/8/8/8/8/8/R6R w K - 0 1",
                     "castling right 'K' requires a white king on e1 and a white rook on h1");
  expect_parse_error("r3k2r/8/8/8/8/8/8/4K2R w Q - 0 1",
                     "castling right 'Q' requires a white king on e1 and a white rook on a1");
  expect_parse_error("r6r/8/8/8/8/8/8/R3K2R w k - 0 1",
                     "castling right 'k' requires a black king on e8 and a black rook on h8");
  expect_parse_error("4k2r/8/8/8/8/8/8/R3K2R w q - 0 1",
                     "castling right 'q' requires a black king on e8 and a black rook on a8");
}

TEST(Fen, ParseWithNoEnPassantSquare) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1");

  EXPECT_EQ(pos.en_passant_square, std::nullopt);
}

TEST(Fen, ParseWithEnPassantSquares) {
  const auto rank3 = Position::from_fen("8/8/8/8/5P2/8/8/8 b - f3 0 1");
  EXPECT_EQ(rank3.en_passant_square, Square::F3);

  const auto rank6 = Position::from_fen("8/8/8/5p2/8/8/8/8 w - f6 0 1");
  EXPECT_EQ(rank6.en_passant_square, Square::F6);
}

TEST(Fen, ParseErrorWithInvalidEnPassantSquare) {
  expect_parse_error("8/8/8/8/8/8/8/8 w - f4 0 1", "invalid en passant square");
}

// An en passant square is only meaningful when the double push that created it
// actually happened and the side that could answer it is the side to move.
TEST(Fen, ParseErrorWithUnsupportedEnPassantSquare) {
  expect_parse_error("8/8/8/8/8/8/8/8 w - f6 0 1",
                     "en passant square f6 requires a black pawn on f5 and white to move");
  expect_parse_error("8/8/8/5p2/8/8/8/8 b - f6 0 1",
                     "en passant square f6 requires a black pawn on f5 and white to move");
  expect_parse_error("8/8/8/8/8/8/8/8 b - f3 0 1",
                     "en passant square f3 requires a white pawn on f4 and black to move");
  expect_parse_error("8/8/8/8/5P2/8/8/8 w - f3 0 1",
                     "en passant square f3 requires a white pawn on f4 and black to move");
}

// The double push also has to have had somewhere to go: both the square it
// jumped over and the square it started on must still be empty. A capture onto
// an occupied en passant square would put two pieces on one square.
TEST(Fen, ParseErrorWithBlockedEnPassantSquare) {
  expect_parse_error("k7/8/5B2/4Pp2/8/8/8/K7 w - f6 0 1",
                     "en passant square f6 and the square f7 behind it must both be empty");
  expect_parse_error("k7/5b2/8/4Pp2/8/8/8/K7 w - f6 0 1",
                     "en passant square f6 and the square f7 behind it must both be empty");
  expect_parse_error("k7/8/8/8/4pP2/5B2/8/K7 b - f3 0 1",
                     "en passant square f3 and the square f2 behind it must both be empty");
  expect_parse_error("k7/8/8/8/4pP2/8/5b2/K7 b - f3 0 1",
                     "en passant square f3 and the square f2 behind it must both be empty");
}

// Pawns cannot exist on the ranks they promote on, so such a board is a typo
// rather than a position the engine should try to search.
TEST(Fen, ParseErrorWithPawnsOnAPromotionRank) {
  expect_parse_error("P7/8/8/8/8/8/8/8 w - - 0 1", "pawns cannot occupy the first or last rank");
  expect_parse_error("8/8/8/8/8/8/8/7p w - - 0 1", "pawns cannot occupy the first or last rank");
}

// Positions with no king at all are deliberately allowed: this engine's own
// tests isolate a single piece on an otherwise empty board. Two kings of the
// same colour, on the other hand, break every "find the king" lookup.
TEST(Fen, ParseErrorWithDuplicateKings) {
  expect_parse_error("8/8/8/8/8/8/8/K6K w - - 0 1", "there can be at most one white king");
  expect_parse_error("k6k/8/8/8/8/8/8/8 w - - 0 1", "there can be at most one black king");
}

TEST(Fen, ParseWithMoveCounters) {
  const auto pos = Position::from_fen("8/8/8/8/8/8/8/8 w - - 10 20");

  EXPECT_EQ(pos.half_move_clock, 10);
  EXPECT_EQ(pos.full_move_counter, 20);
}

// The clocks outgrow a single byte in any long game, so they are stored wide
// enough to survive one and to round-trip unchanged.
TEST(Fen, ParseAndSerialiseLargeMoveCounters) {
  const std::string fen = "8/8/8/8/8/8/8/8 w - - 300 500";
  const auto pos = Position::from_fen(fen);

  EXPECT_EQ(pos.half_move_clock, 300);
  EXPECT_EQ(pos.full_move_counter, 500);
  EXPECT_EQ(pos.to_fen(), fen);
}

TEST(Fen, ParseErrorWithInvalidMoveCounters) {
  expect_parse_error("8/8/8/8/8/8/8/8 w - - x 1", "invalid move counters");
  expect_parse_error("8/8/8/8/8/8/8/8 w - - 1 1x", "invalid move counters");
  expect_parse_error("8/8/8/8/8/8/8/8 w - - 0 100000", "invalid move counters");
  expect_parse_error("8/8/8/8/8/8/8/8 w - - 0 0", "full move counter must be at least 1");
}

TEST(Fen, RoundTripFixtures) {
  const auto records = c3::fixtures::load_perft(c3::fixtures::perft_path());
  ASSERT_FALSE(records.empty());

  for (const auto& record : records) {
    const auto pos = Position::from_fen(record.fen);
    EXPECT_EQ(pos.to_fen(), record.fen);
  }
}
