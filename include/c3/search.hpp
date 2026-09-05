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
//      time re-evaluating identical positions. The table lives longer than a
//      single search so that knowledge carries over from move to move.
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

// ---------------------------------------------------------------------------
// TWO TIME LIMITS, NOT ONE
// ---------------------------------------------------------------------------
// A single deadline forces a bad choice. Set it where the engine should
// normally stop thinking and it will be cut off mid-iteration whenever a
// position turns out to be hard, throwing away everything that iteration had
// cost. Set it where the engine must not overrun and the engine will happily
// start a doomed iteration with a fraction of the time it needs.
//
// So there are two:
//
//   soft_time  the budget for this move. It is checked only BETWEEN
//              iterations, where stopping is free: the previous iteration is
//              complete and its move is ready to play. It also decides
//              whether to start the next iteration at all.
//
//   hard_time  the point of no return, and the only one the search polls
//              while it is running. Crossing it abandons the current
//              iteration wherever it happens to be. Reaching it should be
//              rare—it exists for the iteration that surprises us, not for
//              routine stopping.
//
// Because the soft limit is allowed to be generous about the current
// iteration and the hard limit is not, `hard >= soft` always; a caller that
// asks for less has its soft limit pulled down to the hard one.
// ---------------------------------------------------------------------------

// Safety margin subtracted from the hard limit before the stopper polls it.
// The clock is only consulted every few hundred nodes, and a `bestmove` still
// has to be written afterwards, so the search aims to stop this much early.
inline constexpr auto TIME_SAFETY_MARGIN = std::chrono::milliseconds{5};

// Ceiling on any time limit, and the reason there is one: the search compares
// budgets against steady_clock::duration, which counts NANOSECONDS in a signed
// 64-bit integer and therefore overflows somewhere past 106 days. A GUI is
// free to send `go wtime 9000000000000000000`—the UCI spec puts no bound on
// the number, and a buggy or hostile one will—and a limit derived from that
// wraps to a negative duration, which would make the engine stop instantly
// rather than think for ever. A day is longer than any real move deserves and
// leaves four orders of magnitude of headroom before the arithmetic can wrap.
inline constexpr auto MAX_TIME_LIMIT = std::chrono::milliseconds{24 * 60 * 60 * 1000};

struct Limits {
  std::optional<std::uint8_t> depth{};
  std::optional<std::uint64_t> nodes{};

  // "One budget, meaning both." Convenient for tests and for `go movetime`,
  // where the GUI really has named a single number. soft_time / hard_time
  // below override it individually when set.
  std::optional<std::chrono::milliseconds> time{};

  std::optional<std::chrono::milliseconds> soft_time{};
  std::optional<std::chrono::milliseconds> hard_time{};

  [[nodiscard]] std::optional<std::chrono::milliseconds> soft_limit() const;
  [[nodiscard]] std::optional<std::chrono::milliseconds> hard_limit() const;
};

class Stopper {
public:
  explicit Stopper(std::shared_ptr<std::atomic_bool> stop_signal = nullptr)
      : stop_signal_(std::move(stop_signal)) {}

  // No at_depth(): depth is not a stop condition. Iterative deepening is a
  // loop, and its bound is where the loop ends—asking the stopper about depth
  // as well would be a second, silent copy of the same rule.
  void at_elapsed(std::optional<std::chrono::milliseconds> elapsed);
  void at_nodes(std::optional<std::uint64_t> nodes) { nodes_ = nodes; }

  [[nodiscard]] bool should_stop(const Report& report) const;

  // Stopping is a one-way door: once any limit has been hit, every score the
  // search is still computing is the score of a tree we abandoned half-way,
  // and must never be written to the transposition table or reported. Latching
  // also makes the answer cheap and stable—should_stop() only consults the
  // clock every 256 nodes, so without a latch it would flip back to "keep
  // going" on the very next node.
  [[nodiscard]] bool has_stopped() const { return stopped_; }

private:
  std::shared_ptr<std::atomic_bool> stop_signal_{};
  std::optional<std::chrono::milliseconds> elapsed_{};
  std::optional<std::uint64_t> nodes_{};
  // Written from the single searching thread; `mutable` so the latch can be
  // set from the logically-const should_stop().
  mutable bool stopped_{false};
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
//
// WHY THE TABLE OUTLIVES A SINGLE `go`
// Allocating and zeroing a 64 MB table costs tens of milliseconds, and a
// 256 MB one costs hundreds. Doing that at the start of every move burns
// thinking time before the clock even starts. Worse, it throws away work:
// after the opponent replies we are usually two plies deeper into the SAME
// tree we just searched, so most of what we learned is still true. The table
// therefore belongs to the Engine, not to one call of search(). It is only
// wiped when a genuinely unrelated game starts (`ucinewgame`) or when the
// user changes the Hash size.
//
// GENERATIONS ("AGE")
// Because the table survives across moves, it fills up with entries from
// searches that are now several plies in the past. Those entries are still
// usable (the score for a position does not go stale), but they are much less
// likely to be visited again, so they are the first thing we are willing to
// overwrite. Each entry records the generation — a small counter bumped once
// per search by new_search() — in which it was written. "Older generation"
// then becomes a cheap synonym for "probably dead weight".
//
// PACKING
// A table's value comes from how many positions it holds, so bytes per entry
// translate directly into playing strength. This entry is squeezed into 16
// bytes: a 64 MB table now holds 4,194,304 positions where the old 56-byte
// entry left room for only 1,048,576 once the capacity was rounded down to a
// power of two—four times the memory, four times the knowledge. Sixteen bytes
// also means four entries per 64-byte cache line, so a probe usually pulls in
// its neighbours for free. The squeeze costs nothing real: scores fit in an
// int16 (mate is ±10000), depth fits in a byte (MAX_DEPTH is 255), and a move
// fits in 16 bits because a chess move is just "from square, to square, maybe
// a promotion piece".
//
// VALIDATING THE TT MOVE
// The 16 bits we read back may not describe a move of THIS position at all.
// Two positions share a slot whenever their keys collide, and although 64-bit
// keys make that rare it is not impossible. Playing such a move—moving a piece
// that is not there, capturing on a square that is empty—would corrupt the
// board beyond repair, and the search would carry on happily on a nonsense
// position. So decoding never trusts the bits: decode_tt_move() looks for a
// move with that from/to/promotion in the position's OWN generated move list
// and returns nothing if there is none. Reconstruction and validation are the
// same step, and as a bonus the reconstructed Move carries the captured piece
// and en-passant flag that the 16 bits did not store.
// ---------------------------------------------------------------------------

enum class Bound : std::uint8_t { Exact, Lower, Upper };

// Every real packed move sets this flag, so a freshly zeroed slot can never be
// mistaken for the move a1-a1.
inline constexpr std::uint16_t TT_MOVE_PRESENT_BIT = 0x8000;

// A packed move of all-zero bits means "this entry has no move".
inline constexpr std::uint16_t TT_NO_MOVE = 0;

// Pack a move into 16 bits: from square (6 bits), to square (6 bits),
// promotion piece (3 bits, 0 = none), plus a "present" flag in the top bit.
[[nodiscard]] std::uint16_t encode_tt_move(const Move& mv);

// Rebuild a full Move from 16 packed bits by finding it in `moves`, the
// position's own pseudo-legal move list. Returns nullopt when no move matches,
// which is exactly the validation we need before touching the board.
[[nodiscard]] std::optional<Move> decode_tt_move(std::uint16_t packed, const MoveList& moves);

struct TTEntry {
  std::uint64_t key{0};                  // Zobrist key (for collision detection)
  std::int16_t score{0};                 // Evaluation score (mate scores are ply-adjusted)
  std::uint16_t packed_move{TT_NO_MOVE}; // Best move found, packed (for move ordering)
  std::uint8_t depth{0};                 // Search depth (deeper = more reliable)
  std::uint8_t bound_and_generation{0};  // Bound in bits 0-1, generation in bits 2-7
  // Named rather than left implicit: the compiler would insert these two bytes
  // of padding anyway to align the next entry's key, so we may as well say so
  // and have somewhere to put the next field that needs a home.
  std::uint16_t reserved{0};

  [[nodiscard]] Bound bound() const { return static_cast<Bound>(bound_and_generation & 0b11U); }
  [[nodiscard]] std::uint8_t generation() const {
    return static_cast<std::uint8_t>(bound_and_generation >> 2U);
  }
  // Asks the same question decode_tt_move() asks, so the two can never disagree.
  [[nodiscard]] bool has_move() const { return (packed_move & TT_MOVE_PRESENT_BIT) != 0; }
};

static_assert(sizeof(TTEntry) == 16, "TTEntry must stay packed into 16 bytes");

// Generations are stored in 6 bits, so the counter wraps after 64 searches.
// Wrapping is harmless: an entry 64 searches old looks "current" again, which
// costs us one missed replacement, never a wrong score.
inline constexpr std::uint8_t TT_GENERATION_COUNT = 64;

// How much shallower a new result may be and still evict a stored one from the
// same search (see the replacement policy in search.cpp). Some slack is wanted:
// a result from the iteration we are running now describes the part of the tree
// we are actually walking, while the deep entry it displaces may be about a
// line we have already refuted. Zero slack ossifies the table; too much throws
// away the deep work the table exists to keep.
inline constexpr std::uint8_t TT_REPLACEMENT_DEPTH_SLACK = 4;

class TranspositionTable {
public:
  // Sized at TT_DEFAULT_SIZE_MB. There is deliberately no process-wide "current
  // size" to inherit instead: the only table whose size a user can change is
  // the one an Engine owns, and that one is resized through resize().
  TranspositionTable();
  explicit TranspositionTable(std::size_t size_mb);

  // Copying would duplicate the whole table—up to 4 GB—somewhere the author
  // almost certainly meant to pass a reference. Deleted so that mistake is a
  // compile error rather than a mysterious pause. Moving is cheap and allowed.
  TranspositionTable(const TranspositionTable&) = delete;
  TranspositionTable& operator=(const TranspositionTable&) = delete;
  TranspositionTable(TranspositionTable&&) noexcept = default;
  TranspositionTable& operator=(TranspositionTable&&) noexcept = default;

  [[nodiscard]] const TTEntry* probe(std::uint64_t key) const;
  void store(std::uint64_t key, std::uint8_t depth, int eval, Bound bound,
             std::uint16_t packed_move);

  // Forget everything. Used when a new, unrelated game starts: entries from
  // the previous game are not wrong, they are simply about positions we will
  // never see again, and keeping them only wastes slots.
  void clear();

  // Change the table's size in megabytes. Reallocates and clears, so this is
  // the expensive operation the persistent table exists to avoid doing often.
  // Must not be called while a search is running—the search holds references
  // into the storage this throws away.
  void resize(std::size_t size_mb);

  // Called once per search: bumps the generation so every entry written from
  // now on is marked "current" and everything already in the table becomes a
  // preferred replacement candidate.
  void new_search();

  [[nodiscard]] std::uint8_t generation() const { return generation_; }
  [[nodiscard]] std::size_t usage() const { return usage_; }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  // Fill level in permille (0-1000), the unit UCI's `hashfull` expects.
  [[nodiscard]] std::uint32_t hashfull() const;

private:
  void allocate(std::size_t size_mb);

  std::size_t capacity_{0};
  std::size_t usage_{0};
  std::uint8_t generation_{0};
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

// Primary entry point: the caller owns the transposition table, so knowledge
// gathered while searching one move is still there for the next one.
SearchResult search(Position& pos, TranspositionTable& tt, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal = nullptr);

// TEST-ONLY convenience entry point: builds a throwaway table for this one
// search and destroys it on return. It exists so a test can say "search this
// position" without owning a table, and it is the wrong thing for anything
// that plays chess—it pays the whole allocation on every call and starts from
// zero knowledge every time. Nothing on the UCI path uses it; the frontend
// searches through the Engine's own table.
SearchResult search(Position& pos, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal = nullptr);
SearchResult search(Position& pos, std::uint8_t depth);

// How much bigger the next iteration is assumed to be than the one just
// finished. Each extra ply multiplies the tree by the effective branching
// factor, so a fixed multiple of the last iteration is the natural estimate.
//
// Four, from measurement rather than theory. Timing consecutive iterations in
// a Release build gives ratios that scatter widely—0.75x to 8.4x measured
// across startpos and Kiwipete—because an aspiration window that fails has to
// re-search the root and can cost several times a clean iteration.
//
// The estimate is wrong in both directions, and erring high is the cheaper
// mistake. An abandoned iteration produces nothing at all—the latching Stopper
// refuses its transposition-table stores precisely because its scores are
// untrue—so it costs a ply AND the time, while refusing one costs only the
// ply, and the time goes back to the clock for the next move.
//
// Together with uci::HARD_TIME_MULTIPLIER (the m in hard = m x soft), this is
// the k in the rule the search actually applies:
//
//     start another iteration only while  elapsed + k x last <= hard
//
// so with m = 3 and k = 4 a search may commit to roughly three times its soft
// budget, and stops looking for more once the next ply is predicted to cost
// more than a quarter of what remains.
inline constexpr int ITERATION_GROWTH_FACTOR = 4;

// The same prediction, half as much again, used when soft and hard are the
// same number—which is what `go movetime` produces.
//
// WHY IT DIFFERS. With headroom, a prediction that comes in low is absorbed:
// the iteration overruns the soft limit and finishes anyway inside the hard
// one. Without headroom there is nothing to absorb it, so the iteration is
// killed and its entire cost is lost. The bar to clear is therefore higher.
//
// WHY SIX. Measured on startpos in a Release build, where the depth 9 -> 10
// step is the anomaly the rule exists to catch (923ms against depth 9's
// 136ms, a ratio of 6.8):
//
//   go movetime 1000   after depth 9 at 232ms, 6 x 138 predicts past 995ms
//                      -> refused, and rightly: depth 10 would not have fitted
//   go movetime 400    after depth 8 at ~90ms, 6 x ~50 predicts ~390ms
//                      -> allowed, and rightly: depth 9 finished at 231ms
//
// Eight refuses that second one too and throws a whole ply away for nothing;
// four admits the first and loses ~760ms to an iteration that is then killed.
//
// BUT THE TWO CASES ARE BARELY SEPARABLE, and it is worth being honest about
// that. At each decision point the search has used almost exactly the same
// FRACTION of its budget—232/995 and ~90/395, both about 0.23—so no rule
// phrased in terms of elapsed-against-budget can cleanly tell them apart. What
// actually differs is the true cost of the next iteration, which is not
// knowable until it has been paid. Six lands on the right side of both, but
// the second one is close enough that ordinary timing noise occasionally
// pushes it over and costs a ply. Tuning nearer than this would be fitting to
// noise; a real fix needs a better predictor than "the last iteration, times a
// constant".
inline constexpr int NO_HEADROOM_GROWTH_FACTOR = 6;

// Exposed for tests
namespace detail {

// Should iterative deepening start another iteration? Both time rules live
// here so they are decided in one place and can be reasoned about—and tested—
// without running a search:
//
//   1. The soft limit is spent. We have a complete answer; stop.
//   2. The next iteration is predicted not to fit inside the hard limit.
//      Its cost is estimated from the last one, which is the only evidence
//      there is: an iteration's cost is not knowable until it has been paid.
//
// The prediction is measured against the hard limit MINUS TIME_SAFETY_MARGIN,
// because that is where the stopper actually fires—comparing against the raw
// limit would predict a finish the search is not allowed to reach.
[[nodiscard]] bool should_continue_deepening(std::chrono::steady_clock::duration elapsed,
                                             std::chrono::steady_clock::duration last_iteration,
                                             std::optional<std::chrono::milliseconds> soft_time,
                                             std::optional<std::chrono::milliseconds> hard_time);

void order_moves(MoveList& moves, const KillerMoves& killers, std::uint8_t ply,
                 const std::optional<Move>& hash_move = std::nullopt);
void order_quiescence_moves(MoveList& moves);
int alphabeta(Position& pos, std::uint8_t depth, int alpha, int beta, MoveList& pv,
              TranspositionTable& tt, KillerMoves& killers, Report& report, const Stopper& stopper);
} // namespace detail

} // namespace c3::search
