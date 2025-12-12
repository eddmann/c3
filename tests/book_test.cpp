#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "c3/book.hpp"
#include "c3/position.hpp"

using namespace c3;

// =============================================================================
// POLYGLOT HASH TESTS
// =============================================================================
// Known Polyglot hash values from the official specification

TEST(PolyglotHash, StartPosition) {
  const Position pos = Position::startpos();
  // Known Polyglot hash for start position
  EXPECT_EQ(OpeningBook::compute_polyglot_key(pos), 0x463B96181691FC9CULL);
}

TEST(PolyglotHash, AfterE2E4) {
  Position pos = Position::startpos();
  // Apply e2e4
  Move e2e4{
      .piece = Piece::WP,
      .from = Square::E2,
      .to = Square::E4,
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  };
  pos.make_move(e2e4);

  // Known Polyglot hash after 1.e4
  EXPECT_EQ(OpeningBook::compute_polyglot_key(pos), 0x823C9B50FD114196ULL);
}

TEST(PolyglotHash, AfterE2E4_D7D5) {
  Position pos = Position::startpos();

  // Apply 1.e4
  Move e2e4{
      .piece = Piece::WP,
      .from = Square::E2,
      .to = Square::E4,
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  };
  pos.make_move(e2e4);

  // Apply 1...d5
  Move d7d5{
      .piece = Piece::BP,
      .from = Square::D7,
      .to = Square::D5,
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  };
  pos.make_move(d7d5);

  // Known Polyglot hash after 1.e4 d5
  EXPECT_EQ(OpeningBook::compute_polyglot_key(pos), 0x0756B94461C50FB0ULL);
}

TEST(PolyglotHash, NoCastlingRights) {
  // Position with no castling rights
  const auto pos = Position::from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w - - 0 1");
  // Should not include any castling hashes
  const std::uint64_t key = OpeningBook::compute_polyglot_key(pos);
  EXPECT_NE(key, 0ULL); // Just verify it computes something
}

TEST(PolyglotHash, EnPassantOnlyWhenCapturePossible) {
  // Position where en passant square exists but no pawn can capture
  // After 1.e4 e6 2.e5 d5, en passant square is d6 but white pawn on e5 can capture
  Position pos =
      Position::from_fen("rnbqkbnr/ppp2ppp/4p3/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");
  const std::uint64_t key_with_ep = OpeningBook::compute_polyglot_key(pos);

  // Position without en passant square
  Position pos_no_ep =
      Position::from_fen("rnbqkbnr/ppp2ppp/4p3/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3");
  const std::uint64_t key_without_ep = OpeningBook::compute_polyglot_key(pos_no_ep);

  // Keys should be different because e5 pawn CAN capture on d6
  EXPECT_NE(key_with_ep, key_without_ep);
}

TEST(PolyglotHash, EnPassantNotHashedWhenNoCapturer) {
  // Position where en passant square exists but no pawn can actually capture
  // No white pawn is adjacent to file e to capture on e6
  Position pos = Position::from_fen("rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPPPPPP/RNBQKBNR w KQkq e6 0 2");
  const std::uint64_t key_with_ep = OpeningBook::compute_polyglot_key(pos);

  // Same position without en passant
  Position pos_no_ep =
      Position::from_fen("rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 2");
  const std::uint64_t key_no_ep = OpeningBook::compute_polyglot_key(pos_no_ep);

  // Should be the same because no white pawn can capture on e6
  EXPECT_EQ(key_with_ep, key_no_ep);
}

// =============================================================================
// BOOK LOADING TESTS
// =============================================================================

class BookLoadingTest : public ::testing::Test {
protected:
  std::filesystem::path test_dir_;

  void SetUp() override {
    // Create a temporary directory for test files
    test_dir_ = std::filesystem::temp_directory_path() / "c3_book_test";
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  // Helper to create a test book file with given entries
  void create_book_file(const std::filesystem::path& path,
                        const std::vector<PolyglotEntry>& entries) {
    std::ofstream file(path, std::ios::binary);
    for (const auto& entry : entries) {
      // Write big-endian
      std::uint8_t buf[16];
      buf[0] = static_cast<std::uint8_t>(entry.key >> 56);
      buf[1] = static_cast<std::uint8_t>(entry.key >> 48);
      buf[2] = static_cast<std::uint8_t>(entry.key >> 40);
      buf[3] = static_cast<std::uint8_t>(entry.key >> 32);
      buf[4] = static_cast<std::uint8_t>(entry.key >> 24);
      buf[5] = static_cast<std::uint8_t>(entry.key >> 16);
      buf[6] = static_cast<std::uint8_t>(entry.key >> 8);
      buf[7] = static_cast<std::uint8_t>(entry.key);
      buf[8] = static_cast<std::uint8_t>(entry.move >> 8);
      buf[9] = static_cast<std::uint8_t>(entry.move);
      buf[10] = static_cast<std::uint8_t>(entry.weight >> 8);
      buf[11] = static_cast<std::uint8_t>(entry.weight);
      buf[12] = static_cast<std::uint8_t>(entry.learn >> 24);
      buf[13] = static_cast<std::uint8_t>(entry.learn >> 16);
      buf[14] = static_cast<std::uint8_t>(entry.learn >> 8);
      buf[15] = static_cast<std::uint8_t>(entry.learn);
      file.write(reinterpret_cast<const char*>(buf), 16);
    }
  }

  // Encode a move in Polyglot format
  static std::uint16_t encode_move(std::uint8_t from_file, std::uint8_t from_rank,
                                   std::uint8_t to_file, std::uint8_t to_rank,
                                   std::uint8_t promo = 0) {
    return static_cast<std::uint16_t>(to_file | (to_rank << 3) | (from_file << 6) |
                                      (from_rank << 9) | (promo << 12));
  }
};

TEST_F(BookLoadingTest, LoadNonexistentFile) {
  OpeningBook book;
  EXPECT_FALSE(book.load(test_dir_ / "nonexistent.bin"));
  EXPECT_FALSE(book.is_loaded());
}

TEST_F(BookLoadingTest, LoadEmptyFile) {
  const auto path = test_dir_ / "empty.bin";
  std::ofstream(path, std::ios::binary); // Create empty file

  OpeningBook book;
  EXPECT_TRUE(book.load(path));
  EXPECT_TRUE(book.is_loaded());

  // Probing should return nullopt
  EXPECT_FALSE(book.probe(Position::startpos()).has_value());
}

TEST_F(BookLoadingTest, LoadInvalidSizeFile) {
  const auto path = test_dir_ / "invalid.bin";
  {
    std::ofstream file(path, std::ios::binary);
    const char data[15] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    file.write(data, sizeof(data)); // 15 bytes, not multiple of 16
  } // Close file before loading

  OpeningBook book;
  EXPECT_FALSE(book.load(path));
  EXPECT_FALSE(book.is_loaded());
}

TEST_F(BookLoadingTest, LoadValidBookFile) {
  const auto path = test_dir_ / "valid.bin";

  // Create a book with one entry for start position: e2e4
  const std::uint64_t start_key = 0x463B96181691FC9CULL;
  const std::uint16_t e2e4_encoded = encode_move(4, 1, 4, 3); // e2 to e4

  std::vector<PolyglotEntry> entries = {{start_key, e2e4_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  EXPECT_TRUE(book.load(path));
  EXPECT_TRUE(book.is_loaded());
}

TEST_F(BookLoadingTest, Unload) {
  const auto path = test_dir_ / "test.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;
  const std::uint16_t e2e4_encoded = encode_move(4, 1, 4, 3);

  std::vector<PolyglotEntry> entries = {{start_key, e2e4_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  EXPECT_TRUE(book.load(path));
  EXPECT_TRUE(book.is_loaded());

  book.unload();
  EXPECT_FALSE(book.is_loaded());
  EXPECT_FALSE(book.probe(Position::startpos()).has_value());
}

// =============================================================================
// MOVE DECODING TESTS
// =============================================================================

TEST_F(BookLoadingTest, DecodeNormalMove_E2E4) {
  const auto path = test_dir_ / "e2e4.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;
  const std::uint16_t e2e4_encoded =
      encode_move(4, 1, 4, 3); // e2(file=4,rank=1) to e4(file=4,rank=3)

  std::vector<PolyglotEntry> entries = {{start_key, e2e4_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(Position::startpos());
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, Square::E2);
  EXPECT_EQ(move->to, Square::E4);
  EXPECT_EQ(move->piece, Piece::WP);
  EXPECT_FALSE(move->promotion_piece.has_value());
}

TEST_F(BookLoadingTest, DecodeNormalMove_D2D4) {
  const auto path = test_dir_ / "d2d4.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;
  const std::uint16_t d2d4_encoded = encode_move(3, 1, 3, 3); // d2 to d4

  std::vector<PolyglotEntry> entries = {{start_key, d2d4_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(Position::startpos());
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, Square::D2);
  EXPECT_EQ(move->to, Square::D4);
  EXPECT_EQ(move->piece, Piece::WP);
}

TEST_F(BookLoadingTest, DecodeCastlingKingside) {
  // Position where white can castle kingside
  Position pos = Position::from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
  const std::uint64_t key = OpeningBook::compute_polyglot_key(pos);

  const auto path = test_dir_ / "castle_ks.bin";
  // Polyglot encodes castling as king to rook square: e1h1 for O-O
  const std::uint16_t castle_encoded = encode_move(4, 0, 7, 0); // e1 to h1

  std::vector<PolyglotEntry> entries = {{key, castle_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(pos);
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, Square::E1);
  EXPECT_EQ(move->to, Square::G1); // Decoded to final king square, not rook square
  EXPECT_EQ(move->piece, Piece::WK);
  EXPECT_TRUE(move->is_castling());
}

TEST_F(BookLoadingTest, DecodeCastlingQueenside) {
  Position pos = Position::from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
  const std::uint64_t key = OpeningBook::compute_polyglot_key(pos);

  const auto path = test_dir_ / "castle_qs.bin";
  // Polyglot encodes O-O-O as e1a1
  const std::uint16_t castle_encoded = encode_move(4, 0, 0, 0); // e1 to a1

  std::vector<PolyglotEntry> entries = {{key, castle_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(pos);
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, Square::E1);
  EXPECT_EQ(move->to, Square::C1); // Decoded to c1, not a1
  EXPECT_EQ(move->piece, Piece::WK);
  EXPECT_TRUE(move->is_castling());
}

TEST_F(BookLoadingTest, DecodePromotion) {
  // Position with pawn about to promote
  Position pos = Position::from_fen("8/P7/8/8/8/8/8/4K2k w - - 0 1");
  const std::uint64_t key = OpeningBook::compute_polyglot_key(pos);

  const auto path = test_dir_ / "promo.bin";
  // a7 to a8 with queen promotion (promo=4)
  const std::uint16_t promo_encoded = encode_move(0, 6, 0, 7, 4); // a7 to a8, queen

  std::vector<PolyglotEntry> entries = {{key, promo_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(pos);
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, Square::A7);
  EXPECT_EQ(move->to, Square::A8);
  EXPECT_EQ(move->piece, Piece::WP);
  ASSERT_TRUE(move->promotion_piece.has_value());
  EXPECT_EQ(*move->promotion_piece, Piece::WQ);
}

TEST_F(BookLoadingTest, DecodeKnightPromotion) {
  Position pos = Position::from_fen("8/P7/8/8/8/8/8/4K2k w - - 0 1");
  const std::uint64_t key = OpeningBook::compute_polyglot_key(pos);

  const auto path = test_dir_ / "promo_knight.bin";
  // a7 to a8 with knight promotion (promo=1)
  const std::uint16_t promo_encoded = encode_move(0, 6, 0, 7, 1);

  std::vector<PolyglotEntry> entries = {{key, promo_encoded, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto move = book.probe(pos);
  ASSERT_TRUE(move.has_value());
  ASSERT_TRUE(move->promotion_piece.has_value());
  EXPECT_EQ(*move->promotion_piece, Piece::WN);
}

// =============================================================================
// WEIGHTED SELECTION TESTS
// =============================================================================

TEST_F(BookLoadingTest, WeightedSelectionReturnsMove) {
  const auto path = test_dir_ / "weighted.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;

  // Multiple moves with different weights
  const std::uint16_t e2e4 = encode_move(4, 1, 4, 3);
  const std::uint16_t d2d4 = encode_move(3, 1, 3, 3);
  const std::uint16_t c2c4 = encode_move(2, 1, 2, 3);

  std::vector<PolyglotEntry> entries = {
      {start_key, e2e4, 100, 0},
      {start_key, d2d4, 50, 0},
      {start_key, c2c4, 25, 0},
  };
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  // Should always return a valid move
  const auto move = book.probe(Position::startpos());
  ASSERT_TRUE(move.has_value());

  // Move should be one of the book moves
  EXPECT_TRUE(move->from == Square::E2 || move->from == Square::D2 || move->from == Square::C2);
}

TEST_F(BookLoadingTest, ProbeAllReturnsAllMoves) {
  const auto path = test_dir_ / "multi.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;

  const std::uint16_t e2e4 = encode_move(4, 1, 4, 3);
  const std::uint16_t d2d4 = encode_move(3, 1, 3, 3);

  std::vector<PolyglotEntry> entries = {
      {start_key, e2e4, 100, 0},
      {start_key, d2d4, 50, 0},
  };
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  const auto all_moves = book.probe_all(Position::startpos());
  ASSERT_EQ(all_moves.size(), 2);

  // Check moves are returned with weights
  bool found_e4 = false, found_d4 = false;
  for (const auto& [move, weight] : all_moves) {
    if (move.from == Square::E2 && move.to == Square::E4) {
      found_e4 = true;
      EXPECT_EQ(weight, 100);
    }
    if (move.from == Square::D2 && move.to == Square::D4) {
      found_d4 = true;
      EXPECT_EQ(weight, 50);
    }
  }
  EXPECT_TRUE(found_e4);
  EXPECT_TRUE(found_d4);
}

TEST_F(BookLoadingTest, ProbeNoMatch) {
  const auto path = test_dir_ / "no_match.bin";

  // Create entry for a different position
  const std::uint64_t other_key = 0x1234567890ABCDEFULL;
  const std::uint16_t some_move = encode_move(0, 0, 0, 1);

  std::vector<PolyglotEntry> entries = {{other_key, some_move, 100, 0}};
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  // Probing start position should not find a match
  EXPECT_FALSE(book.probe(Position::startpos()).has_value());
}

TEST_F(BookLoadingTest, ZeroWeightsFallbackToUniform) {
  const auto path = test_dir_ / "zero_weights.bin";
  const std::uint64_t start_key = 0x463B96181691FC9CULL;

  const std::uint16_t e2e4 = encode_move(4, 1, 4, 3);
  const std::uint16_t d2d4 = encode_move(3, 1, 3, 3);

  // All weights are zero
  std::vector<PolyglotEntry> entries = {
      {start_key, e2e4, 0, 0},
      {start_key, d2d4, 0, 0},
  };
  create_book_file(path, entries);

  OpeningBook book;
  ASSERT_TRUE(book.load(path));

  // Should still return a move (uniform random)
  const auto move = book.probe(Position::startpos());
  ASSERT_TRUE(move.has_value());
}
