#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "c3/engine.hpp"
#include "c3/movegen.hpp"
#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/search.hpp"
#include "c3/square.hpp"

using namespace c3;
namespace search = c3::search;

namespace {

Move make_move(Piece piece, Square from, Square to, std::optional<Piece> captured = std::nullopt,
               std::optional<Piece> promotion = std::nullopt, bool is_en_passant = false) {
  return Move{piece, from, to, captured, promotion, is_en_passant};
}

bool is_legal_move(const Position& pos, const Move& mv) {
  const auto moves = pseudo_legal_moves(pos);
  return std::find(moves.begin(), moves.end(), mv) != moves.end();
}

} // namespace

// -----------------------------------------------------------------------------
// Constructor and initialization tests
// -----------------------------------------------------------------------------

TEST(Engine, StartsAtInitialPosition) {
  Engine engine;

  const auto& pos = engine.position();
  EXPECT_EQ(pos.colour_to_move, Colour::White);
  EXPECT_EQ(pos.board.piece_at(Square::E1), Piece::WK);
  EXPECT_EQ(pos.board.piece_at(Square::E8), Piece::BK);
  EXPECT_EQ(pos.board.piece_at(Square::E2), Piece::WP);
  EXPECT_EQ(pos.board.piece_at(Square::E7), Piece::BP);
  EXPECT_EQ(pos.full_move_counter, 1);
  EXPECT_EQ(pos.half_move_clock, 0);
}

TEST(Engine, NewGameResetsToStartPosition) {
  Engine engine;

  // Make some moves
  engine.apply_move(make_move(Piece::WP, Square::E2, Square::E4));
  engine.apply_move(make_move(Piece::BP, Square::E7, Square::E5));

  // Verify position changed
  EXPECT_EQ(engine.position().colour_to_move, Colour::White);
  EXPECT_FALSE(engine.position().board.has_piece_at(Square::E2));
  EXPECT_TRUE(engine.position().board.has_piece_at(Square::E4));

  // Reset
  engine.new_game();

  // Verify back to start
  const auto& pos = engine.position();
  EXPECT_EQ(pos.colour_to_move, Colour::White);
  EXPECT_EQ(pos.board.piece_at(Square::E2), Piece::WP);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E4));
  EXPECT_EQ(pos.full_move_counter, 1);
}

// -----------------------------------------------------------------------------
// Position setting tests
// -----------------------------------------------------------------------------

TEST(Engine, SetPositionFromFenKiwipete) {
  Engine engine;
  const std::string kiwipete =
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

  engine.set_position_from_fen(kiwipete);

  const auto& pos = engine.position();
  EXPECT_EQ(pos.colour_to_move, Colour::White);
  EXPECT_EQ(pos.board.piece_at(Square::E5), Piece::WN);
  EXPECT_EQ(pos.board.piece_at(Square::F3), Piece::WQ);
  EXPECT_EQ(pos.board.piece_at(Square::E7), Piece::BQ);
  EXPECT_EQ(pos.board.piece_at(Square::A6), Piece::BB);
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::WhiteKing));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::WhiteQueen));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::BlackKing));
  EXPECT_TRUE(pos.castling_rights.has(CastlingRight::BlackQueen));
}

TEST(Engine, SetPositionCopiesNotReferences) {
  Engine engine;
  Position original = Position::startpos();

  engine.set_position(original);

  // Modify the original
  original.make_move(make_move(Piece::WP, Square::E2, Square::E4));

  // Engine position should be unchanged
  EXPECT_EQ(engine.position().board.piece_at(Square::E2), Piece::WP);
  EXPECT_FALSE(engine.position().board.has_piece_at(Square::E4));
  EXPECT_EQ(engine.position().colour_to_move, Colour::White);
}

// -----------------------------------------------------------------------------
// Move application tests
// -----------------------------------------------------------------------------

TEST(Engine, ApplyMoveUpdatesPosition) {
  Engine engine;

  const Move e2e4 = make_move(Piece::WP, Square::E2, Square::E4);
  engine.apply_move(e2e4);

  const auto& pos = engine.position();
  EXPECT_EQ(pos.board.piece_at(Square::E4), Piece::WP);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E2));
  EXPECT_EQ(pos.colour_to_move, Colour::Black);
  EXPECT_EQ(pos.en_passant_square, Square::E3);
}

TEST(Engine, ApplyMovesPlaysSicilianDefense) {
  Engine engine;

  // 1. e4 c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6
  std::vector<Move> sicilian = {
      make_move(Piece::WP, Square::E2, Square::E4),
      make_move(Piece::BP, Square::C7, Square::C5),
      make_move(Piece::WN, Square::G1, Square::F3),
      make_move(Piece::BP, Square::D7, Square::D6),
      make_move(Piece::WP, Square::D2, Square::D4),
      make_move(Piece::BP, Square::C5, Square::D4, Piece::WP),
      make_move(Piece::WN, Square::F3, Square::D4, Piece::BP),
      make_move(Piece::BN, Square::G8, Square::F6),
  };

  engine.apply_moves(sicilian);

  const auto& pos = engine.position();
  EXPECT_EQ(pos.colour_to_move, Colour::White);
  EXPECT_EQ(pos.board.piece_at(Square::E4), Piece::WP);
  EXPECT_EQ(pos.board.piece_at(Square::D4), Piece::WN);
  EXPECT_EQ(pos.board.piece_at(Square::D6), Piece::BP);
  EXPECT_EQ(pos.board.piece_at(Square::F6), Piece::BN);
  EXPECT_FALSE(pos.board.has_piece_at(Square::C5));
  EXPECT_EQ(pos.full_move_counter, 5);
}

// -----------------------------------------------------------------------------
// Search tests
// -----------------------------------------------------------------------------

TEST(Engine, SearchDepth1ReturnsLegalMove) {
  Engine engine;

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 1;

  const auto result = engine.search(limits, reporter);

  EXPECT_EQ(result.depth, 1);
  ASSERT_FALSE(result.pv.empty());

  // The first move in the PV should be legal from the start position
  EXPECT_TRUE(is_legal_move(engine.position(), result.pv[0]));
}

TEST(Engine, SearchFindsObviousMaterialGain) {
  Engine engine;
  // Position where knight on f3 can capture free queen on d4
  // Knight from f3 can reach d4 (L-shape: 2 left, 1 up)
  engine.set_position_from_fen("4k3/8/8/8/3q4/5N2/8/4K3 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  const auto result = engine.search(limits, reporter);

  ASSERT_FALSE(result.pv.empty());
  const auto& best = result.pv[0];

  // Knight should capture queen: f3 to d4
  EXPECT_EQ(best.piece, Piece::WN);
  EXPECT_EQ(best.from, Square::F3);
  EXPECT_EQ(best.to, Square::D4);
  EXPECT_TRUE(best.captured_piece.has_value());
  EXPECT_EQ(*best.captured_piece, Piece::BQ);

  // Eval should be positive (material advantage after queen capture)
  EXPECT_GT(result.eval, 200);
}

TEST(Engine, SearchPositionUnchangedAfterSearch) {
  Engine engine;
  const std::string kiwipete =
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
  engine.set_position_from_fen(kiwipete);

  const auto fen_before = engine.position().to_fen();
  const auto key_before = engine.position().key;

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  engine.search(limits, reporter);

  EXPECT_EQ(engine.position().to_fen(), fen_before);
  EXPECT_EQ(engine.position().key, key_before);
}

// -----------------------------------------------------------------------------
// Configuration tests
// -----------------------------------------------------------------------------

TEST(Engine, SetHashSizeWorks) {
  Engine engine;

  // Set to minimum allowed size
  engine.set_hash_size_mb(search::TT_MIN_SIZE_MB);

  // Search should still work
  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  const auto result = engine.search(limits, reporter);
  EXPECT_GE(result.depth, 1);
  EXPECT_FALSE(result.pv.empty());
}
