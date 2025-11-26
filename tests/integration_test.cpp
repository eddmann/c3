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

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

std::string to_uci(const Move& mv) {
  auto sq_to_str = [](Square sq) {
    std::string out;
    out.push_back(static_cast<char>('a' + sq.file()));
    out.push_back(static_cast<char>('1' + sq.rank()));
    return out;
  };

  std::string uci = sq_to_str(mv.from) + sq_to_str(mv.to);
  if (mv.promotion_piece.has_value()) {
    uci.push_back(static_cast<char>(std::tolower(to_char(*mv.promotion_piece))));
  }
  return uci;
}

} // namespace

// -----------------------------------------------------------------------------
// Full game integration
// -----------------------------------------------------------------------------

TEST(Integration, PlayMultipleMoves) {
  // Play several moves (engine vs itself) to verify integration works
  Engine engine;

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2; // Fast search

  constexpr int max_moves = 50;
  int moves_played = 0;

  while (moves_played < max_moves) {
    const auto moves = pseudo_legal_moves(engine.position());

    if (moves.empty()) {
      // Checkmate or stalemate - stop
      break;
    }

    // Search and play
    const auto result = engine.search(limits, reporter);

    if (result.pv.empty()) {
      break;
    }

    engine.apply_move(result.pv[0]);
    moves_played++;
  }

  // We should have played several moves
  EXPECT_GE(moves_played, 5);
}

TEST(Integration, DefendsAgainstFoolsMate) {
  // Position after 1.f3 e5 2.g4 - black can play Qh4#
  Engine engine;
  engine.set_position_from_fen("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  const auto result = engine.search(limits, reporter);

  // Black should find Qh4#
  ASSERT_FALSE(result.pv.empty());
  EXPECT_EQ(to_uci(result.pv[0]), "d8h4");
}

TEST(Integration, SearchIsDeterministic) {
  // Same position with same limits should give same result
  Engine engine1;
  Engine engine2;

  const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
  engine1.set_position_from_fen(fen);
  engine2.set_position_from_fen(fen);

  search::NullReporter reporter1;
  search::NullReporter reporter2;
  search::Limits limits;
  limits.depth = 3;

  const auto result1 = engine1.search(limits, reporter1);
  const auto result2 = engine2.search(limits, reporter2);

  // Results should be identical
  EXPECT_EQ(result1.depth, result2.depth);
  EXPECT_EQ(result1.eval, result2.eval);
  ASSERT_EQ(result1.pv.size(), result2.pv.size());
  for (size_t i = 0; i < result1.pv.size(); ++i) {
    EXPECT_EQ(result1.pv[i], result2.pv[i]) << "PV differs at index " << i;
  }
}

TEST(Integration, ZobristConsistencyThroughGame) {
  // Play 20 legal moves and verify Zobrist key stays in sync
  Position pos = Position::startpos();

  for (int i = 0; i < 20; ++i) {
    const auto moves = pseudo_legal_moves(pos);
    if (moves.empty()) {
      break;
    }

    // Pick the first legal move
    pos.make_move(moves[0]);

    // Zobrist key should match recomputed key
    EXPECT_EQ(pos.key, pos.compute_key()) << "Zobrist mismatch after move " << (i + 1);
  }
}

TEST(Integration, MakeUnmakeRestoresPosition) {
  // For each legal move in Kiwipete, make and unmake should restore position exactly
  const std::string kiwipete =
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
  Position pos = parse(kiwipete);

  const auto original_fen = pos.to_fen();
  const auto original_key = pos.key;

  const auto moves = pseudo_legal_moves(pos);
  ASSERT_FALSE(moves.empty());

  for (const auto& mv : moves) {
    pos.make_move(mv);
    pos.unmake_move(mv);

    EXPECT_EQ(pos.to_fen(), original_fen) << "Position changed after make/unmake of " << to_uci(mv);
    EXPECT_EQ(pos.key, original_key) << "Zobrist key changed after make/unmake of " << to_uci(mv);
  }
}
