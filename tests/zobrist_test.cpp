#include <gtest/gtest.h>

#include <cstddef>

#include "c3/position.hpp"
#include "c3/zobrist.hpp"
#include "fixtures.hpp"

using namespace c3;

TEST(Zobrist, MatchesFixtureKeys) {
  const auto records = fixtures::load_zobrist(fixtures::zobrist_path());

  for (const auto& record : records) {
    const auto pos = Position::from_fen(record.fen);
    EXPECT_EQ(pos.key, record.key) << "fixture mismatch for " << record.name;
  }
}

TEST(Zobrist, IncrementalToggleMatchesRecompute) {
  Position pos = Position::startpos();
  const auto original_key = pos.key;

  const Square from = Square::E2;
  const Square to = Square::E4;
  const Piece pawn_piece = Piece::WP;

  pos.board.remove_piece(from);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][from.index()];

  pos.board.put_piece(pawn_piece, to);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][to.index()];

  pos.en_passant_square = Square::from_file_and_rank(4, 2); // e3
  pos.colour_to_move = Colour::Black;
  pos.key ^= ZOBRIST.colour_to_move;

  EXPECT_EQ(pos.key, pos.compute_key());

  pos.key ^= ZOBRIST.colour_to_move;
  pos.colour_to_move = Colour::White;
  pos.en_passant_square = std::nullopt;

  pos.board.remove_piece(to);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][to.index()];

  pos.board.put_piece(pawn_piece, from);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][from.index()];

  EXPECT_EQ(pos.key, pos.compute_key());
  EXPECT_EQ(pos.key, original_key);
}
