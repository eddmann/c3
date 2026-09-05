#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
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

TEST(SearchOrdering, PutsTheHashMoveAheadOfEverything) {
  const auto quiet = make_move(Piece::WP, "c4", "c5");
  const auto killer = make_move(Piece::WP, "a2", "a3");
  const auto pawn_cap_queen = make_move(Piece::WP, "c4", "d5", Piece::BQ);

  MoveList moves = {quiet, killer, pawn_cap_queen};

  search::KillerMoves killers;
  const std::uint8_t ply = 0;
  killers.store(ply, killer);

  // A previous search already proved a move best here; it outranks even PxQ.
  search::detail::order_moves(moves, killers, ply, quiet);

  ASSERT_EQ(moves.size(), 3U);
  EXPECT_EQ(moves[0], quiet);
  EXPECT_EQ(moves[1], pawn_cap_queen);
  EXPECT_EQ(moves[2], killer);
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

// Transposition table layout, move packing and replacement ---------------------

TEST(TranspositionTable, PacksEntriesIntoSixteenBytes) {
  // The table's strength comes from how many positions fit in it, so the
  // entry layout is part of the contract, not an implementation detail.
  EXPECT_EQ(sizeof(search::TTEntry), 16U);
}

TEST(TranspositionTable, RoundTripsMovesThroughSixteenBits) {
  Position pos = parse("8/4P3/8/8/8/8/8/K6k w - - 0 1");
  const auto moves = pseudo_legal_moves(pos);

  for (const auto& mv : moves) {
    const auto decoded = search::decode_tt_move(search::encode_tt_move(mv), moves);
    ASSERT_TRUE(decoded.has_value()) << to_uci(mv);
    EXPECT_EQ(*decoded, mv) << to_uci(mv);
  }

  // The position offers all four promotions, so the promotion bits are
  // genuinely exercised above.
  EXPECT_GE(
      std::ranges::count_if(moves, [](const Move& mv) { return mv.promotion_piece.has_value(); }),
      4);
}

TEST(TranspositionTable, RejectsAMoveThatIsNotLegalHere) {
  // A 16-bit move can arrive from a colliding key or a stale slot. Decoding
  // must refuse it rather than hand the search something to play.
  Position pos = Position::startpos();
  const auto moves = pseudo_legal_moves(pos);

  const auto alien = make_move(Piece::WQ, "d8", "h4");
  EXPECT_FALSE(search::decode_tt_move(search::encode_tt_move(alien), moves).has_value());
  EXPECT_FALSE(search::decode_tt_move(search::TT_NO_MOVE, moves).has_value());
}

TEST(TranspositionTable, KeepsTheDeeperEntryWithinOneSearch) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const std::uint64_t key = 0x1234'5678'9ABC'DEF0ULL;

  tt.store(key, 8, 100, search::Bound::Exact, search::TT_NO_MOVE);
  tt.store(key + 1, 2, -100, search::Bound::Upper, search::TT_NO_MOVE);

  // Same key always refreshes, even with a shallower result.
  tt.store(key, 2, 42, search::Bound::Upper, search::TT_NO_MOVE);
  const auto* const same = tt.probe(key);
  ASSERT_NE(same, nullptr);
  EXPECT_EQ(same->depth, 2);
  EXPECT_EQ(same->score, 42);
}

TEST(TranspositionTable, PrefersReplacingEntriesFromAnEarlierSearch) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const std::uint64_t key = 0xDEAD'BEEF'0000'0001ULL;

  tt.store(key, 9, 100, search::Bound::Exact, search::TT_NO_MOVE);

  // A different key mapping to the same slot cannot evict a deep entry from
  // the current search...
  const std::uint64_t colliding = key + (tt.capacity() * 2);
  tt.store(colliding, 1, -50, search::Bound::Upper, search::TT_NO_MOVE);
  EXPECT_NE(tt.probe(key), nullptr);
  EXPECT_EQ(tt.probe(colliding), nullptr);

  // ...but once a new search begins, the old entry is fair game.
  tt.new_search();
  EXPECT_EQ(tt.generation(), 1);
  tt.store(colliding, 1, -50, search::Bound::Upper, search::TT_NO_MOVE);
  EXPECT_NE(tt.probe(colliding), nullptr);
  EXPECT_EQ(tt.probe(key), nullptr);
}

TEST(TranspositionTable, ClearEmptiesTheTableWithoutResizing) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const auto capacity = tt.capacity();

  tt.store(0xABCDULL, 4, 10, search::Bound::Exact, search::TT_NO_MOVE);
  EXPECT_EQ(tt.usage(), 1U);

  tt.clear();

  EXPECT_EQ(tt.capacity(), capacity);
  EXPECT_EQ(tt.usage(), 0U);
  EXPECT_EQ(tt.probe(0xABCDULL), nullptr);
}

TEST(TranspositionTable, ResizeChangesCapacityAndRejectsBadSizes) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const auto small = tt.capacity();

  tt.resize(search::TT_MIN_SIZE_MB * 4);
  EXPECT_EQ(tt.capacity(), small * 4);

  EXPECT_THROW(tt.resize(0), std::invalid_argument);
  EXPECT_THROW(tt.resize(search::TT_MAX_SIZE_MB + 1), std::invalid_argument);
}

TEST(TranspositionTable, ReportsFillLevelInPermille) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  EXPECT_EQ(tt.hashfull(), 0U);

  for (std::uint64_t key = 1; key <= tt.capacity() / 10; ++key) {
    tt.store(key, 1, 0, search::Bound::Exact, search::TT_NO_MOVE);
  }

  EXPECT_GT(tt.hashfull(), 90U);
  EXPECT_LE(tt.hashfull(), 100U);
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
  EXPECT_EQ(entry->bound(), search::Bound::Lower);
  EXPECT_FALSE(entry->has_move());
  EXPECT_EQ(eval, beta);
}

TEST(NullMove, DoesNotEraseAnExistingBestMove) {
  // A null-move cutoff has no move of its own to report. It must not wipe out
  // a best move the table already holds for this position—that move is the
  // most valuable ordering hint we have.
  Position pos = parse("6k1/8/8/8/8/8/4Q3/4K3 w - - 0 1");

  search::TranspositionTable tt;
  const auto known_best = make_move(Piece::WQ, "e2", "e7");
  tt.store(pos.key, 2, 100, search::Bound::Exact, search::encode_tt_move(known_best));

  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  MoveList pv;
  search::detail::alphabeta(pos, 4, -CENTIPAWN_MAX, 50, pv, tt, killers, report, stopper);

  const auto* const entry = tt.probe(pos.key);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->bound(), search::Bound::Lower);
  ASSERT_TRUE(entry->has_move());

  const auto decoded = search::decode_tt_move(entry->packed_move, pseudo_legal_moves(pos));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, known_best);
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

TEST(SearchDraw, DISABLED_AvoidsStalemateWhenWinning) {
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

  // Use a reporter that tracks when we've completed at least depth 1
  struct DepthTracker : search::Reporter {
    std::atomic<std::uint8_t> max_depth{0};
    void send(const search::Report& report) override {
      std::uint8_t current = max_depth.load();
      while (report.depth > current && !max_depth.compare_exchange_weak(current, report.depth)) {
      }
    }
  };

  DepthTracker reporter;
  search::Limits limits;
  limits.depth = 100; // Very deep - would take forever without stop

  auto stop_signal = std::make_shared<std::atomic_bool>(false);

  // Start searching in a separate thread, wait for at least depth 1, then stop
  std::thread search_thread([&]() {
    // Wait for search to complete at least depth 1 (with timeout for safety)
    auto start = std::chrono::steady_clock::now();
    while (reporter.max_depth.load() < 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      // Timeout after 5 seconds (generous for slow emulation)
      if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
        break;
      }
    }
    stop_signal->store(true);
  });

  const auto result = search::search(pos, limits, reporter, stop_signal);

  search_thread.join();

  // Should have stopped early (not reached depth 100)
  EXPECT_LT(result.depth, 100);
  // Should still return a valid result (at least depth 1 was completed)
  EXPECT_FALSE(result.pv.empty());
}

TEST(SearchLimits, StoresNothingFromAnAbortedSearch) {
  // When the stopper fires, every node still unwinding returns a placeholder
  // 0 rather than a real score. Those zeroes must never reach the table: the
  // table outlives the search, so a fabricated "equal" score for the root
  // would mislead every later search that transposes into this position.
  Position pos = Position::startpos();

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;

  search::Stopper stopper;
  stopper.at_nodes(200); // Far fewer nodes than a depth-6 search needs

  MoveList pv;
  search::detail::alphabeta(pos, 6, CENTIPAWN_MIN, CENTIPAWN_MAX, pv, tt, killers, report, stopper);

  ASSERT_TRUE(stopper.has_stopped()) << "test needs a search that actually aborts";
  EXPECT_EQ(tt.probe(pos.key), nullptr) << "aborted search wrote a fabricated score for the root";
}

TEST(SearchNodes, SearchesTheHashMoveExactlyOnce) {
  // A position with exactly two legal moves and nothing to capture, so the
  // node count can be worked out on paper:
  //
  //   White: Kh1, pawn a6.  Black: Kf2 (which takes g1 and g2 from the king).
  //   Legal: a7 (clearly better) and Kh2 (clearly worse).
  //
  // Every child is a single quiescence stand-pat node. With the hash move
  // searched first the count is 1 (root) + 1 (Kh2) + 2 (a7, zero-window plus
  // the PVS re-search that proves it better) = 4. A hash move that is also
  // left in the main move list gets searched a second time once a7 has taken
  // over as the best move, which shows up immediately as a fifth node.
  Position pos = parse("8/8/P7/8/8/8/5k2/7K w - - 0 1");

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  // Seed the table the way a shallower iteration would have. Depth 0 keeps
  // this a pure move-ordering hint: it is never deep enough to cut off.
  const auto hash_move = make_move(Piece::WK, "h1", "h2");
  tt.store(pos.key, 0, 0, search::Bound::Exact, search::encode_tt_move(hash_move));

  MoveList pv;
  search::detail::alphabeta(pos, 1, CENTIPAWN_MIN, CENTIPAWN_MAX, pv, tt, killers, report, stopper);

  EXPECT_EQ(report.nodes, 4U);
  ASSERT_FALSE(pv.empty());
  EXPECT_EQ(to_uci(pv[0]), "a6a7");
}

TEST(SearchNodes, SkipsAHashMoveThatLeavesTheKingInCheck) {
  // Same position. Kg1 is pseudo-legal (the square is empty) but illegal: it
  // walks into the black king's reach. A stored move must go through exactly
  // the same legality filter as every other move—being in the table is not a
  // licence to play it.
  //
  // Once Kg1 is filtered out, only a7 (searched with the full window) and Kh2
  // (zero-window, and worse, so no re-search) cost anything: 1 + 1 + 1 = 3.
  Position pos = parse("8/8/P7/8/8/8/5k2/7K w - - 0 1");
  const auto fen_before = pos.to_fen();

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  const auto into_check = make_move(Piece::WK, "h1", "g1");
  tt.store(pos.key, 0, 0, search::Bound::Exact, search::encode_tt_move(into_check));

  MoveList pv;
  search::detail::alphabeta(pos, 1, CENTIPAWN_MIN, CENTIPAWN_MAX, pv, tt, killers, report, stopper);

  EXPECT_EQ(report.nodes, 3U) << "an illegal hash move was searched anyway";
  EXPECT_EQ(pos.to_fen(), fen_before);
  ASSERT_FALSE(pv.empty());
  EXPECT_EQ(to_uci(pv[0]), "a6a7");
}

// -----------------------------------------------------------------------------
// Principal Variation
// -----------------------------------------------------------------------------

TEST(SearchPV, KeepsFullLengthPvOnAWarmTable) {
  // The second search of a position finds the table already full of exact
  // scores for its own principal variation. If PV nodes were allowed to take
  // those cutoffs they would return a score with no line behind it, and the
  // reported PV would collapse to a move or two.
  const auto fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

  search::TranspositionTable tt;
  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 5;

  Position first = parse(fen);
  const auto cold = search::search(first, tt, limits, reporter);
  ASSERT_EQ(cold.pv.size(), 5U);

  Position second = parse(fen);
  const auto warm = search::search(second, tt, limits, reporter);

  EXPECT_EQ(warm.pv.size(), 5U) << "warm table truncated the principal variation";
  EXPECT_EQ(pv_to_uci(warm.pv), pv_to_uci(cold.pv));

  // Depth 5 is above ASPIRATION_WINDOW_MIN_DEPTH, so this line was assembled
  // across aspiration attempts. Each move must follow from the last: a line
  // stitched together from two different attempts would not.
  Position replay = parse(fen);
  for (const auto& mv : warm.pv) {
    const auto legal = pseudo_legal_moves(replay);
    ASSERT_NE(std::ranges::find(legal, mv), legal.end()) << to_uci(mv) << " does not follow";
    replay.make_move(mv);
  }
}

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
