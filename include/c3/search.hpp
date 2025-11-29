#pragma once

// =============================================================================
// CHESS SEARCH: Finding the Best Move
// =============================================================================
//
// Search is the heart of a chess engine. Given a position, we want to find
// the best move—the one that leads to the best outcome assuming optimal play
// by both sides.
//
// THE BASIC ALGORITHM: MINIMAX
// Chess is a two-player zero-sum game. What's good for me is bad for you.
// Minimax recursively evaluates positions: I pick the move that maximizes
// my score, you pick the move that minimizes my score (maximizes yours).
//
// THE OPTIMIZATION: ALPHA-BETA PRUNING
// Naive minimax explores every possible move sequence—exponentially expensive.
// Alpha-beta pruning skips branches that can't possibly affect the result.
// "If I've already found a move that guarantees +5, and you show me a line
// where you can force -3, I don't need to see how much worse it can get."
//
// KEY ENHANCEMENTS (all implemented in this engine):
//
//   1. ITERATIVE DEEPENING
//      Search depth 1, then depth 2, then depth 3... This seems wasteful but
//      enables time management (stop anytime) and improves move ordering
//      (use the best move from depth N-1 to order moves at depth N).
//
//   2. TRANSPOSITION TABLE
//      Cache evaluated positions. Chess has many transpositions (different
//      move orders reaching the same position). Without caching, we'd waste
//      time re-evaluating identical positions.
//
//   3. KILLER MOVES
//      Track quiet moves that caused beta cutoffs at each ply. These moves
//      are likely good in sibling nodes too, so try them early.
//
//   4. QUIESCENCE SEARCH
//      At leaf nodes, don't just evaluate—keep searching captures until the
//      position is "quiet". This avoids the "horizon effect" where we stop
//      searching just before a piece gets captured.
//
//   5. NULL-MOVE PRUNING
//      If doing nothing (passing) still gives a good score, the position is
//      probably winning and we can prune aggressively.
//
//   6. ASPIRATION WINDOWS
//      Use narrow alpha-beta windows based on the previous iteration's score.
//      If the score is stable, this drastically reduces the search tree.
//
//   7. FUTILITY PRUNING
//      At shallow depths, skip quiet moves that can't possibly improve alpha.
//      If static_eval + margin < alpha, the move won't help—prune it.
//
// =============================================================================

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "c3/movegen.hpp"
#include "c3/position.hpp"

namespace c3::search {

inline constexpr std::uint8_t MAX_DEPTH = 255;

// Transposition Table (TT) size configuration.
// Larger tables = fewer collisions = more cache hits = faster search.
// But larger tables use more memory. 64MB is a good default for most systems.
inline constexpr std::size_t TT_MIN_SIZE_MB = 1;
inline constexpr std::size_t TT_MAX_SIZE_MB = 4096;
inline constexpr std::size_t TT_DEFAULT_SIZE_MB = 64;

// ---------------------------------------------------------------------------
// Reporting and limits
// ---------------------------------------------------------------------------

struct Report {
  std::uint8_t depth{0};
  std::uint8_t ply{0};
  std::uint64_t nodes{0};
  std::optional<std::pair<MoveList, int>> pv{};
  std::pair<std::size_t, std::size_t> tt_stats{0, 0};
  std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};

  [[nodiscard]] std::chrono::steady_clock::duration elapsed() const {
    return std::chrono::steady_clock::now() - started_at;
  }

  [[nodiscard]] std::optional<std::uint8_t> moves_until_mate() const;
};

class Reporter {
public:
  virtual ~Reporter() = default;
  virtual void send(const Report& report) = 0;
};

class NullReporter : public Reporter {
public:
  void send(const Report&) override {}
};

struct Limits {
  std::optional<std::uint8_t> depth{};
  std::optional<std::uint64_t> nodes{};
  std::optional<std::chrono::milliseconds> time{};
};

class Stopper {
public:
  explicit Stopper(std::shared_ptr<std::atomic_bool> stop_signal = nullptr)
      : stop_signal_(std::move(stop_signal)) {}

  void at_depth(std::optional<std::uint8_t> depth) { depth_ = depth; }
  void at_elapsed(std::optional<std::chrono::milliseconds> elapsed);
  void at_nodes(std::optional<std::uint64_t> nodes) { nodes_ = nodes; }

  [[nodiscard]] bool should_stop(const Report& report) const;

private:
  std::shared_ptr<std::atomic_bool> stop_signal_{};
  std::optional<std::uint8_t> depth_{};
  std::optional<std::chrono::milliseconds> elapsed_{};
  std::optional<std::uint64_t> nodes_{};
};

// ---------------------------------------------------------------------------
// Transposition Table
// ---------------------------------------------------------------------------
//
// The TT is a hash table keyed by Zobrist position hash. It stores previously
// computed search results so we don't re-search identical positions.
//
// BOUND TYPES:
// During alpha-beta search, not every node determines an exact score:
//
//   - Exact: We searched all moves and found the true minimax value.
//            This happens at "PV nodes" (principal variation).
//
//   - Lower: We got a beta cutoff—this move is "too good" (opponent won't
//            allow it). The real score is AT LEAST this high, maybe higher.
//
//   - Upper: All moves failed low (score ≤ alpha). The position is "too bad".
//            The real score is AT MOST this high, maybe lower.
//
// When we probe the TT, we can use these bounds to prune:
//   - If TT says "at least X" and X ≥ beta, we can cut off
//   - If TT says "at most X" and X ≤ alpha, we can cut off
// ---------------------------------------------------------------------------

enum class Bound { Exact, Lower, Upper };

struct TTEntry {
  std::uint64_t key{0};       // Zobrist key (for collision detection)
  std::uint8_t depth{0};      // Search depth (deeper = more reliable)
  int eval{0};                // Evaluation score (may need ply adjustment)
  Bound bound{Bound::Exact};  // Type of bound (see above)
  std::optional<Move> move{}; // Best move found (for move ordering)
};

class TranspositionTable {
public:
  TranspositionTable();

  [[nodiscard]] const TTEntry* probe(std::uint64_t key) const;
  void store(std::uint64_t key, std::uint8_t depth, int eval, Bound bound,
             std::optional<Move> move);

  [[nodiscard]] std::size_t usage() const { return usage_; }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  void clear();

  static void set_size_mb(std::size_t size_mb);
  static std::size_t size_mb();

private:
  std::size_t capacity_{0};
  std::size_t usage_{0};
  std::vector<TTEntry> entries_;
};

// ---------------------------------------------------------------------------
// Killer Moves
// ---------------------------------------------------------------------------
// "Killer moves" are quiet (non-capture) moves that caused beta cutoffs.
// The insight: if a move refuted one position, it might refute sibling
// positions at the same ply too.
//
// We store 2 killer moves per ply. When ordering moves, killers are tried
// after captures but before other quiet moves. This simple heuristic
// significantly improves pruning efficiency.
// ---------------------------------------------------------------------------

class KillerMoves {
public:
  KillerMoves();

  [[nodiscard]] std::optional<Move> probe(std::uint8_t ply, std::size_t index) const;
  void store(std::uint8_t ply, const Move& mv);

private:
  // 2 killer slots per ply, indexed by search depth
  std::array<std::array<std::optional<Move>, 2>, MAX_DEPTH + 1> moves_{};
};

// ---------------------------------------------------------------------------
// Mate Score Normalization
// ---------------------------------------------------------------------------
// Mate scores include the distance to mate (e.g., "mate in 3" = 9997).
// But when storing in the TT, we need "distance from THIS position", not
// distance from the root. These helpers adjust scores when storing (eval_in)
// and retrieving (eval_out) from the TT.
//
// Example: At ply 5, we find mate in 3 more moves (total 8 from root).
//   Store as: mate - 3 (distance from current node)
//   Retrieve at ply 5: adjust back to mate - 8 (distance from root)
// ---------------------------------------------------------------------------

int eval_in(int eval, std::uint8_t ply);
int eval_out(int eval, std::uint8_t ply);

// ---------------------------------------------------------------------------
// Search API
// ---------------------------------------------------------------------------

struct SearchResult {
  std::uint8_t depth{0};
  int eval{0};
  MoveList pv{};
  std::uint64_t nodes{0};
  std::uint32_t hashfull{0}; // permille of TT usage
};

SearchResult search(Position& pos, const Limits& limits, Reporter& reporter, TranspositionTable& tt,
                    std::shared_ptr<std::atomic_bool> stop_signal = nullptr);
SearchResult search(Position& pos, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal = nullptr);
SearchResult search(Position& pos, std::uint8_t depth);

// Exposed for tests
namespace detail {
void order_moves(MoveList& moves, const KillerMoves& killers, std::uint8_t ply);
void order_quiescence_moves(MoveList& moves);
int alphabeta(Position& pos, std::uint8_t depth, int alpha, int beta, MoveList& pv,
              TranspositionTable& tt, KillerMoves& killers, Report& report, const Stopper& stopper);
} // namespace detail

} // namespace c3::search
