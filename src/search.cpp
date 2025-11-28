// =============================================================================
// ALPHA-BETA SEARCH IMPLEMENTATION
// =============================================================================
//
// This file contains the core search algorithm. Here's the high-level flow:
//
// search() → Iterative deepening loop
//   └── alphabeta() → Recursive alpha-beta search with pruning
//         └── quiescence() → Capture-only search at leaf nodes
//
// The search uses many enhancements to reduce the number of nodes examined
// while still finding the best move. Each enhancement is documented where
// it's implemented.
//
// =============================================================================

#include "c3/search.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "c3/eval.hpp"
#include "c3/movegen.hpp"
#include "c3/piece.hpp"

namespace c3::search {
namespace {

// =============================================================================
// ASPIRATION WINDOWS
// =============================================================================
// Instead of searching with alpha=-infinity, beta=+infinity, we use a narrow
// window centered on the previous iteration's score. If the true score is
// within this window, we search far fewer nodes. If it falls outside (a "fail"),
// we re-search with a wider window.
//
// This is a gamble that usually pays off: scores are often stable between
// iterations, so the narrow window succeeds most of the time.
// =============================================================================

constexpr std::uint8_t ASPIRATION_WINDOW_MIN_DEPTH = 4;   // Don't use at low depths
constexpr int ASPIRATION_WINDOW_INITIAL_DELTA = 25;       // ±0.25 pawns
constexpr int ASPIRATION_WINDOW_EXPANSION_FACTOR = 2;     // Double window on fail
constexpr std::uint8_t ASPIRATION_WINDOW_MAX_RETRIES = 3; // Then fall back to full window

// Check stop conditions every 256 nodes instead of every node.
// Checking involves atomic loads and time queries—amortizing the cost matters.
constexpr std::uint64_t STOPPER_NODES_MASK = 0xFF;

// Sanitise the PV by checking if it leads to a draw. If the PV results in a
// fifty-move draw or repetition, truncate it and return CENTIPAWN_DRAW. This
// prevents the engine from reporting a winning eval when the best line
// actually leads to a drawn position.
std::pair<MoveList, int> sanitise_pv(Position pos, const MoveList& moves, int eval) {
  for (std::size_t i = 0; i < moves.size(); ++i) {
    pos.make_move(moves[i]);

    if (pos.is_fifty_move_draw() || pos.is_repetition_draw(0)) {
      MoveList truncated(moves.begin(), moves.begin() + static_cast<std::ptrdiff_t>(i + 1));
      return {truncated, CENTIPAWN_DRAW};
    }
  }

  return {moves, eval};
}

constexpr std::size_t MIN_SIZE_MB = TT_MIN_SIZE_MB;
constexpr std::size_t MAX_SIZE_MB = TT_MAX_SIZE_MB;
constexpr std::size_t DEFAULT_SIZE_MB = TT_DEFAULT_SIZE_MB;

std::atomic<std::size_t> TT_SIZE_MB{DEFAULT_SIZE_MB};

// Check if a side has any pieces besides pawns.
// Used in null-move pruning: don't prune in pawn-only endgames (zugzwang risk).
bool has_non_pawn_material(const Board& board, Colour colour) {
  const auto knights = board.count_pieces(knight(colour));
  const auto bishops = board.count_pieces(bishop(colour));
  const auto rooks = board.count_pieces(rook(colour));
  const auto queens = board.count_pieces(queen(colour));
  return (knights + bishops + rooks + queens) > 0;
}

// =============================================================================
// FUTILITY PRUNING
// =============================================================================
// At shallow depths, skip quiet moves that can't possibly improve alpha.
// If static_eval + margin < alpha, a quiet move has no chance of raising alpha.
constexpr int FUTILITY_MARGIN[] = {0, 100, 300}; // margins for depth 0, 1, 2
constexpr int FUTILITY_DEPTH = 2;

// =============================================================================
// MVV-LVA: Most Valuable Victim - Least Valuable Attacker
// =============================================================================
// The best captures tend to be: high-value pieces captured by low-value pieces.
// PxQ (pawn takes queen) is almost always good; QxP might lose the queen.
//
// Score = (victim_value * 100) - attacker_value
// Higher scores = better captures = searched first
//
// Example scores:
//   PxQ = 900*100 - 100 = 89,900 (excellent)
//   NxQ = 900*100 - 300 = 89,700 (great)
//   QxP = 100*100 - 900 =  9,100 (questionable)
//
// Returns negative so that std::sort orders best captures first (ascending).
// =============================================================================

int capture_priority_score(const Move& mv) {
  if (mv.captured_piece.has_value()) {
    const auto victim_value = PIECE_VALUES[static_cast<std::size_t>(*mv.captured_piece)];
    const auto attacker_value = PIECE_VALUES[static_cast<std::size_t>(mv.piece)];
    return -((victim_value * 100) - attacker_value);
  }

  if (mv.promotion_piece.has_value()) {
    return 1;
  }

  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Report helpers
// ---------------------------------------------------------------------------

std::optional<std::uint8_t> Report::moves_until_mate() const {
  if (!pv.has_value()) {
    return std::nullopt;
  }

  const auto eval = pv->second;
  const auto abs_eval = std::abs(eval);

  if (abs_eval < CENTIPAWN_MATE_THRESHOLD || abs_eval > CENTIPAWN_MATE) {
    return std::nullopt;
  }

  return static_cast<std::uint8_t>(CENTIPAWN_MATE - abs_eval);
}

// ---------------------------------------------------------------------------
// Stopper
// ---------------------------------------------------------------------------

bool Stopper::should_stop(const Report& report) const {
  if (stop_signal_ && stop_signal_->load(std::memory_order_relaxed)) {
    return true;
  }

  if ((report.nodes & STOPPER_NODES_MASK) != 0) {
    return false;
  }

  if (stop_signal_ && stop_signal_->load(std::memory_order_relaxed)) {
    return true;
  }

  if (elapsed_.has_value() && report.elapsed() > *elapsed_) {
    return true;
  }

  if (nodes_.has_value() && report.nodes > *nodes_) {
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// Transposition table
// ---------------------------------------------------------------------------

TranspositionTable::TranspositionTable() {
  const std::size_t size_bytes = size_mb() * 1024 * 1024;
  std::size_t capacity = size_bytes / sizeof(TTEntry);

  // nearest lower power of two
  std::size_t pow2 = 1;
  while ((pow2 << 1) <= capacity) {
    pow2 <<= 1;
  }
  if (pow2 == 0) {
    pow2 = 1;
  }

  capacity_ = pow2;
  entries_.assign(capacity_, TTEntry{});
}

const TTEntry* TranspositionTable::probe(std::uint64_t key) const {
  const auto& entry = entries_[key & (capacity_ - 1)];
  if (entry.key == key) {
    return &entry;
  }
  return nullptr;
}

void TranspositionTable::store(std::uint64_t key, std::uint8_t depth, int eval, Bound bound,
                               std::optional<Move> move) {
  const auto index = key & (capacity_ - 1);
  auto& entry = entries_[index];

  if (depth >= entry.depth) {
    if (entry.key == 0) {
      ++usage_;
    }
    entry.key = key;
    entry.depth = depth;
    entry.eval = eval;
    entry.bound = bound;
    entry.move = move;
  }
}

void TranspositionTable::set_size_mb(std::size_t size_mb) {
  if (size_mb < MIN_SIZE_MB || size_mb > MAX_SIZE_MB) {
    throw std::invalid_argument("invalid transposition table size");
  }
  TT_SIZE_MB.store(size_mb, std::memory_order_release);
}

std::size_t TranspositionTable::size_mb() {
  return TT_SIZE_MB.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Killer moves
// ---------------------------------------------------------------------------

KillerMoves::KillerMoves() = default;

std::optional<Move> KillerMoves::probe(std::uint8_t ply, std::size_t index) const {
  if (index >= moves_[ply].size()) {
    return std::nullopt;
  }
  return moves_[ply][index];
}

void KillerMoves::store(std::uint8_t ply, const Move& mv) {
  auto& slot = moves_[ply];

  if (!slot[0].has_value() || mv != *slot[0]) {
    slot[1] = slot[0];
    slot[0] = mv;
  }
}

// ---------------------------------------------------------------------------
// Mate score normalisation
// ---------------------------------------------------------------------------

int eval_in(int eval, std::uint8_t ply) {
  if (eval >= CENTIPAWN_MATE_THRESHOLD) {
    return eval + static_cast<int>(ply);
  }
  if (eval <= -CENTIPAWN_MATE_THRESHOLD) {
    return eval - static_cast<int>(ply);
  }
  return eval;
}

int eval_out(int eval, std::uint8_t ply) {
  if (eval >= CENTIPAWN_MATE_THRESHOLD) {
    return eval - static_cast<int>(ply);
  }
  if (eval <= -CENTIPAWN_MATE_THRESHOLD) {
    return eval + static_cast<int>(ply);
  }
  return eval;
}

// ---------------------------------------------------------------------------
// MOVE ORDERING
// ---------------------------------------------------------------------------
// Good move ordering is CRITICAL for alpha-beta efficiency. With perfect
// ordering (best move first), alpha-beta examines O(b^(d/2)) nodes instead
// of O(b^d). That's the difference between depth 16 and depth 8!
//
// Our ordering priority:
//   1. TT move (proven best from previous search)
//   2. Captures by MVV-LVA (likely to be good)
//   3. Promotions (creating a queen is usually good)
//   4. Killer moves (caused cutoffs at this ply before)
//   5. Quiet moves (lowest priority)
// ---------------------------------------------------------------------------

void detail::order_moves(MoveList& moves, const KillerMoves& killers, std::uint8_t ply) {
  const auto killer1 = killers.probe(ply, 0);
  const auto killer2 = killers.probe(ply, 1);

  std::ranges::sort(moves, [&](const Move& a, const Move& b) {
    const auto score = [&](const Move& mv) {
      if (mv.captured_piece.has_value()) {
        return capture_priority_score(mv);
      }
      if (mv.promotion_piece.has_value()) {
        return 1;
      }
      if (killer1.has_value() && mv == *killer1) {
        return 2;
      }
      if (killer2.has_value() && mv == *killer2) {
        return 3;
      }
      return 4;
    };

    return score(a) < score(b);
  });
}

void detail::order_quiescence_moves(MoveList& moves) {
  std::ranges::sort(moves, [](const Move& a, const Move& b) {
    const auto score = [](const Move& mv) {
      const auto victim = mv.captured_piece.value_or(pawn(colour(mv.piece)));
      const auto lva = mv.promotion_piece.value_or(mv.piece);
      const auto victim_score = PIECE_VALUES[static_cast<std::size_t>(victim)];
      const auto lva_score = PIECE_VALUES[static_cast<std::size_t>(lva)];
      return -((victim_score * 100) - lva_score);
    };
    return score(a) < score(b);
  });
}

// ---------------------------------------------------------------------------
// QUIESCENCE SEARCH
// ---------------------------------------------------------------------------
// Problem: If we evaluate a position at the search horizon, we might miss
// that a piece is about to be captured. This is the "horizon effect"—the
// engine thinks it's equal, but next move it loses its queen.
//
// Solution: At leaf nodes, don't just evaluate—keep searching captures until
// the position is "quiet" (no immediate captures available).
//
// Stand-pat: The current static evaluation serves as a baseline. The side to
// move can always "stand pat" (decline to capture) if no capture improves the
// position. This prevents quiescence from searching forever.
// ---------------------------------------------------------------------------

int quiescence(Position& pos, int alpha, int beta, Report& report) {
  report.nodes += 1;

  // Stand-pat: static evaluation as the fallback
  const int stand_pat = eval(pos);

  // Beta cutoff: position is already too good, opponent won't allow this
  if (stand_pat >= beta) {
    return beta;
  }

  // Update alpha: we can always achieve at least the stand-pat score
  alpha = std::max(alpha, stand_pat);

  const Colour colour_to_move = pos.colour_to_move;

  // Only search noisy moves (captures and promotions)
  MoveList moves = pseudo_legal_noisy_moves(pos);
  detail::order_quiescence_moves(moves);

  for (const auto& mv : moves) {
    pos.make_move(mv);

    // Skip illegal moves (leave king in check)
    if (is_in_check(colour_to_move, pos.board)) {
      pos.unmake_move(mv);
      continue;
    }

    // Negamax: negate score and swap alpha/beta
    const int score = -quiescence(pos, -beta, -alpha, report);

    pos.unmake_move(mv);

    if (score >= beta) {
      return beta; // Beta cutoff
    }
    alpha = std::max(alpha, score);
  }

  return alpha;
}

// =============================================================================
// ALPHA-BETA SEARCH WITH NEGAMAX
// =============================================================================
// Alpha-beta is the core search algorithm. It explores the game tree while
// pruning branches that can't possibly affect the result.
//
// Key concepts:
//   - alpha: the best score the current player is guaranteed (lower bound)
//   - beta: the best score the opponent is guaranteed (upper bound)
//   - If a move scores >= beta, we can stop (beta cutoff): opponent won't
//     allow this position, so searching further is pointless.
//
// Negamax simplifies the implementation: instead of maximizing for one side
// and minimizing for the other, we always maximize but negate the score and
// swap alpha/beta at each level. max(a,b) = -min(-a,-b).
// =============================================================================

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int detail::alphabeta(Position& pos, std::uint8_t depth, int alpha, int beta, MoveList& pv,
                      TranspositionTable& tt, KillerMoves& killers, Report& report,
                      const Stopper& stopper) {
  // Check if we should stop searching (time limit, node limit, external signal)
  if (stopper.should_stop(report)) {
    return 0;
  }

  // Draw detection: 50-move rule or threefold repetition
  if (pos.is_fifty_move_draw() || pos.is_repetition_draw(report.ply)) {
    return CENTIPAWN_DRAW;
  }

  // Leaf node: drop into quiescence search
  if (depth == 0) {
    if (!is_in_check(pos.colour_to_move, pos.board)) {
      return quiescence(pos, alpha, beta, report);
    }
    // CHECK EXTENSION: Don't stop search while in check (tactical danger)
    depth = 1;
  }

  std::optional<Move> tt_move = std::nullopt;

  // TRANSPOSITION TABLE PROBE
  // Check if we've seen this position before at sufficient depth.
  // If so, we may be able to return immediately or narrow alpha/beta.
  if (const auto* const entry = tt.probe(pos.key)) {
    if (entry->depth >= depth) {
      const int tt_eval = eval_out(entry->eval, report.ply);

      switch (entry->bound) {
      case Bound::Exact:
        return tt_eval; // Exact score: we're done
      case Bound::Lower:
        if (tt_eval >= beta) {
          return beta; // TT says "at least X", and X >= beta: cutoff
        }
        break;
      case Bound::Upper:
        if (tt_eval <= alpha) {
          return alpha; // TT says "at most X", and X <= alpha: cutoff
        }
        break;
      }
    }

    // Even if depth is insufficient, use the TT move for ordering
    tt_move = entry->move;
  }

  report.nodes += 1;

  const Colour colour_to_move = pos.colour_to_move;
  const bool in_check = is_in_check(colour_to_move, pos.board);

  // NULL-MOVE PRUNING
  // Idea: if we can "pass" our turn and STILL beat beta, the position is so
  // good we probably don't need to search it fully. This is a big time saver.
  //
  // Conditions:
  //   - Not in check (can't pass when in check)
  //   - Have non-pawn material (pawn-only endgames have zugzwang risk)
  //   - Sufficient depth (not worth it at shallow depths)
  //
  // Reduction (R): How much shallower we search after the null move.
  // Higher R = more aggressive pruning but higher risk of missing tactics.
  if (depth >= 3 && !in_check && has_non_pawn_material(pos.board, colour_to_move)) {
    pos.make_null_move();
    report.ply += 1;

    const int r = depth > 6 ? 3 : 2; // Deeper positions allow more reduction
    MoveList scratch;
    // Zero-window search: just checking if score >= beta
    const int null_eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - r - 1), -beta,
                                     -beta + 1, scratch, tt, killers, report, stopper);

    report.ply -= 1;
    pos.unmake_null_move();

    if (null_eval >= beta) {
      // Passing was good enough—this position is probably winning
      tt.store(pos.key, depth, eval_in(null_eval, report.ply), Bound::Lower, std::nullopt);
      return beta;
    }
  }

  bool has_searched_one = false;
  Bound tt_bound = Bound::Upper;

  if (tt_move.has_value()) {
    pos.make_move(*tt_move);
    report.ply += 1;

    MoveList child_pv;
    const int eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -beta, -alpha, child_pv,
                                tt, killers, report, stopper);

    report.ply -= 1;
    pos.unmake_move(*tt_move);

    if (eval >= beta) {
      tt.store(pos.key, depth, eval_in(eval, report.ply), Bound::Lower, tt_move);
      return beta;
    }

    if (eval > alpha) {
      alpha = eval;
      tt_bound = Bound::Exact;

      pv.clear();
      pv.push_back(*tt_move);
      pv.insert(pv.end(), child_pv.begin(), child_pv.end());
    }

    has_searched_one = true;
  }

  // Static evaluation for futility pruning (only compute at shallow depths when not in check)
  const int static_eval = (depth <= FUTILITY_DEPTH && !in_check) ? eval(pos) : 0;

  MoveList moves = pseudo_legal_moves(pos);
  detail::order_moves(moves, killers, report.ply);

  for (const auto& mv : moves) {
    if (tt_move.has_value() && mv == *tt_move) {
      continue;
    }

    pos.make_move(mv);

    if (is_in_check(colour_to_move, pos.board)) {
      pos.unmake_move(mv);
      continue;
    }

    // FUTILITY PRUNING
    // At shallow depths, skip quiet moves that can't possibly raise alpha.
    // Don't prune captures, promotions, or when in check.
    // Only prune after first move to avoid falsely returning stalemate.
    if (has_searched_one && depth <= FUTILITY_DEPTH && !in_check &&
        !mv.captured_piece.has_value() && !mv.promotion_piece.has_value() &&
        static_eval + FUTILITY_MARGIN[depth] <= alpha) {
      pos.unmake_move(mv);
      continue;
    }

    report.ply += 1;

    MoveList child_pv;
    int eval;

    // PRINCIPAL VARIATION SEARCH (PVS)
    // After searching the first move (assumed best due to move ordering),
    // search remaining moves with a "zero window" (alpha, alpha+1). This is
    // faster but only proves "this move is worse than alpha" or "better".
    //
    // If a move beats alpha in the zero-window search, it might be a new best
    // move—re-search with the full window to get the true score.
    if (has_searched_one) {
      MoveList zero_window_pv;
      // Zero-window: just checking if move can beat alpha
      eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -alpha - 1, -alpha,
                        zero_window_pv, tt, killers, report, stopper);

      // Re-search with full window if zero-window found a potential improvement
      if (eval > alpha && eval < beta) {
        eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -beta, -alpha, child_pv, tt,
                          killers, report, stopper);
      }
    } else {
      // First move: search with full window
      eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -beta, -alpha, child_pv, tt,
                        killers, report, stopper);
    }

    report.ply -= 1;
    pos.unmake_move(mv);

    // BETA CUTOFF: This move is too good—opponent won't allow this position
    if (eval >= beta) {
      // Store quiet moves that cause cutoffs as "killer moves"
      if (!mv.captured_piece.has_value() && !mv.promotion_piece.has_value()) {
        killers.store(report.ply, mv);
      }

      // Store in TT as a lower bound (actual score might be even higher)
      tt.store(pos.key, depth, eval_in(eval, report.ply), Bound::Lower, mv);
      return beta;
    }

    if (eval > alpha) {
      alpha = eval;
      tt_bound = Bound::Exact;
      tt_move = mv;

      pv.clear();
      pv.push_back(mv);
      pv.insert(pv.end(), child_pv.begin(), child_pv.end());
    }

    has_searched_one = true;
  }

  // No legal moves: either checkmate or stalemate
  if (!has_searched_one) {
    // Checkmate: return negative mate score (we're getting mated)
    // Stalemate: draw
    return in_check ? -CENTIPAWN_MATE + report.ply : CENTIPAWN_DRAW;
  }

  // Store the result in the transposition table for future use
  tt.store(pos.key, depth, eval_in(alpha, report.ply), tt_bound, tt_move);

  return alpha;
}

// ---------------------------------------------------------------------------
// ITERATIVE DEEPENING
// ---------------------------------------------------------------------------
// Instead of searching directly to the target depth, we search to depth 1,
// then depth 2, then depth 3, etc. This seems wasteful, but it's actually
// more efficient because:
//
//   1. TIME MANAGEMENT: We can stop anytime and have a complete result from
//      the previous depth. Essential for time-controlled games.
//
//   2. MOVE ORDERING: The best move at depth N-1 is likely best at depth N.
//      Searching it first improves alpha-beta pruning dramatically.
//
//   3. ASPIRATION WINDOWS: We use the previous depth's score to set a narrow
//      search window, which speeds up the search significantly.
//
// The overhead of re-searching shallower depths is small because most nodes
// are in the final iteration (exponential growth of the tree).
// ---------------------------------------------------------------------------

SearchResult search(Position& pos, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal) {
  Stopper stopper(std::move(stop_signal));
  stopper.at_depth(limits.depth);
  stopper.at_nodes(limits.nodes);
  stopper.at_elapsed(limits.time);

  TranspositionTable tt;
  KillerMoves killers;
  Report report;

  const std::uint8_t max_depth = limits.depth.has_value() ? *limits.depth : MAX_DEPTH;

  int last_eval = 0;
  MoveList best_pv;
  std::uint8_t best_depth = 0;

  for (std::uint8_t depth = 1; depth <= max_depth; ++depth) {
    MoveList pv;

    const bool do_aspiration =
        depth >= ASPIRATION_WINDOW_MIN_DEPTH && std::abs(last_eval) < CENTIPAWN_MATE_THRESHOLD;

    int delta_low = ASPIRATION_WINDOW_INITIAL_DELTA;
    int delta_high = ASPIRATION_WINDOW_INITIAL_DELTA;

    int alpha = do_aspiration ? std::max(CENTIPAWN_MIN, last_eval - delta_low) : CENTIPAWN_MIN;
    int beta = do_aspiration ? std::min(CENTIPAWN_MAX, last_eval + delta_high) : CENTIPAWN_MAX;

    int eval_final = 0;
    std::uint8_t retries = 0;
    bool using_full_window = !do_aspiration;

    while (true) {
      const int eval = detail::alphabeta(pos, depth, alpha, beta, pv, tt, killers, report, stopper);

      // Accept result if: within bounds, stopped, or already using full window
      // (full window means we can't widen further, so accept whatever we get)
      if ((eval > alpha && eval < beta) || stopper.should_stop(report) || using_full_window) {
        eval_final = eval;
        break;
      }

      ++retries;
      if (retries > ASPIRATION_WINDOW_MAX_RETRIES) {
        alpha = CENTIPAWN_MIN;
        beta = CENTIPAWN_MAX;
        using_full_window = true;
        continue;
      }

      if (eval <= alpha) {
        delta_low *= ASPIRATION_WINDOW_EXPANSION_FACTOR;
        alpha = std::max(CENTIPAWN_MIN, last_eval - delta_low);
      } else if (eval >= beta) {
        delta_high *= ASPIRATION_WINDOW_EXPANSION_FACTOR;
        beta = std::min(CENTIPAWN_MAX, last_eval + delta_high);
      }
    }

    if (stopper.should_stop(report)) {
      break;
    }

    // Sanitise the PV to detect draws and adjust eval accordingly
    auto [sanitised_pv, sanitised_eval] = sanitise_pv(pos, pv, eval_final);

    last_eval = sanitised_eval;
    best_pv = sanitised_pv;
    best_depth = depth;

    report.depth = depth;
    report.pv = std::make_pair(sanitised_pv, sanitised_eval);
    report.tt_stats = {tt.usage(), tt.capacity()};
    reporter.send(report);
  }

  SearchResult result;
  result.depth = best_depth;
  result.eval = last_eval;
  result.pv = best_pv;
  result.nodes = report.nodes;
  result.hashfull = static_cast<std::uint32_t>(
      result.nodes == 0 || tt.capacity() == 0 ? 0 : (tt.usage() * 1000) / tt.capacity());

  return result;
}

SearchResult search(Position& pos, std::uint8_t depth) {
  NullReporter reporter;
  Limits limits;
  limits.depth = depth;
  return search(pos, limits, reporter, nullptr);
}

} // namespace c3::search
