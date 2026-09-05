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

TEST(SearchOrdering, QuiescencePrefersQueenPromotions) {
  // A pawn reaching the last rank almost always wants to be a queen. An
  // underpromotion is a curiosity that decides perhaps one game in ten
  // thousand, so searching =N before =Q wastes the first—and often only—move
  // the node gets to look at.
  const auto promote_knight = make_move(Piece::WP, "b7", "b8", std::nullopt, Piece::WN);
  const auto promote_rook = make_move(Piece::WP, "b7", "b8", std::nullopt, Piece::WR);
  const auto promote_queen = make_move(Piece::WP, "b7", "b8", std::nullopt, Piece::WQ);
  const auto capture_promote_knight = make_move(Piece::WP, "b7", "c8", Piece::BR, Piece::WN);
  const auto capture_promote_queen = make_move(Piece::WP, "b7", "c8", Piece::BR, Piece::WQ);

  MoveList moves = {promote_knight, promote_rook, promote_queen, capture_promote_knight,
                    capture_promote_queen};

  search::detail::order_quiescence_moves(moves);

  const auto position_of = [&moves](const Move& mv) {
    return std::ranges::find(moves, mv) - moves.begin();
  };

  EXPECT_EQ(moves[0], capture_promote_queen);
  EXPECT_EQ(moves[1], promote_queen);
  EXPECT_LT(position_of(capture_promote_queen), position_of(capture_promote_knight));
  EXPECT_LT(position_of(promote_queen), position_of(promote_rook));
  EXPECT_LT(position_of(promote_rook), position_of(promote_knight));
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

TEST(TranspositionTable, DistinguishesPromotionPieces) {
  // e7-e8 is four different moves. Matching only on from/to would hand the
  // search a queen when the table said knight—a real difference, since
  // underpromotion to a knight is sometimes the only move that wins.
  const auto to_queen = make_move(Piece::WP, "e7", "e8", std::nullopt, Piece::WQ);
  const auto to_knight = make_move(Piece::WP, "e7", "e8", std::nullopt, Piece::WN);

  const MoveList only_the_knight = {to_knight};
  EXPECT_FALSE(
      search::decode_tt_move(search::encode_tt_move(to_queen), only_the_knight).has_value());

  const auto decoded = search::decode_tt_move(search::encode_tt_move(to_knight), only_the_knight);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->promotion_piece, Piece::WN);
}

TEST(TranspositionTable, KeepsTheDeeperEntryWithinOneSearch) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const std::uint64_t key = 0x1234'5678'9ABC'DEF0ULL;

  tt.store(key, 8, 100, search::Bound::Exact, search::TT_NO_MOVE);

  // A shallow bound arriving later in the same search must not throw away a
  // deep exact score for the same position—that is the most expensive thing
  // in the table, and the shallow result adds nothing we did not know.
  tt.store(key, 2, 42, search::Bound::Upper, search::TT_NO_MOVE);
  const auto* const kept = tt.probe(key);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->depth, 8);
  EXPECT_EQ(kept->score, 100);

  // Within the slack, though, a shallower result is fresh enough to be worth
  // having: it comes from the current search, the deep one may not.
  tt.store(key, 8 - search::TT_REPLACEMENT_DEPTH_SLACK, 42, search::Bound::Upper,
           search::TT_NO_MOVE);
  const auto* const refreshed = tt.probe(key);
  ASSERT_NE(refreshed, nullptr);
  EXPECT_EQ(refreshed->score, 42);
}

TEST(TranspositionTable, AlwaysKeepsAnExactScore) {
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const std::uint64_t key = 0x0F0F'0F0F'0F0F'0F0FULL;

  tt.store(key, 10, 100, search::Bound::Exact, search::TT_NO_MOVE);

  // An exact score is the most useful thing an entry can hold—it answers any
  // window—so it is always worth writing, however shallow.
  tt.store(key, 1, -25, search::Bound::Exact, search::TT_NO_MOVE);
  const auto* const entry = tt.probe(key);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->depth, 1);
  EXPECT_EQ(entry->score, -25);
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

TEST(TranspositionTable, GenerationWrapsAfterSixtyFourSearches) {
  // Six bits hold the generation, so after TT_GENERATION_COUNT searches the
  // counter comes back round and a very old entry looks current again. That is
  // allowed to cost us a replacement decision; it must never cost us a wrong
  // score, and the depth rule must still be doing its job on the other side.
  search::TranspositionTable tt(search::TT_MIN_SIZE_MB);
  const std::uint64_t key = 0xC0FF'EE00'0000'0011ULL;
  const std::uint64_t colliding = key + (tt.capacity() * 3);

  tt.store(key, 9, 100, search::Bound::Exact, search::TT_NO_MOVE);

  for (int i = 0; i < search::TT_GENERATION_COUNT; ++i) {
    tt.new_search();
  }
  EXPECT_EQ(tt.generation(), 0) << "the counter should have wrapped exactly once";

  // The entry now claims the current generation, so it is no longer "stale"
  // and the depth rule protects it from a shallow bound...
  tt.store(colliding, 1, -50, search::Bound::Upper, search::TT_NO_MOVE);
  const auto* const survivor = tt.probe(key);
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(survivor->score, 100);

  // ...but an exact score still gets in, and reads back exactly as written.
  tt.store(colliding, 1, -50, search::Bound::Exact, search::TT_NO_MOVE);
  const auto* const replacement = tt.probe(colliding);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->score, -50);
  EXPECT_EQ(replacement->bound(), search::Bound::Exact);
  EXPECT_EQ(replacement->generation(), tt.generation());
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

TEST(NullMove, NeverStoresAMateScoreFromPassing) {
  // Black is checkmated, but it is White's move. Passing therefore "finds" a
  // mate—one that only exists because Black was never allowed to reply. The
  // table outlives the search, so a fabricated forced mate stored here would
  // go on lying to every later search that reaches this position.
  Position pos = parse("R6k/8/6K1/8/8/8/8/8 w - - 0 1");

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  MoveList pv;
  const int beta = 50;
  search::detail::alphabeta(pos, 4, -CENTIPAWN_MAX, beta, pv, tt, killers, report, stopper);

  const auto* const entry = tt.probe(pos.key);
  if (entry != nullptr) {
    EXPECT_LT(entry->score, CENTIPAWN_MATE_THRESHOLD)
        << "a null-move cutoff stored a mate that cannot happen on the real board";
    EXPECT_LE(entry->score, beta);
  }
}

// Transposition table cutoffs --------------------------------------------------

TEST(TranspositionTable, NonPvNodesTakeCutoffsFromEveryBoundType) {
  // Non-PV nodes are asked one question—"better or worse than alpha?"—and a
  // stored bound can answer it outright. That is where the table earns its
  // keep, so all three bound types must still cut off without searching.
  Position pos = Position::startpos();

  const struct {
    const char* name;
    int score;
    search::Bound bound;
    int expected;
  } cases[] = {
      {"exact", 123, search::Bound::Exact, 123},
      {"lower", 500, search::Bound::Lower, 1},  // >= beta: return beta
      {"upper", -500, search::Bound::Upper, 0}, // <= alpha: return alpha
  };

  for (const auto& scenario : cases) {
    search::TranspositionTable tt;
    search::KillerMoves killers;
    search::Report report;
    search::Stopper stopper;

    tt.store(pos.key, 5, scenario.score, scenario.bound, search::TT_NO_MOVE);

    MoveList pv;
    const int eval = search::detail::alphabeta(pos, 3, 0, 1, pv, tt, killers, report, stopper);

    EXPECT_EQ(eval, scenario.expected) << scenario.name;
    // One node: the cutoff node itself, which we did enter and did decide.
    // Nothing BELOW it was searched, which is the saving the table exists for.
    EXPECT_EQ(report.nodes, 1U) << scenario.name << ": cutoff should search no children";
  }
}

TEST(TranspositionTable, PvNodesRefuseTheCutoffAndSearchAnyway) {
  Position pos = Position::startpos();

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  tt.store(pos.key, 5, 123, search::Bound::Exact, search::TT_NO_MOVE);

  MoveList pv;
  search::detail::alphabeta(pos, 3, CENTIPAWN_MIN, CENTIPAWN_MAX, pv, tt, killers, report, stopper);

  EXPECT_GT(report.nodes, 0U) << "a PV node must search so that it has a line to report";
  EXPECT_FALSE(pv.empty());
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

  // Nb1c3 and Ng1f3 are mirror images of one another in the starting position and
  // score EXACTLY the same, as do Nb8c6 and Ng8f6 in reply. Which one comes back
  // is a tie broken by move ordering, not a preference the search has, so the
  // test asks for a developing knight move rather than pinning the coin flip.
  // (It used to pin "g1f3": the order an unstable std::sort happened to leave
  // equal-scoring moves in before ordering became a deterministic selection.)
  const auto pv = pv_to_uci(result.pv);
  ASSERT_GE(pv.size(), 1U);
  EXPECT_TRUE(pv[0] == "b1c3" || pv[0] == "g1f3") << "unexpected first move " << pv[0];
  if (pv.size() > 1) {
    EXPECT_TRUE(pv[1] == "b8c6" || pv[1] == "g8f6") << "unexpected reply " << pv[1];
  }
}

TEST(SearchCorrectness, MatchesKiwipeteDepth3) {
  Position pos = parse("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.depth, 3);

  // A small edge for White, not a won position: the exact number belongs to the
  // evaluation and is expected to move whenever that is retuned.
  EXPECT_GT(result.eval, 0);
  EXPECT_LT(result.eval, 200);

  const auto pv = pv_to_uci(result.pv);
  ASSERT_GE(pv.size(), 1U);
  EXPECT_EQ(pv[0], "e2a6");
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

TEST(SearchDraw, ReportsADrawnPvAsADrawAndTruncatesIt) {
  // King, bishop and knight against a bare king is a forced win, but not
  // within the fifty-move rule from here: the clock is at 95, so the best
  // line runs into the draw before the mate arrives. The engine must report
  // what the line is actually worth—nothing—and stop the line at the move
  // that draws, rather than advertise a bishop and a knight it never cashes.
  //
  // This is also where the search's own score and the reported score are
  // allowed to differ. Only the reported one is sanitised; the searched one
  // stays as it was so it can centre the next aspiration window.
  Position pos = parse("8/8/8/3k4/8/8/8/3BNK2 w - - 95 60");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 6;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.eval, CENTIPAWN_DRAW);
  ASSERT_FALSE(result.pv.empty());

  // The PV ends exactly at the drawing move, not before and not after.
  Position replayed = pos;
  for (std::size_t i = 0; i < result.pv.size(); ++i) {
    replayed.make_move(result.pv[i]);
    const bool drawn = replayed.is_fifty_move_draw() || replayed.is_repetition_draw(0);
    EXPECT_EQ(drawn, i + 1 == result.pv.size()) << "at PV index " << i;
  }
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

// -----------------------------------------------------------------------------
// Soft and hard time limits
// -----------------------------------------------------------------------------

TEST(SearchTime, PlainTimeStillMeansBothLimits) {
  // Every caller that has one number in mind keeps working: `time` fills in
  // for whichever of the two was not named.
  search::Limits both;
  both.time = std::chrono::milliseconds{500};
  EXPECT_EQ(both.soft_limit(), std::chrono::milliseconds{500});
  EXPECT_EQ(both.hard_limit(), std::chrono::milliseconds{500});

  search::Limits split;
  split.soft_time = std::chrono::milliseconds{100};
  split.hard_time = std::chrono::milliseconds{300};
  EXPECT_EQ(split.soft_limit(), std::chrono::milliseconds{100});
  EXPECT_EQ(split.hard_limit(), std::chrono::milliseconds{300});

  const search::Limits none;
  EXPECT_FALSE(none.soft_limit().has_value());
  EXPECT_FALSE(none.hard_limit().has_value());
}

TEST(SearchTime, AbsurdClocksAreCappedBeforeTheyOverflow) {
  // Budgets are compared against steady_clock::duration, which counts
  // nanoseconds in a signed 64-bit integer. A limit of a few quintillion
  // milliseconds—which a GUI is free to send—cannot be converted to one, and
  // the wrap makes the engine stop instantly instead of thinking for ever.
  search::Limits limits;
  limits.time = std::chrono::milliseconds{9'000'000'000'000'000'000LL};

  ASSERT_TRUE(limits.soft_limit().has_value());
  ASSERT_TRUE(limits.hard_limit().has_value());
  EXPECT_EQ(*limits.soft_limit(), search::MAX_TIME_LIMIT);
  EXPECT_EQ(*limits.hard_limit(), search::MAX_TIME_LIMIT);

  // The capped value survives the conversion the search actually performs.
  const auto as_duration = std::chrono::steady_clock::duration{*limits.hard_limit()};
  EXPECT_GT(as_duration.count(), 0);

  search::Limits split;
  split.soft_time = std::chrono::milliseconds{9'000'000'000'000'000'000LL};
  split.hard_time = std::chrono::milliseconds{9'000'000'000'000'000'000LL};
  EXPECT_EQ(*split.soft_limit(), search::MAX_TIME_LIMIT);
  EXPECT_EQ(*split.hard_limit(), search::MAX_TIME_LIMIT);
}

TEST(SearchTime, SearchRunsCleanlyUnderAnAbsurdClock) {
  // The arithmetic above is exercised for real here: under the sanitisers this
  // is what catches a limit that wrapped on its way into the search.
  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;
  limits.time = std::chrono::milliseconds{9'000'000'000'000'000'000LL};

  const auto result = search::search(pos, limits, reporter);

  // A clock that large is no limit at all, so the depth bound is what stops it.
  EXPECT_EQ(result.depth, 4);
  EXPECT_FALSE(result.pv.empty());
}

TEST(SearchTime, SoftLimitNeverExceedsTheHardOne) {
  // A soft limit past the hard one would promise time the search is not
  // allowed to take, so the ceiling wins.
  search::Limits limits;
  limits.soft_time = std::chrono::milliseconds{900};
  limits.hard_time = std::chrono::milliseconds{200};

  EXPECT_EQ(limits.soft_limit(), std::chrono::milliseconds{200});
  EXPECT_EQ(limits.hard_limit(), std::chrono::milliseconds{200});
}

TEST(SearchTime, RefusesAnIterationThatCannotFinish) {
  using namespace std::chrono_literals;

  // With headroom (soft < hard) the factor is 4. 40ms gone and a 60ms last
  // iteration predicts 240ms more: 280ms in total, measured against the hard
  // limit less the 5ms safety margin. The soft limit is set clear of the
  // elapsed time so that only the affordability rule can answer here.
  const auto soft = std::make_optional(100ms);
  EXPECT_TRUE(search::detail::should_continue_deepening(40ms, 60ms, soft, 400ms));
  EXPECT_FALSE(search::detail::should_continue_deepening(40ms, 60ms, soft, 250ms));

  // Landing exactly on the deadline still counts as affordable:
  // 40 + 4 x 40 = 200, against 205 - 5.
  EXPECT_TRUE(search::detail::should_continue_deepening(40ms, 40ms, soft, 205ms));

  // Without a hard limit there is nothing to be unable to afford.
  EXPECT_TRUE(search::detail::should_continue_deepening(40ms, 10000ms, soft, std::nullopt));

  // The first iteration has no predecessor to measure, so it is never refused.
  EXPECT_TRUE(search::detail::should_continue_deepening(std::chrono::steady_clock::duration::zero(),
                                                        std::chrono::steady_clock::duration::zero(),
                                                        std::nullopt, 6ms));
}

TEST(SearchTime, StopsOnceTheSoftBudgetIsSpent) {
  using namespace std::chrono_literals;
  const auto generous_hard = std::make_optional(10000ms);

  // Under the soft limit: keep going. Past it: stop, however much the hard
  // limit would still allow.
  EXPECT_TRUE(search::detail::should_continue_deepening(50ms, 1ms, std::make_optional(100ms),
                                                        generous_hard));
  EXPECT_FALSE(search::detail::should_continue_deepening(150ms, 1ms, std::make_optional(100ms),
                                                         generous_hard));

  // Neither limit set is an unbounded search: it never stops on time.
  EXPECT_TRUE(search::detail::should_continue_deepening(
      std::chrono::hours{1}, std::chrono::hours{1}, std::nullopt, std::nullopt));
}

TEST(SearchTime, IsMoreCautiousWhenThereIsNoHeadroom) {
  using namespace std::chrono_literals;

  // `go movetime` makes soft and hard the same number. An iteration that
  // overruns is then killed outright instead of being absorbed by the
  // headroom, so the same prediction has to clear a higher bar: 4x with
  // headroom, 6x without.
  //
  // 100ms gone, last iteration 50ms. With headroom the prediction is 200ms
  // (300ms in total); without, 300ms (400ms). A 395ms deadline—400ms less the
  // 5ms safety margin—admits the first and refuses the second.
  EXPECT_TRUE(
      search::detail::should_continue_deepening(100ms, 50ms, std::make_optional(200ms), 400ms));
  EXPECT_FALSE(
      search::detail::should_continue_deepening(100ms, 50ms, std::make_optional(400ms), 400ms));
}

namespace {

// Records the depth of the last iteration the search actually reported. An
// iteration the clock abandons never reaches the reporter, so this is how far
// the search really got.
struct LastReportedDepth : search::Reporter {
  std::uint8_t depth{0};
  std::size_t reports{0};

  void send(const search::Report& report) override {
    depth = report.depth;
    reports += 1;
  }
};

} // namespace

TEST(SearchTime, SoftLimitStopsBetweenIterations) {
  // The soft limit is small and the hard limit is far away, so the only thing
  // that can end this search is the between-iterations check—and that check
  // only ever fires with a completed iteration in hand.
  Position pos = Position::startpos();

  LastReportedDepth reporter;
  search::Limits limits;
  limits.soft_time = std::chrono::milliseconds{100};
  limits.hard_time = std::chrono::milliseconds{30000};

  const auto started = std::chrono::steady_clock::now();
  const auto result = search::search(pos, limits, reporter);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_LT(elapsed, std::chrono::seconds{20}) << "the soft limit did not end the search";
  EXPECT_GE(result.depth, 1);
  EXPECT_FALSE(result.pv.empty()) << "stopping between iterations must leave a complete answer";

  // Nothing was abandoned: what the search returns is exactly the last
  // iteration it finished and reported.
  EXPECT_EQ(result.depth, reporter.depth);
}

TEST(SearchTime, HardLimitCutsAnIterationShort) {
  // soft == hard is what `go movetime` produces: no headroom, so the only
  // thing that can end the search is the limit the stopper polls, and that one
  // fires wherever in the tree the search happens to be.
  //
  // The discriminating fact is the depth. Given the same depth bound and no
  // clock at all the search reports every iteration up to it; with the clock
  // it does not get that far, and the iteration it was in the middle of is
  // never reported and never returned.
  Position pos = Position::startpos();

  constexpr std::uint8_t DEPTH = 6;

  LastReportedDepth without_clock;
  search::Limits unclocked;
  unclocked.depth = DEPTH;
  search::search(pos, unclocked, without_clock);

  LastReportedDepth with_clock;
  search::Limits clocked;
  clocked.depth = DEPTH;
  clocked.soft_time = std::chrono::milliseconds{5};
  clocked.hard_time = std::chrono::milliseconds{5};
  const auto result = search::search(pos, clocked, with_clock);

  ASSERT_EQ(without_clock.depth, DEPTH) << "the unclocked run must reach the depth bound";
  EXPECT_LT(with_clock.depth, without_clock.depth) << "the clock did not cut the search short";
  EXPECT_LT(with_clock.reports, without_clock.reports);

  // Whatever it managed to finish is what it returns: an abandoned iteration
  // never becomes the answer.
  EXPECT_EQ(result.depth, with_clock.depth);
}

TEST(SearchLimits, NodeLimitHoldsInACaptureHeavyPosition) {
  // Quiescence used to run without ever asking the stopper, so once the search
  // dropped into a thicket of captures the node limit stopped applying: the
  // capture tree ran to its natural end however many nodes that took. This
  // position is nothing but captures—two full sets of pieces contesting the
  // same central squares—so a quiescence search that does not poll overshoots
  // by orders of magnitude.
  Position pos = parse("r2q1rk1/pp2ppbp/2np1np1/2pP4/2P1PB2/2N2N2/PP2BPPP/R2Q1RK1 b - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.nodes = 2000;

  const auto result = search::search(pos, limits, reporter);

  // The stopper is only consulted every 256 nodes and the tree still has to
  // unwind, so a small overshoot is expected and a large one is the bug.
  EXPECT_LE(result.nodes, limits.nodes.value() + 256 + 64);
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
  // Every child is a single quiescence stand-pat node, so today the count is
  // 1 (root) + 1 (Kh2) + 2 (a7: zero-window, plus the PVS re-search that
  // proves it better) = 4. A hash move that is ALSO left in the main move list
  // gets searched a second time once a7 has taken over as the best move, and
  // that shows up immediately as a fifth node.
  //
  // The bound is what matters, not the number: later pruning work (reductions,
  // history ordering) may search fewer nodes here, but nothing should ever
  // push it back up to 5.
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

  EXPECT_LT(report.nodes, 5U) << "the hash move was searched twice";
  ASSERT_FALSE(pv.empty());
  EXPECT_EQ(to_uci(pv[0]), "a6a7");
}

TEST(SearchNodes, CountsANodeAnsweredByTheTable) {
  // A node whose score comes straight out of the table is still a node: we
  // reached the position, probed for it, and returned an answer. Counting
  // below the probe instead made every transposition cutoff invisible, so
  // `go nodes N` sailed past N and the reported nps understated the search by
  // exactly the amount the table was saving.
  Position pos = parse("8/8/P7/8/8/8/5k2/7K w - - 0 1");

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  // A deep exact score for this very position, so the probe answers at once.
  // The zero window makes this a non-PV node, the only kind allowed to take
  // a cutoff.
  tt.store(pos.key, 10, 42, search::Bound::Exact, search::TT_NO_MOVE);

  MoveList pv;
  const int eval = search::detail::alphabeta(pos, 4, 0, 1, pv, tt, killers, report, stopper);

  ASSERT_EQ(eval, 42) << "no cutoff happened, so this test measures nothing";
  EXPECT_EQ(report.nodes, 1U) << "a transposition cutoff was counted as zero nodes";
}

TEST(SearchNodes, CountsADrawTerminal) {
  // Same argument for a position the rules have already decided: no moves are
  // searched, but we still visited it and still spent the draw tests on it.
  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 w - - 100 50");

  search::TranspositionTable tt;
  search::KillerMoves killers;
  search::Report report;
  search::Stopper stopper;

  MoveList pv;
  const int eval = search::detail::alphabeta(pos, 4, CENTIPAWN_MIN, CENTIPAWN_MAX, pv, tt, killers,
                                             report, stopper);

  ASSERT_EQ(eval, CENTIPAWN_DRAW);
  EXPECT_EQ(report.nodes, 1U) << "a drawn terminal was counted as zero nodes";
}

TEST(SearchNodes, SkipsAHashMoveThatLeavesTheKingInCheck) {
  // Same position. Kg1 is pseudo-legal (the square is empty) but illegal: it
  // walks into the black king's reach. A stored move must go through exactly
  // the same legality filter as every other move—being in the table is not a
  // licence to play it.
  //
  // Once Kg1 is filtered out, only a7 (searched with the full window) and Kh2
  // (zero-window, and worse, so no re-search) cost anything: 1 + 1 + 1 = 3.
  // Searching the illegal move as well would add at least one node on top of
  // that—and, as it happens, capture a king.
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

  EXPECT_LE(report.nodes, 3U) << "an illegal hash move was searched anyway";
  EXPECT_EQ(pos.to_fen(), fen_before) << "an unvalidated hash move corrupted the board";
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
  ASSERT_GE(cold.pv.size(), 3U);

  Position second = parse(fen);
  const auto warm = search::search(second, tt, limits, reporter);

  // The invariant is that a warm table costs us nothing in reported line
  // length, not that the line is exactly `depth` long—a forced mate or a draw
  // legitimately ends it early.
  EXPECT_EQ(warm.pv.size(), cold.pv.size()) << "warm table truncated the principal variation";
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
