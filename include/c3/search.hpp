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
//   3. MOVE ORDERING
//      Alpha-beta's saving depends almost entirely on searching the best move
//      first. Moves are scored once—hash move, then captures by MVV-LVA, then
//      killers, counter-moves and history—and picked one at a time, because a
//      node that cuts off never looks at the rest of the list.
//
//   4. KILLER MOVES, HISTORY AND COUNTER-MOVES
//      Three ways of remembering which QUIET moves have been causing beta
//      cutoffs: killers per ply, history per from/to square pair, and one
//      counter-move per move the opponent just made. Quiet moves are otherwise
//      indistinguishable from each other, and these are what tells them apart.
//
//   5. QUIESCENCE SEARCH
//      At leaf nodes, don't just evaluate—keep searching captures until the
//      position is "quiet". This avoids the "horizon effect" where we stop
//      searching just before a piece gets captured. A leaf that is IN CHECK is
//      not quiet and does not get to pretend otherwise: it searches every legal
//      reply and reports mate when there is none. The captures it does search
//      are filtered—by delta pruning, by static exchange evaluation, and to
//      queen promotions only—because most of them cannot change the answer.
//
//   6. NULL-MOVE PRUNING
//      If doing nothing (passing) still gives a good score, the position is
//      probably winning and we can prune aggressively.
//
//   7. ASPIRATION WINDOWS
//      Use narrow alpha-beta windows based on the previous iteration's score.
//      If the score is stable, this drastically reduces the search tree.
//
//   8. FUTILITY PRUNING
//      At shallow depths, skip quiet moves that can't possibly improve alpha.
//      If static_eval + margin < alpha, the move won't help—prune it.
//
//   9. LATE MOVE REDUCTIONS
//      Quiet moves that ordering ranked near the back are searched shallower
//      than the rest. If such a move turns out to beat alpha anyway, it is
//      searched again at full depth, so the saving costs nothing when the
//      guess was right and a re-search when it was wrong.
//
//  10. STATIC EXCHANGE EVALUATION
//      Price the whole exchange on a square—I take, you take back, and so on
//      with the cheapest piece each time—without searching a node. Quiescence
//      uses it to refuse captures that lose material outright, which is the
//      one thing MVV-LVA cannot see.
//
//  11. REVERSE FUTILITY PRUNING
//      Futility pruning's mirror image, asked about the node rather than about
//      a move: if the position is so far ahead that a few plies cannot claw
//      the margin back, fail high without searching at all.
//
//  12. RAZORING
//      The same question asked from behind: a node whose static evaluation is
//      far BELOW alpha drops into quiescence, and fails low without a
//      full-width search only if the capture search agrees with the guess.
//
//  13. LATE MOVE PRUNING
//      Reductions with the verification removed: near the horizon, once a
//      non-PV node has searched enough quiet moves, the rest of them are not
//      searched at all. Captures, checks and the moves the search has evidence
//      for are exempt, and the first legal move always is.
//
//  14. INTERNAL ITERATIVE REDUCTION
//      A node the transposition table knows nothing about has no move worth
//      trying first, so it is searched one ply shallower and the move it finds
//      is left in the table for the next iteration to order by.
//
//  15. CHECK EXTENSIONS, WITH A BUDGET
//      A node that runs out of depth while in check is given a ply back, so a
//      forcing line is seen to its end rather than evaluated in the middle. A
//      per-path budget stops a perpetual check from buying plies for ever;
//      past it, quiescence answers the check instead.
//
// WHERE THE SEARCH'S WORKING STORAGE LIVES
// Not on the stack. The recursion is up to 255 frames deep and each frame
// would otherwise hold several two-kilobyte move lists, which overflows the
// 512 KiB stack the search thread gets on macOS. Move lists, ordering scores
// and principal variations live in per-ply rows owned by the SearchContext
// below; see WHY THE SCRATCH SPACE LIVES HERE for the full argument, and THE
// PLY CEILING for what keeps the ply from running past the end of those rows.
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
//
// THE PLY CEILING
// ---------------------------------------------------------------------------
// MAX_DEPTH is not only the deepest iteration the engine will start; it is the
// deepest PLY any node may sit at, and that is the stricter of the two claims.
// Killer slots, and the per-ply scratch rows a SearchContext hands out, are
// arrays with exactly MAX_DEPTH + 1 rows, and Report::ply is a single byte. A
// search that recursed past the ceiling would not run out of room, it would
// WRAP: the child of ply 255 is ply 0, and a line that long would start
// overwriting the root's own tables from underneath it. Each of those frames
// also costs real stack, which is the other reason the ceiling has to hold.
//
// Depth alone does not enforce it. Every recursion normally spends a ply of
// depth, so ply <= root depth <= MAX_DEPTH falls out for free—except at the
// check extension, which resets depth to 1 and can therefore go on for as long
// as the checks do. So the ceiling is checked explicitly, in alphabeta(), and
// a node that has reached it answers with quiescence instead of recursing.
//
// The ceiling is the last line of defence, not the working limit: a separate
// budget caps how many check extensions any one PATH may take (see CAPPING THE
// CHECK EXTENSION in search.cpp), so a perpetual check stops buying plies long
// before it could reach ply 255.
// ---------------------------------------------------------------------------

// How many check extensions ONE PATH may take before a node in check resolves
// with quiescence instead of buying another ply. The extension is the only
// place the recursion does not spend depth, so without a bound a line of checks
// can grow until the ply ceiling stops it. See CAPPING THE CHECK EXTENSION in
// search.cpp for why four, and for what the cap deliberately does not buy.
inline constexpr std::uint8_t MAX_CHECK_EXTENSIONS = 4;

struct Report {
  std::uint8_t depth{0};
  std::uint8_t ply{0};
  // SELDEPTH: the deepest ply ANY line reached, which is what the `seldepth`
  // field of a UCI `info` line reports and what the ply ceiling is asserted
  // against. It only ever grows, so it survives the recursion unwinding back to
  // the root.
  //
  // QUIESCENCE COUNTS TOWARDS IT. `depth` is what the engine committed to
  // searching everywhere; seldepth is how far it actually looked down the one
  // line it found most interesting, and most of that difference is quiescence
  // resolving a long exchange, not the main search. A seldepth that stopped at
  // the alpha-beta horizon would report a number the engine beat on almost
  // every line, which is not what a GUI showing "18/34" is telling its user.
  // A quiescence node's ply is its parent's ply plus how deep into quiescence
  // it is, clamped to MAX_DEPTH: the two are bounded separately (255 plies and
  // 64 quiescence levels) and their sum does not have to fit in a byte.
  std::uint8_t max_ply{0};

  // The longest run of check extensions any single path took. It exists to be
  // asserted against MAX_CHECK_EXTENSIONS—the cap has no other visible effect,
  // since a node that stops extending hands its position to quiescence rather
  // than disappearing—and it says how close a search came to the bound.
  std::uint8_t max_check_extensions{0};

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
// History Heuristic
// ---------------------------------------------------------------------------
// Killers remember the last two quiet moves that refuted something AT ONE PLY.
// The history heuristic is the same idea with the ply forgotten: a "butterfly"
// table scoring, for every from-square/to-square pair, how well that quiet move
// has been doing at causing beta cutoffs ANYWHERE in the tree. A move like Rd1
// or h6 that keeps working all over the search floats to the front of the quiet
// moves, and no chess knowledge goes into it—only the search's own record of
// what has been working.
//
// ONE TABLE PER COLOUR. A from/to pair means something different for each side
// (e2-e4 is a white move), so the table is indexed by the colour of the piece
// that moved.
//
// BONUS AND MALUS. A cutoff is evidence for the move that caused it and, just
// as usefully, evidence against the quiet moves that were searched before it
// and failed. Rewarding only the winner would let a move that happens to be
// tried early everywhere collect credit for being tried; penalising the losers
// is what makes the table discriminate rather than just accumulate.
//
// GRAVITY. Adding raw bonuses for ever would let a few square pairs run away
// with scores nothing could catch, and would make ancient evidence weigh as
// much as fresh evidence. So the update is
//
//     score += bonus - score * |bonus| / HISTORY_MAX
//
// which is an ordinary addition while a score is small and cancels the bonus
// out completely as the score approaches HISTORY_MAX. The table saturates
// instead of overflowing, and every update gently decays what was already
// there, so recent cutoffs outweigh old ones without anyone running an ageing
// pass.
//
// FAILURE MODE. History is a statistic, not a fact about the position in front
// of us: a move that is excellent in one pawn structure and dreadful in another
// gets a single score for both. That is affordable because history only ORDERS
// moves—it never prunes a branch or scores a position—so when it is wrong it
// costs time, never correctness.
// ---------------------------------------------------------------------------

// The magnitude a history score saturates at. Large enough to rank thousands of
// cutoffs apart, small enough that the gravity term below stays exact in ints.
inline constexpr int HISTORY_MAX = 16'384;

class HistoryTable {
public:
  // The colour is read from the moving piece, so a caller cannot pass one that
  // disagrees with the move it is talking about.
  [[nodiscard]] int probe(const Move& mv) const;

  // Positive for the move that caused a cutoff, negative for the quiet moves
  // that were tried before it and failed. See GRAVITY above.
  void update(const Move& mv, int bonus);

  void clear();

private:
  std::array<std::array<std::array<int, 64>, 64>, 2> scores_{};
};

// ---------------------------------------------------------------------------
// Counter Moves
// ---------------------------------------------------------------------------
// Some refutations answer a MOVE rather than a position: ...Nf6 meets e5, ...c5
// meets d4, and they meet it wherever in the tree it is played. This table
// remembers, for each (piece, destination square) the opponent last moved to,
// the quiet move that refuted it, and the ordering tries that move immediately
// after the killers.
//
// It is one guess per (piece, square) pair for the whole search, overwritten
// freely and never validated—like the killers it can only reorder moves, and
// an ordering that names a move this position does not have simply never
// matches anything.
// ---------------------------------------------------------------------------

class CounterMoves {
public:
  [[nodiscard]] std::optional<Move> probe(const Move& previous) const;
  void store(const Move& previous, const Move& refutation);

  // Forget every refutation. A SearchContext is built fresh for each search, so
  // the search itself never needs this; it is here because HistoryTable has it,
  // and a pair of tables that are cleared in different ways is a trap for
  // whoever eventually decides to carry one of them between moves.
  void clear();

private:
  std::array<std::array<std::optional<Move>, 64>, 12> moves_{};
};

// ---------------------------------------------------------------------------
// What one search remembers
// ---------------------------------------------------------------------------
// Killers, history and counter-moves are three versions of the same thing:
// cheap guesses about which quiet move to try first, learned from the search's
// own cutoffs and used for nothing but ordering. They are bundled so that they
// are created, threaded through the recursion and discarded together, instead
// of arriving as three more parameters at every call.
//
// Unlike the transposition table, this context does NOT outlive one search.
// Killer slots are indexed by ply, and once the opponent has replied the
// position at a given ply is a different one, so the guesses would be answers
// to questions nobody asked. Building it fresh is therefore also how it is
// "cleared between searches", and it is what keeps `bench` reproducible.
// (Stronger engines do carry history from move to move, halving it each time;
// that is a refinement, not a correction.)
//
// WHY THE SCRATCH SPACE LIVES HERE TOO, AND NOT ON THE STACK
// The context also owns the working storage each node needs while it decides:
// the move list it generated, the ordering scores for those moves, the line it
// reports to its parent, and the quiet moves it has already tried. Every one of
// those is naturally a local variable, and every one of them is here instead.
// Three reasons, in order of how badly they bite:
//
//   1. THREAD STACKS ARE SMALL, AND THE SEARCH IS 255 FRAMES DEEP. A MoveList
//      is a fixed-capacity array of 256 moves—two kilobytes—so a frame holding
//      three of them costs six. Multiply by the ply ceiling and alpha-beta
//      alone wants a megabyte and a half of stack. The main thread usually has
//      eight megabytes, but the search does not run there: it runs on a
//      std::thread so the UCI loop can keep reading `stop`, and a default
//      std::thread gets 512 KiB on macOS and 1 MiB on Windows. The overflow
//      that follows is not an exception, it is a dead process mid-game—and it
//      only shows up on the deep tactical positions the engine most wants to
//      get right.
//
//      DECLARING THE ROWS HERE IS ONLY HALF OF IT. A generator that RETURNS a
//      MoveList puts those two kilobytes in the caller's frame on the way to
//      the row, which costs the same as never having moved it: measured with
//      -fstack-usage, alphabeta's Release frame was 2528 bytes that way and
//      464 once generation wrote into the row directly. So the search fills
//      its rows through pseudo_legal_moves_into() (movegen.hpp), and its
//      frames hold no move list at all.
//
//   2. LOCALITY. A row is allocated once and reused by every node that visits
//      that ply, so the same cache lines are hit again and again instead of a
//      fresh, cold slice of stack being touched at each node.
//
//   3. REPRODUCIBILITY. Rows are zeroed once, when the context is built. A node
//      that reads a row before writing it therefore sees the same bytes on
//      every run, rather than whatever the previous frame left behind, which is
//      what keeps `bench` reporting the same node count twice in a row.
//
// The rows are heap-allocated (a couple of megabytes) and owned by the context,
// so they are freed when the search that needed them ends.
// ---------------------------------------------------------------------------

// How many quiet moves one node remembers, so that a later cutoff can penalise
// the quiet moves that were tried before it and failed. A node that searched
// more than this before finding a cutoff has bigger problems than the exactness
// of its bookkeeping.
inline constexpr std::size_t MAX_PENALISED_QUIETS = 32;

// One ordering score per move, so a row must be as long as a move list can be.
// MoveList::CAPACITY is 256; the most pseudo-legal moves anyone has found a
// position producing is 248, and the most LEGAL moves is 218. A row this long
// can therefore score anything move generation can hand us, and the
// static_assert in search.cpp keeps the two numbers tied together.
inline constexpr std::size_t MAX_ORDERED_MOVES = 256;

// How deep quiescence may recurse before it stops resolving and simply returns
// the static evaluation. In practice it never comes close: each recursion needs
// another capture or promotion, and a position only has so many pieces to trade
// off. The cap exists for the pathological case—a long forced sequence, or a
// bug in move generation—where "keep going until it is quiet" would otherwise
// be an unbounded promise, and it is what makes the per-depth storage below a
// fixed, affordable size.
inline constexpr std::size_t QUIESCENCE_MAX_DEPTH = 64;

// The scratch one node needs to pick its moves: the list it generated, and the
// score ordering gave each entry of that list.
struct MoveScratch {
  MoveList moves;
  std::array<int, MAX_ORDERED_MOVES> scores{};
};

// Everything one ply of the main search needs, on top of the above: the line it
// reports to its parent, and the quiet moves it has already tried.
//
// searched_quiets is value-initialised, and here that costs nothing worth
// counting: the row is zeroed once when the context is built, not—as it was
// when it lived in alphabeta's frame—a quarter of a kilobyte written at every
// single node for the sake of entries the node was about to overwrite anyway.
struct PlyScratch {
  MoveScratch ordering;
  MoveList pv;
  std::array<Move, MAX_PENALISED_QUIETS> searched_quiets{};

  // How many check extensions the line from the root down to and including a
  // node at this ply has taken. A node writes its own total here before it
  // recurses, and its children read the row one ply up to find out what the
  // path above them has already spent. Being per-ply is what makes it a
  // property of the PATH rather than of the search: two sibling lines each get
  // their own count, and a line that unwinds gives its extensions back.
  std::uint8_t check_extensions{0};
};

class SearchContext {
public:
  SearchContext();

  KillerMoves killers;
  HistoryTable history;
  CounterMoves counters;

  // TEST-ONLY SWITCH. Late move reductions have no output of their own—the only
  // thing they change is how much work a search does—so the only honest way to
  // measure them is to run the same search twice with them on and off. Nothing
  // on the UCI path ever writes this; it is here for tests and for bench
  // experiments, and it is why the reduction block in search.cpp asks a
  // question that always answers "yes" in a real game.
  bool reductions_enabled{true};

  // TEST-ONLY SWITCH, for the same reason. Reverse futility pruning cuts a node
  // off before it searches anything, so what it changes is the size of the tree
  // and nothing else. Nothing on the UCI path writes it.
  bool reverse_futility_enabled{true};

  // TEST-ONLY SWITCH, for the same reason. Razoring replaces a full-width
  // search with a quiescence search, so what it changes is how the tree is
  // spent rather than anything reported. Nothing on the UCI path writes it.
  bool razoring_enabled{true};

  // TEST-ONLY SWITCH, for the same reason. Late move pruning drops quiet moves
  // from the back of a node's list, so the only thing that changes is how many
  // of them are searched. Nothing on the UCI path writes it.
  bool late_move_pruning_enabled{true};

  // TEST-ONLY SWITCH, for the same reason. Internal iterative reduction gives
  // up a ply at a node the transposition table knows nothing about, so what it
  // changes is where the search spends its depth. Nothing on the UCI path
  // writes it.
  bool internal_iterative_reduction_enabled{true};

  // TEST-ONLY SWITCH, for the same reason. The cap on check extensions only
  // decides how long a forcing line may keep buying plies, so what it changes
  // is the size of the tree under a position full of checks. Nothing on the UCI
  // path writes it.
  bool check_extension_cap_enabled{true};

  // TEST-ONLY SWITCH, for the same reason: delta pruning, the SEE filter and
  // the underpromotion filter in quiescence all change how much work is done
  // and (deliberately) a little of what is seen, so the only way to measure
  // them is to run with and without. Nothing on the UCI path writes it.
  bool quiescence_pruning_enabled{true};

  // The scratch row for a node at `ply`. Rows live for the whole search and are
  // reused by every node that visits that ply, so a reference into one stays
  // valid across the recursion below it—which is exactly what alphabeta relies
  // on when it hands its child `at_ply(ply + 1).pv` to fill in.
  [[nodiscard]] PlyScratch& at_ply(std::uint8_t ply) { return (*plies_)[ply]; }

  // The same, for quiescence, indexed by how deep INTO quiescence we are rather
  // than by search ply. Callers must respect QUIESCENCE_MAX_DEPTH.
  [[nodiscard]] MoveScratch& at_quiescence_depth(std::size_t depth) {
    return (*quiescence_)[depth];
  }

private:
  // std::array rather than std::vector so the rows cannot be reallocated out
  // from under a reference the recursion is still holding, and behind a
  // unique_ptr because these are megabytes: putting them in the object itself
  // would just move the stack problem to whoever declares a SearchContext.
  std::unique_ptr<std::array<PlyScratch, MAX_DEPTH + 1>> plies_;
  std::unique_ptr<std::array<MoveScratch, QUIESCENCE_MAX_DEPTH>> quiescence_;
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

// Orders the WHOLE list, which the search itself never needs: it asks for one
// move at a time and usually stops after the first. Kept because "the list, in
// order" is the only form the ordering can be read in from outside.
void order_moves(MoveList& moves, const SearchContext& ctx, std::uint8_t ply,
                 const std::optional<Move>& hash_move = std::nullopt,
                 const std::optional<Move>& previous_move = std::nullopt);
void order_quiescence_moves(MoveList& moves);

// How many plies to shave off the search of the `move_number`-th move at this
// depth (1 for the first move searched). Zero means "search it in full". PV
// nodes get one ply less reduction than the rest; see the LATE MOVE REDUCTIONS
// block in search.cpp for why.
[[nodiscard]] std::uint8_t lmr_reduction(std::uint8_t depth, std::size_t move_number,
                                         bool is_pv_node);

// STATIC EXCHANGE EVALUATION
// "If I capture on this square and both sides then keep capturing there with
// their cheapest piece, what do I end up with?" The answer is a material
// number, in centipawns, computed without searching a single node: build the
// swap list of alternating captures, then walk it backwards asking at each step
// whether the side to move would rather stop than continue. PxQ comes out
// positive, QxP defended by a pawn comes out negative, and a rook that trades
// itself for a rook comes out at zero.
//
// WHAT IT IS FOR. Quiescence generates every capture, and most of them are
// simply bad. MVV-LVA can only guess—it ranks by what is being taken, not by
// what happens next—so it happily puts QxP at the front of a list where the
// pawn is defended. SEE knows, so quiescence can refuse the capture outright
// instead of searching a subtree to rediscover that it loses a queen.
//
// WHAT IT DOES NOT KNOW, AND WHY THAT IS ACCEPTED
//   - PINS AND LEGALITY. A defender that is pinned against its own king cannot
//     actually recapture, and SEE counts it anyway; the mirror case—a defender
//     that would be giving away its king—is only handled for the king itself,
//     which may not capture into a square the other side still defends.
//   - PROMOTIONS DURING THE SWAP. The promotion of the move being scored is
//     counted; a defending pawn that would itself promote while recapturing is
//     scored as a pawn.
//   - EVERYTHING THAT IS NOT MATERIAL. A capture that loses a rook to open a
//     mating net scores badly, and rightly, as far as material goes.
// X-rays ARE handled: pieces are removed from a private copy of the board as
// they capture, so a queen behind a rook on the same file joins the exchange by
// itself, which is the case a naive "count the attackers" version gets wrong.
[[nodiscard]] int see(const Position& pos, const Move& mv);

// The capture-only search alphabeta drops into at its leaves. Exposed so that
// what it does and refuses to do—stand pat, resolve a check, prune a losing
// capture—can be tested for itself, without a whole search wrapped round it.
// `quiescence_depth` counts levels INTO quiescence, not search plies, and is
// what QUIESCENCE_MAX_DEPTH bounds; callers start at 0.
int quiescence(Position& pos, int alpha, int beta, SearchContext& ctx, Report& report,
               const Stopper& stopper, std::size_t quiescence_depth = 0);

// `previous_move` is the move that led to this position; it is what the
// counter-move table is keyed by, and it is empty at the root and immediately
// after a null move, where there is no move to answer.
int alphabeta(Position& pos, std::uint8_t depth, int alpha, int beta, MoveList& pv,
              TranspositionTable& tt, SearchContext& ctx, Report& report, const Stopper& stopper,
              const std::optional<Move>& previous_move = std::nullopt);
} // namespace detail

} // namespace c3::search
