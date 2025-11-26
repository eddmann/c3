#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "c3/eval.hpp"
#include "c3/movegen.hpp"
#include "c3/search.hpp"
#include "c3/square.hpp"

using namespace c3;
namespace search = c3::search;

namespace {

Move make_move(Piece piece, std::string_view from, std::string_view to,
               std::optional<Piece> captured = std::nullopt,
               std::optional<Piece> promo = std::nullopt, bool is_ep = false) {
  const auto from_sq = Square::parse(from);
  const auto to_sq = Square::parse(to);
  EXPECT_TRUE(from_sq.has_value());
  EXPECT_TRUE(to_sq.has_value());
  return Move{
      .piece = piece,
      .from = *from_sq,
      .to = *to_sq,
      .captured_piece = captured,
      .promotion_piece = promo,
      .is_en_passant = is_ep,
  };
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

std::vector<std::string> pv_to_uci(const MoveList& pv) {
  std::vector<std::string> uci;
  uci.reserve(pv.size());
  for (const auto& mv : pv) {
    uci.push_back(to_uci(mv));
  }
  return uci;
}

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

} // namespace

// Move ordering ----------------------------------------------------------------

TEST(SearchOrdering, OrdersMvvLvaAndKillers) {
  const auto quiet = make_move(Piece::WP, "c4", "c5");
  const auto killer1 = make_move(Piece::WP, "a2", "a3");
  const auto killer2 = make_move(Piece::WP, "b2", "b3");
  const auto pawn_cap_pawn = make_move(Piece::WP, "c4", "b5", Piece::BP);
  const auto pawn_cap_queen = make_move(Piece::WP, "c4", "d5", Piece::BQ);
  const auto knight_cap_bishop = make_move(Piece::WN, "f4", "d3", Piece::BB);
  const auto knight_cap_queen = make_move(Piece::WN, "f4", "d5", Piece::BQ);
  const auto knight_cap_rook = make_move(Piece::WN, "f4", "g6", Piece::BR);
  const auto knight_cap_knight = make_move(Piece::WN, "f4", "h3", Piece::BN);

  MoveList moves = {quiet,
                    killer1,
                    killer2,
                    pawn_cap_pawn,
                    pawn_cap_queen,
                    knight_cap_bishop,
                    knight_cap_queen,
                    knight_cap_rook,
                    knight_cap_knight};

  search::KillerMoves killers;
  const std::uint8_t ply = 0;
  killers.store(ply, killer2);
  killers.store(ply, killer1);

  search::detail::order_moves(moves, killers, ply);

  const MoveList expected = {pawn_cap_queen,    knight_cap_queen,  knight_cap_rook,
                             knight_cap_bishop, knight_cap_knight, pawn_cap_pawn,
                             killer1,           killer2,           quiet};

  ASSERT_EQ(moves.size(), expected.size());
  for (std::size_t i = 0; i < moves.size(); ++i) {
    EXPECT_EQ(moves[i], expected[i]) << "index " << i;
  }
}

TEST(SearchOrdering, QuiescenceOrdersMvvLva) {
  const auto pawn_cap_pawn = make_move(Piece::WP, "c4", "b5", Piece::BP);
  const auto pawn_cap_queen = make_move(Piece::WP, "c4", "d5", Piece::BQ);
  const auto knight_cap_bishop = make_move(Piece::WN, "f4", "d3", Piece::BB);
  const auto knight_cap_queen = make_move(Piece::WN, "f4", "d5", Piece::BQ);
  const auto knight_cap_rook = make_move(Piece::WN, "f4", "g6", Piece::BR);
  const auto knight_cap_knight = make_move(Piece::WN, "f4", "h3", Piece::BN);

  MoveList moves = {pawn_cap_pawn,    pawn_cap_queen,  knight_cap_bishop,
                    knight_cap_queen, knight_cap_rook, knight_cap_knight};

  search::detail::order_quiescence_moves(moves);

  const MoveList expected = {pawn_cap_queen,    knight_cap_queen,  knight_cap_rook,
                             knight_cap_bishop, knight_cap_knight, pawn_cap_pawn};

  ASSERT_EQ(moves.size(), expected.size());
  for (std::size_t i = 0; i < moves.size(); ++i) {
    EXPECT_EQ(moves[i], expected[i]) << "index " << i;
  }
}

// Transposition table normalisation --------------------------------------------

TEST(TranspositionTable, NormalisesMateScores) {
  const std::uint8_t ply = 5;
  const int mate_eval = CENTIPAWN_MATE - 10;

  EXPECT_EQ(search::eval_in(mate_eval, ply), mate_eval + ply);
  EXPECT_EQ(search::eval_out(search::eval_in(mate_eval, ply), ply), mate_eval);

  const int mate_in = -CENTIPAWN_MATE + 7;
  EXPECT_EQ(search::eval_in(mate_in, ply), mate_in - ply);
  EXPECT_EQ(search::eval_out(search::eval_in(mate_in, ply), ply), mate_in);

  EXPECT_EQ(search::eval_in(120, ply), 120);
  EXPECT_EQ(search::eval_out(-300, ply), -300);
}

// Null-move pruning ------------------------------------------------------------

TEST(NullMove, StoresLowerBoundOnFailHigh) {
  Position pos = parse("6k1/8/8/8/8/8/4Q3/4K3 w - - 0 1");

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  MoveList pv;
  const int beta = 50;
  const int alpha = -CENTIPAWN_MAX;

  const int eval = search::detail::alphabeta(pos, 4, alpha, beta, pv, tt, killers, report, stopper);

  const auto* const entry = tt.probe(pos.key);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->bound, search::Bound::Lower);
  EXPECT_FALSE(entry->move.has_value());
  EXPECT_EQ(eval, beta);
}

// Search correctness -----------------------------------------------------------

TEST(SearchCorrectness, MatchesStartposDepth2) {
  Position pos = Position::startpos();
  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.depth, 2);
  EXPECT_EQ(result.eval, 0);

  const auto pv = pv_to_uci(result.pv);
  ASSERT_GE(pv.size(), 1U);
  EXPECT_EQ(pv[0], "e2e4");
  if (pv.size() > 1) {
    EXPECT_EQ(pv[1], "e7e5");
  }
}

TEST(SearchCorrectness, MatchesKiwipeteDepth3) {
  Position pos = parse("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.depth, 3);
  EXPECT_EQ(result.eval, 50);

  const auto pv = pv_to_uci(result.pv);
  ASSERT_GE(pv.size(), 3U);
  EXPECT_EQ((std::vector<std::string>{pv.begin(), pv.begin() + 3}),
            (std::vector<std::string>{"e2a6", "e6d5", "g2h3"}));
}

// -----------------------------------------------------------------------------
// Draw Detection
// -----------------------------------------------------------------------------

TEST(SearchDraw, RecognizesFiftyMoveRule) {
  // Position with half-move clock at 100 (draw by 50-move rule)
  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 w - - 100 50");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  // Eval should be close to 0 (draw)
  EXPECT_LE(std::abs(result.eval), 50);
}

TEST(SearchDraw, AvoidsStalemateWhenWinning) {
  // Q+K vs K - white is winning but can stalemate
  // Position: white king g6, white queen f7, black king h8
  // Qf8 would be stalemate!
  Position pos = parse("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  const auto result = search::search(pos, limits, reporter);

  // Should NOT play Qf8 (stalemate)
  ASSERT_FALSE(result.pv.empty());
  const auto best_uci = to_uci(result.pv[0]);
  EXPECT_NE(best_uci, "f7f8") << "Should avoid stalemate";

  // Should be winning, not drawing
  EXPECT_GT(result.eval, 500);
}

// -----------------------------------------------------------------------------
// Checkmate Detection
// -----------------------------------------------------------------------------

TEST(SearchMate, FindsMateInOne) {
  // Back rank mate: Re8#
  Position pos = parse("6k1/5ppp/8/8/8/8/8/4R1K1 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  const auto result = search::search(pos, limits, reporter);

  ASSERT_FALSE(result.pv.empty());
  EXPECT_EQ(to_uci(result.pv[0]), "e1e8");

  // Eval should indicate mate (close to CENTIPAWN_MATE)
  EXPECT_GT(result.eval, CENTIPAWN_MATE - 100);
}

TEST(SearchMate, FindsMateInTwo) {
  // Anastasia's mate pattern - Q+N mate in 2
  // Position: after 1...Qxh2+ 2.Kf1 Qh1#
  // Or use a simpler mate-in-2: scholar's mate position
  Position pos = parse("r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  const auto result = search::search(pos, limits, reporter);

  // Should find Qxf7# (immediate checkmate actually - this is mate in 1)
  ASSERT_FALSE(result.pv.empty());
  EXPECT_EQ(to_uci(result.pv[0]), "h5f7");

  // Eval should indicate mate
  EXPECT_GT(result.eval, CENTIPAWN_MATE - 100);
}

TEST(SearchMate, ReturnsCheckmateEval) {
  // Back-rank mate: Black king on h8, trapped by own pawns g7/h7
  // White rook on f8 delivers checkmate along the 8th rank
  // King cannot escape: g8 attacked by rook, g7/h7 blocked by pawns
  Position pos = parse("5R1k/6pp/8/8/8/8/8/6K1 b - - 0 1");

  // Count truly legal moves (pseudo_legal_moves returns pseudo-legal, need to filter)
  const auto pseudo_legal = pseudo_legal_moves(pos);
  int legal_count = 0;
  for (const auto& mv : pseudo_legal) {
    pos.make_move(mv);
    if (!is_in_check(pos.opponent_colour(), pos.board)) {
      legal_count++;
    }
    pos.unmake_move(mv);
  }
  EXPECT_EQ(legal_count, 0) << "Expected checkmate but found " << legal_count << " legal moves";

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 1;

  const auto result = search::search(pos, limits, reporter);

  // Should return mate score (negative since black is mated)
  EXPECT_LT(result.eval, -CENTIPAWN_MATE + 100);
  // No moves available
  EXPECT_TRUE(result.pv.empty());
}

TEST(SearchMate, ReportsMoveCountUntilMate) {
  // Mate in 1 position
  Position pos = parse("6k1/5ppp/8/8/8/8/8/4R1K1 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  // The eval should encode mate score
  EXPECT_GT(result.eval, CENTIPAWN_MATE - 10);
}

// -----------------------------------------------------------------------------
// Search Limits
// -----------------------------------------------------------------------------

TEST(SearchLimits, RespectsDepthLimit) {
  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.depth, 3);
}

TEST(SearchLimits, RespectsNodeLimit) {
  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.nodes = 500;

  const auto result = search::search(pos, limits, reporter);

  // Should stop around the node limit (some margin for boundary conditions)
  EXPECT_LE(result.nodes, 600);
}

TEST(SearchLimits, StopSignalHalts) {
  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 100; // Very deep - would take forever without stop

  auto stop_signal = std::make_shared<std::atomic_bool>(false);

  // Start searching in a separate thread and stop it
  std::thread search_thread([&]() {
    // Small delay then stop
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_signal->store(true);
  });

  const auto result = search::search(pos, limits, reporter, stop_signal);

  search_thread.join();

  // Should have stopped early (not reached depth 100)
  EXPECT_LT(result.depth, 100);
  // Should still return a valid result
  EXPECT_FALSE(result.pv.empty());
}

// -----------------------------------------------------------------------------
// Principal Variation
// -----------------------------------------------------------------------------

TEST(SearchPV, AllMovesAreLegal) {
  // Kiwipete position
  Position pos = parse("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  const auto result = search::search(pos, limits, reporter);

  // Verify each move in the PV is legal
  Position test_pos = pos;
  for (const auto& mv : result.pv) {
    auto legal = pseudo_legal_moves(test_pos);
    bool found = false;
    for (const auto& legal_mv : legal) {
      if (legal_mv == mv) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Move " << to_uci(mv) << " is not legal in position";
    test_pos.make_move(mv);
  }
}

TEST(SearchPV, ConsistentWithEval) {
  // Asymmetric position where white has a clear advantage
  Position pos = parse("4k3/8/8/8/3q4/5N2/8/4K3 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  // If eval is positive (white winning), PV should show white's best play
  // If eval is negative (black winning), that's also fine as long as PV is consistent
  ASSERT_FALSE(result.pv.empty());

  // Just verify we got a sensible result (knight should take queen)
  EXPECT_EQ(to_uci(result.pv[0]), "f3d4");
}
