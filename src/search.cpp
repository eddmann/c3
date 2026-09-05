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
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

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
// At shallow depths (1-2 ply from horizon), if a position is already losing
// by more than what a quiet move could reasonably gain, skip that move.
//
// The idea: quiet moves (non-captures, non-promotions) typically improve
// a position by at most a few hundred centipawns. If we're already down
// by more than that margin, searching the move is futile.
//
// Conditions:
//   - Shallow depth (depth <= 2)
//   - Not in check (tactical positions need full search)
//   - Not a capture or promotion (these can swing the eval dramatically)
//   - Already searched at least one move (avoid false stalemate detection)
//
// Margins increase with depth: deeper searches need larger margins because
// there's more potential for the position to improve over multiple plies.
// =============================================================================
constexpr int FUTILITY_MARGIN[] = {0, 100, 300}; // margins for depth 0, 1, 2
constexpr int FUTILITY_DEPTH = 2;

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
// Limits
// ---------------------------------------------------------------------------

namespace {

// Every limit passes through here on its way into the search, so a budget too
// large to be a steady_clock::duration cannot reach the arithmetic that would
// wrap it. See MAX_TIME_LIMIT for why the ceiling exists at all.
std::optional<std::chrono::milliseconds> capped(std::optional<std::chrono::milliseconds> limit) {
  if (!limit.has_value()) {
    return std::nullopt;
  }
  return std::min(*limit, MAX_TIME_LIMIT);
}

} // namespace

std::optional<std::chrono::milliseconds> Limits::hard_limit() const {
  return capped(hard_time.has_value() ? hard_time : time);
}

std::optional<std::chrono::milliseconds> Limits::soft_limit() const {
  const auto soft = capped(soft_time.has_value() ? soft_time : time);
  const auto hard = hard_limit();

  if (!soft.has_value()) {
    return hard;
  }

  // A soft limit beyond the hard one would promise time the search is not
  // allowed to take, so the ceiling wins.
  return hard.has_value() ? std::make_optional(std::min(*soft, *hard)) : soft;
}

bool detail::should_continue_deepening(std::chrono::steady_clock::duration elapsed,
                                       std::chrono::steady_clock::duration last_iteration,
                                       std::optional<std::chrono::milliseconds> soft_time,
                                       std::optional<std::chrono::milliseconds> hard_time) {
  // The budget for this move is spent, and an iteration has just finished, so
  // there is a complete answer to hand over. Nothing is lost by stopping.
  if (soft_time.has_value() && elapsed > *soft_time) {
    return false;
  }

  if (!hard_time.has_value()) {
    return true;
  }

  // No headroom (soft == hard, as `go movetime` produces) means an iteration
  // that overruns is killed outright rather than absorbed, so the prediction
  // has to clear a higher bar. See NO_HEADROOM_GROWTH_FACTOR.
  // A soft limit ABOVE the hard one counts as no headroom too—Limits clamps
  // that away before the search sees it, and the cautious reading is right
  // either way, since the hard limit is the real constraint.
  const bool has_headroom = !soft_time.has_value() || *soft_time < *hard_time;
  const int factor = has_headroom ? ITERATION_GROWTH_FACTOR : NO_HEADROOM_GROWTH_FACTOR;

  // Measured against where the stopper really fires, not the raw limit.
  const auto deadline = *hard_time - TIME_SAFETY_MARGIN;

  return elapsed + (last_iteration * factor) <= deadline;
}

// ---------------------------------------------------------------------------
// Stopper
// ---------------------------------------------------------------------------

void Stopper::at_elapsed(std::optional<std::chrono::milliseconds> elapsed) {
  if (elapsed && *elapsed > TIME_SAFETY_MARGIN) {
    elapsed_ = *elapsed - TIME_SAFETY_MARGIN;
  } else {
    elapsed_ = elapsed;
  }
}

bool Stopper::should_stop(const Report& report) const {
  // Once we have decided to stop we never change our mind: the rest of the
  // tree is being abandoned, so nothing it produces may be trusted.
  if (stopped_) {
    return true;
  }

  if (stop_signal_ && stop_signal_->load(std::memory_order_relaxed)) {
    stopped_ = true;
    return true;
  }

  if ((report.nodes & STOPPER_NODES_MASK) != 0) {
    return false;
  }

  if (elapsed_.has_value() && report.elapsed() > *elapsed_) {
    stopped_ = true;
    return true;
  }

  if (nodes_.has_value() && report.nodes > *nodes_) {
    stopped_ = true;
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// PACKING A MOVE INTO 16 BITS
// ---------------------------------------------------------------------------
// A move needs a from square (0-63, 6 bits), a to square (6 bits) and, for a
// pawn reaching the last rank, which piece it becomes (4 choices, 3 bits with
// zero meaning "no promotion"). That is 15 bits; the 16th is
// TT_MOVE_PRESENT_BIT, so that an all-zero entry unambiguously means "no move".
//
// Everything else about a Move—the moving piece, what it captured, whether it
// was en passant—is deliberately dropped. Those fields are all recoverable
// from the position itself, and recovering them is how we validate the move.
// ---------------------------------------------------------------------------

namespace {

constexpr unsigned TT_MOVE_TO_SHIFT = 6;
constexpr unsigned TT_MOVE_PROMOTION_SHIFT = 12;
constexpr std::uint16_t TT_MOVE_SQUARE_MASK = 0b11'1111;
constexpr std::uint16_t TT_MOVE_PROMOTION_MASK = 0b111;

// Promotion pieces are encoded as 1..4 (knight, bishop, rook, queen) so that
// 0 can mean "not a promotion". Colour is not stored—the position knows it.
std::uint16_t encode_promotion(std::optional<Piece> promotion) {
  if (!promotion.has_value()) {
    return 0;
  }

  switch (*promotion) {
  case Piece::WN:
  case Piece::BN:
    return 1;
  case Piece::WB:
  case Piece::BB:
    return 2;
  case Piece::WR:
  case Piece::BR:
    return 3;
  case Piece::WQ:
  case Piece::BQ:
    return 4;
  default:
    // A pawn can only become a knight, bishop, rook or queen. Anything else is
    // a malformed Move, and returning "no promotion" would quietly make it
    // match the wrong move in decode_tt_move()—so trip in Debug builds.
    assert(false && "promotion_piece must be a knight, bishop, rook or queen");
    return 0;
  }
}

} // namespace

std::uint16_t encode_tt_move(const Move& mv) {
  return static_cast<std::uint16_t>(
      TT_MOVE_PRESENT_BIT | mv.from.index() | (mv.to.index() << TT_MOVE_TO_SHIFT) |
      (encode_promotion(mv.promotion_piece) << TT_MOVE_PROMOTION_SHIFT));
}

std::optional<Move> decode_tt_move(std::uint16_t packed, const MoveList& moves) {
  if ((packed & TT_MOVE_PRESENT_BIT) == 0) {
    return std::nullopt;
  }

  const auto from = packed & TT_MOVE_SQUARE_MASK;
  const auto to = (packed >> TT_MOVE_TO_SHIFT) & TT_MOVE_SQUARE_MASK;
  const auto promotion = (packed >> TT_MOVE_PROMOTION_SHIFT) & TT_MOVE_PROMOTION_MASK;

  // Scan the position's own move list. A from/to/promotion triple identifies
  // at most one move in a given position, so the first match is the move—and
  // the fact that it is in this list is proof it is playable here.
  for (const auto& mv : moves) {
    if (mv.from.index() == from && mv.to.index() == to &&
        encode_promotion(mv.promotion_piece) == promotion) {
      return mv;
    }
  }

  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Transposition table
// ---------------------------------------------------------------------------

TranspositionTable::TranspositionTable() {
  allocate(DEFAULT_SIZE_MB);
}

TranspositionTable::TranspositionTable(std::size_t size_mb) {
  allocate(size_mb);
}

void TranspositionTable::allocate(std::size_t size_mb) {
  if (size_mb < MIN_SIZE_MB || size_mb > MAX_SIZE_MB) {
    throw std::invalid_argument("invalid transposition table size");
  }

  const std::size_t size_bytes = size_mb * 1024 * 1024;
  const std::size_t requested = size_bytes / sizeof(TTEntry);

  // Round down to a power of two so that indexing is `key & (capacity - 1)`
  // instead of a modulo—an AND is a single instruction, and we do it on every
  // probe and every store.
  std::size_t pow2 = 1;
  while ((pow2 << 1) <= requested) {
    pow2 <<= 1;
  }

  // Storage first, then the fields that describe it. assign() can throw
  // (bad_alloc on a large table), and publishing capacity_ before the memory
  // exists would leave the table claiming millions of entries it does not
  // have—every probe would then index into whatever the old, smaller vector
  // still held, or past its end.
  entries_.assign(pow2, TTEntry{});
  capacity_ = pow2;
  usage_ = 0;
}

void TranspositionTable::clear() {
  std::ranges::fill(entries_, TTEntry{});
  usage_ = 0;
  // Nothing is left to be older than anything else, so the generation counter
  // may as well start again from zero.
  generation_ = 0;
}

void TranspositionTable::resize(std::size_t size_mb) {
  allocate(size_mb);
  generation_ = 0;
}

void TranspositionTable::new_search() {
  generation_ = static_cast<std::uint8_t>((generation_ + 1) % TT_GENERATION_COUNT);
}

std::uint32_t TranspositionTable::hashfull() const {
  if (capacity_ == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>((usage_ * 1000) / capacity_);
}

const TTEntry* TranspositionTable::probe(std::uint64_t key) const {
  const auto& entry = entries_[key & (capacity_ - 1)];
  if (entry.key == key) {
    return &entry;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// REPLACEMENT POLICY
// ---------------------------------------------------------------------------
// One slot, and every position whose key lands on that index wants it. We keep
// whichever is more likely to earn its place again:
//
//   1. The slot is empty       — nothing to lose, always take it.
//   2. Stored entry is older   — written during an earlier search, so probably
//                                about a branch of the game tree we have
//                                already left behind. Its depth is not worth
//                                defending against a result from right now.
//   3. New bound is Exact      — an exact score answers ANY window, which no
//                                bound can do. It is the most useful thing an
//                                entry can hold, so it always goes in.
//   4. Nearly as deep          — otherwise the new result must satisfy
//                                depth + TT_REPLACEMENT_DEPTH_SLACK >= stored
//                                depth. The slack is deliberate: a result from
//                                the iteration we are running describes the
//                                part of the tree we are actually walking,
//                                while the deep entry may be about a line we
//                                have already refuted. With zero slack the
//                                table ossifies; with unlimited slack every
//                                shallow probe throws away deep work.
//
// Note what rule 4 rules out even for the SAME position: a depth-2 upper bound
// (or the depth-limited lower bound a null-move cutoff writes) must not erase
// a depth-10 exact score we paid dearly for earlier in this very search.
// ---------------------------------------------------------------------------

void TranspositionTable::store(std::uint64_t key, std::uint8_t depth, int eval, Bound bound,
                               std::uint16_t packed_move) {
  auto& entry = entries_[key & (capacity_ - 1)];

  // Zobrist keys are effectively never zero, so a zero key marks a slot that
  // has never been written.
  const bool is_empty = entry.key == 0;
  const bool same_position = entry.key == key;
  const bool is_stale = entry.generation() != generation_;
  const bool is_deep_enough = depth + TT_REPLACEMENT_DEPTH_SLACK >= static_cast<int>(entry.depth);

  if (!is_empty && !is_stale && bound != Bound::Exact && !is_deep_enough) {
    return;
  }

  if (is_empty) {
    ++usage_;
  }

  // A store with no move—the null-move cutoff in alphabeta() is the only case
  // that has none—must not erase a real best move we already know for this
  // position. The bound is new information; "no move" is an absence of it.
  const std::uint16_t move_to_keep =
      (packed_move == TT_NO_MOVE && same_position) ? entry.packed_move : packed_move;

  entry.key = key;
  entry.depth = depth;
  entry.score = static_cast<std::int16_t>(eval);
  entry.packed_move = move_to_keep;
  entry.bound_and_generation =
      static_cast<std::uint8_t>(static_cast<std::uint8_t>(bound) | (generation_ << 2U));
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
//   1. Hash move          the move a previous, usually deeper, search found
//                         best in this very position
//   2. Captures and promotions, ranked by MVV-LVA
//   3. Killer 1           the quiet move that most recently caused a cutoff
//                         at this ply
//   4. Killer 2           the one before it
//   5. Everything else    the quiet moves
//
// MVV-LVA: MOST VALUABLE VICTIM, LEAST VALUABLE ATTACKER
// The best captures tend to be high-value pieces taken by low-value ones. PxQ
// (pawn takes queen) is almost always good; QxP might lose the queen.
// Multiplying the victim by MVV_VICTIM_WEIGHT makes the victim dominate: every
// capture of a queen ranks above every capture of a rook, whatever did the
// capturing. Subtracting the attacker then breaks ties within a victim class,
// cheapest attacker first—if two pieces can take the queen, try the one we
// mind losing least. So PxQ beats NxQ beats QxQ, and all of them beat QxP.
// The values live in PIECE_VALUES (eval.hpp); the ordering depends only on
// their relative sizes, not on the exact numbers.
//
// SCORE ONCE, THEN PICK LAZILY
// The obvious implementation is a full sort with a comparator that scores both
// moves it is handed. That gets two things wrong.
//
// First, such a comparator re-scores the same move on every comparison it takes
// part in: sorting n moves costs O(n log n) comparisons and therefore twice
// that many scorings, where n would do. Scoring is not free—it reads piece
// values, compares against the hash move and both killers, and (once history
// arrives) probes a table—so that repetition is most of the cost of ordering.
//
// Second, and worse, a full sort puts the whole list in order when the search
// will usually look at the first move or two and leave. With decent ordering a
// node that fails high does so on its FIRST move around nine times in ten;
// every comparison spent arranging the rest of the list bought nothing.
//
// So each move is scored exactly once into a parallel array, and the loop then
// asks for one move at a time: scan the moves not yet searched, swap the
// highest-scoring one into place, hand it over. That is a selection sort
// abandoned as soon as the caller stops asking. Picking k moves out of n costs
// O(k*n), which for the k = 1 a cutoff usually needs is a single pass instead
// of a whole sort. In the worst case—every move searched—it is the O(n^2) that
// selection sort always is, but on at most a couple of hundred 8-byte moves
// that stay in cache, and by then the node has paid far more for the searches
// themselves than for any ordering.
// ---------------------------------------------------------------------------

namespace {

constexpr int MVV_VICTIM_WEIGHT = 100;

// PROMOTIONS ARE NOT CAPTURES BY THE PROMOTED PIECE
// Feeding the promoted piece to MVV-LVA as the ATTACKER inverts the ranking a
// promotion deserves: the more valuable the piece the pawn becomes, the worse
// its "least valuable attacker" score, so =N ranked ahead of =Q and every node
// with a promotion spent its first move on the one promotion nobody wants.
//
// What a promotion actually does is trade a pawn for the piece it becomes, so
// it is scored like a capture whose victim is the new piece and whose attacker
// is the pawn. A move that both captures and promotes earns both halves, which
// is right: it is two gains in one move.
int noisy_move_score(const Move& mv) {
  int score = 0;

  if (mv.captured_piece.has_value()) {
    score += (PIECE_VALUES[static_cast<std::size_t>(*mv.captured_piece)] * MVV_VICTIM_WEIGHT) -
             PIECE_VALUES[static_cast<std::size_t>(mv.piece)];
  }

  if (mv.promotion_piece.has_value()) {
    score += (PIECE_VALUES[static_cast<std::size_t>(*mv.promotion_piece)] * MVV_VICTIM_WEIGHT) -
             PIECE_VALUES[static_cast<std::size_t>(mv.piece)];
  }

  return score;
}

// Ordering bands. The gaps are wide enough that nothing can score its way out
// of its band: the richest move imaginable—a promotion to queen that also
// captures a queen—is worth about 180,000 above ORDER_NOISY, which still
// leaves it far below the hash move.
constexpr int ORDER_HASH_MOVE = 100'000'000;
constexpr int ORDER_NOISY = 1'000'000;
constexpr int ORDER_KILLER_1 = 900'000;
constexpr int ORDER_KILLER_2 = 800'000;

// The busiest legal position anyone has constructed offers 218 moves, and 256
// is the round number engines conventionally allow for. Nothing is written past
// it: a position that somehow offered more would simply have its surplus moves
// searched in generation order (see select()), never scored out of bounds.
constexpr std::size_t MAX_SCORED_MOVES = MOVE_LIST_RESERVE;

class OrderedMoves {
public:
  // Main search: the full priority list above.
  OrderedMoves(MoveList& moves, const KillerMoves& killers, std::uint8_t ply,
               const std::optional<Move>& hash_move)
      : moves_(moves), scored_(std::min(moves.size(), MAX_SCORED_MOVES)) {
    const auto killer1 = killers.probe(ply, 0);
    const auto killer2 = killers.probe(ply, 1);

    for (std::size_t i = 0; i < scored_; ++i) {
      scores_[i] = score(moves_[i], hash_move, killer1, killer2);
    }
  }

  // Quiescence: every move there is a capture or a promotion, so MVV-LVA is
  // the whole ordering—there are no killers or quiet moves to rank.
  explicit OrderedMoves(MoveList& moves)
      : moves_(moves), scored_(std::min(moves.size(), MAX_SCORED_MOVES)) {
    for (std::size_t i = 0; i < scored_; ++i) {
      scores_[i] = noisy_move_score(moves_[i]);
    }
  }

  // Hand over the best move not yet searched, swapping it into `index` so that
  // `moves` ends up in searched order and the tail stays untouched. Ties keep
  // generation order, which is what makes a search reproducible.
  const Move& select(std::size_t index) {
    if (index >= scored_) {
      return moves_[index];
    }

    std::size_t best = index;
    for (std::size_t i = index + 1; i < scored_; ++i) {
      if (scores_[i] > scores_[best]) {
        best = i;
      }
    }

    if (best != index) {
      std::swap(moves_[index], moves_[best]);
      std::swap(scores_[index], scores_[best]);
    }

    return moves_[index];
  }

private:
  static int score(const Move& mv, const std::optional<Move>& hash_move,
                   const std::optional<Move>& killer1, const std::optional<Move>& killer2) {
    // The hash move is the best move a previous (usually deeper) search found
    // here, so nothing outranks it—not even a queen capture.
    if (hash_move.has_value() && mv == *hash_move) {
      return ORDER_HASH_MOVE;
    }

    if (mv.captured_piece.has_value() || mv.promotion_piece.has_value()) {
      return ORDER_NOISY + noisy_move_score(mv);
    }

    if (killer1.has_value() && mv == *killer1) {
      return ORDER_KILLER_1;
    }

    if (killer2.has_value() && mv == *killer2) {
      return ORDER_KILLER_2;
    }

    return 0;
  }

  MoveList& moves_;
  std::size_t scored_;
  // Deliberately left uninitialised: entries [0, scored_) are written by the
  // constructor before anything reads them, and zeroing a kilobyte at every
  // node would cost more than the ordering it serves.
  std::array<int, MAX_SCORED_MOVES> scores_; // NOLINT(*-member-init)
};

} // namespace

void detail::order_moves(MoveList& moves, const KillerMoves& killers, std::uint8_t ply,
                         const std::optional<Move>& hash_move) {
  // The eager form: run the lazy selection all the way to the end. The search
  // itself never calls this—it asks for one move at a time and usually stops
  // after the first—but "the whole list, in order" is what a test can read.
  OrderedMoves ordering(moves, killers, ply, hash_move);
  for (std::size_t i = 0; i < moves.size(); ++i) {
    ordering.select(i);
  }
}

void detail::order_quiescence_moves(MoveList& moves) {
  OrderedMoves ordering(moves);
  for (std::size_t i = 0; i < moves.size(); ++i) {
    ordering.select(i);
  }
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
//
// QUIESCENCE MUST ASK THE STOPPER TOO
// "Until the position is quiet" is not a bound anybody set. A middlegame with
// both sides' pieces contesting the same squares can spend thousands of nodes
// resolving one exchange, and while it does so the search is not looking at
// the clock at all. Skipping the check here therefore made every limit
// approximate: `go nodes` overshot, and a move played on the last second of
// the clock could be handed in late. So quiescence polls like alphabeta does,
// and returns the same placeholder score when it is told to stop—the latching
// Stopper makes sure that score is discarded rather than stored.
// ---------------------------------------------------------------------------

int quiescence(Position& pos, int alpha, int beta, Report& report, const Stopper& stopper) {
  if (stopper.should_stop(report)) {
    return 0;
  }

  // THIS position has already been counted—by the alphabeta leaf that handed
  // it over, or by the recursive call below. Quiescence counts each child it
  // decides to visit instead, which keeps every position worth exactly one
  // node whichever of the two searches resolves it.

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
  OrderedMoves ordering(moves);

  for (std::size_t i = 0; i < moves.size(); ++i) {
    const Move mv = ordering.select(i);

    pos.make_move(mv);

    // Skip illegal moves (leave king in check)
    if (is_in_check(colour_to_move, pos.board)) {
      pos.unmake_move(mv);
      continue;
    }

    // The child is a position we are about to enter and decide, so it is
    // counted here—the callee will not count itself.
    report.nodes += 1;

    // Negamax: negate score and swap alpha/beta
    const int score = -quiescence(pos, -beta, -alpha, report, stopper);

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

  // NODE ACCOUNTING
  // A "node" is a position the search entered and decided, and this is where
  // one is counted: at true entry, above the draw tests and the transposition
  // probe and every other way out below. Counting further down made the two
  // cheapest outcomes free—a drawn terminal and a transposition cutoff each
  // returned an answer without reaching the increment—which understated nps by
  // exactly what the table was saving, and let `go nodes N` run well past N,
  // because the nodes the limit is meant to count were the ones it could not
  // see.
  //
  // This one increment covers the quiescence hand-off below too: a leaf is
  // counted here and quiescence does not count itself, so every position costs
  // exactly one node no matter which of the two searches resolves it.
  report.nodes += 1;

  // Draw detection: 50-move rule or threefold repetition
  if (pos.is_fifty_move_draw() || pos.is_repetition_draw(report.ply)) {
    return CENTIPAWN_DRAW;
  }

  // Leaf node: drop into quiescence search
  if (depth == 0) {
    if (!is_in_check(pos.colour_to_move, pos.board)) {
      return quiescence(pos, alpha, beta, report, stopper);
    }
    // CHECK EXTENSION: Don't stop search while in check (tactical danger)
    depth = 1;
  }

  // PV NODES vs NON-PV NODES
  // A node searched with a full window (beta > alpha + 1) is a "PV node": it
  // is on the principal variation, and its job is not just to produce a score
  // but to produce the line of play behind that score. A node searched with a
  // zero window (beta == alpha + 1) is a non-PV node; it only ever answers
  // "better or worse than alpha?", so it has no line to report.
  const bool is_pv_node = beta - alpha > 1;

  std::uint16_t hash_move_packed = TT_NO_MOVE;

  // TRANSPOSITION TABLE PROBE
  // Check if we've seen this position before at sufficient depth.
  // If so, we may be able to return immediately or narrow alpha/beta.
  if (const auto* const entry = tt.probe(pos.key)) {
    // Cutting off here returns a score without filling in `pv`. At a non-PV
    // node nobody wants the line, so that is a pure win. At a PV node it would
    // hand the caller a score with a truncated—often empty—principal
    // variation, which is what the GUI and the next iteration's move ordering
    // rely on. So PV nodes pay for the search and keep their line; they still
    // get the hash move below, which is where most of the speed came from.
    if (!is_pv_node && entry->depth >= depth) {
      const int tt_eval = eval_out(entry->score, report.ply);

      switch (entry->bound()) {
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

    // Even if depth is insufficient (or we refused the cutoff), the stored
    // move is worth having: it is the best move a previous search found here.
    hash_move_packed = entry->packed_move;
  }

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
      // Passing was good enough—this position is probably winning.
      //
      // What we store is clamped to beta, the value this node actually
      // returns. That matters most for mate scores: passing is not a legal
      // chess move, so a "mate" found behind a null move is a mate against an
      // opponent who was never allowed to reply. It does not exist on the real
      // board. Because this table outlives the search, storing it would leave
      // a fabricated forced win behind to mislead every later search that
      // reaches this position, so we refuse to write a mate score from here at
      // all. (Clamping alone is not quite enough: at the root beta can itself
      // be a mate-range bound.)
      //
      // We also have no move to offer—we did not make one—so we store
      // TT_NO_MOVE and rely on the table to keep any move it already holds.
      const int null_bound = std::min(null_eval, beta);
      if (!stopper.has_stopped() && null_bound < CENTIPAWN_MATE_THRESHOLD) {
        tt.store(pos.key, depth, eval_in(null_bound, report.ply), Bound::Lower, TT_NO_MOVE);
      }
      return beta;
    }
  }

  // Static evaluation for futility pruning (only compute at shallow depths when not in check)
  const int static_eval = (depth <= FUTILITY_DEPTH && !in_check) ? eval(pos) : 0;

  MoveList moves = pseudo_legal_moves(pos);

  // Turn the 16 packed bits back into a real Move by finding it among the
  // moves this position actually has. This is the only place a stored move is
  // allowed to become playable, so a corrupted or colliding entry can never
  // reach make_move(). `hash_move` is const on purpose: it is the move a
  // PREVIOUS search recommended, and confusing it with the best move THIS
  // search is finding would make us search the same move twice.
  const std::optional<Move> hash_move = decode_tt_move(hash_move_packed, moves);

  OrderedMoves ordering(moves, killers, report.ply, hash_move);

  bool has_searched_one = false;
  Bound tt_bound = Bound::Upper;

  // The best move THIS search finds, kept apart from `hash_move` above.
  std::optional<Move> best_move = std::nullopt;

  // The hash move is simply the first move the ordering hands over, so it goes
  // through the same loop as everything else: same legality check, same PVS
  // treatment, and—crucially—exactly once.
  for (std::size_t i = 0; i < moves.size(); ++i) {
    const Move mv = ordering.select(i);

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
      if (!stopper.has_stopped()) {
        tt.store(pos.key, depth, eval_in(eval, report.ply), Bound::Lower, encode_tt_move(mv));
      }
      return beta;
    }

    if (eval > alpha) {
      alpha = eval;
      tt_bound = Bound::Exact;
      best_move = mv;

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

  // Store the result in the transposition table for future use.
  // If the stopper fired somewhere below us, `alpha` was assembled from
  // children that returned a placeholder 0 instead of a real score. Writing
  // that into the table would poison the position with a fabricated "equal"
  // evaluation that outlives the aborted search, so we say nothing.
  if (!stopper.has_stopped()) {
    tt.store(pos.key, depth, eval_in(alpha, report.ply), tt_bound,
             best_move.has_value() ? encode_tt_move(*best_move) : TT_NO_MOVE);
  }

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

SearchResult search(Position& pos, TranspositionTable& tt, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal) {
  const auto soft_time = limits.soft_limit();
  const auto hard_time = limits.hard_limit();

  Stopper stopper(std::move(stop_signal));
  stopper.at_nodes(limits.nodes);
  // Only the hard limit is polled inside the tree. The soft limit is a
  // between-iterations decision, made at the bottom of the loop below.
  stopper.at_elapsed(hard_time);

  // Mark everything already in the table as belonging to a previous search.
  // Those entries stay readable—they are still true—but they become the first
  // candidates for replacement as this search fills the table.
  tt.new_search();

  KillerMoves killers;
  Report report;

  const std::uint8_t max_depth = limits.depth.has_value() ? *limits.depth : MAX_DEPTH;

  // TWO SCORES, TWO JOBS. They are equal on almost every iteration, and it is
  // tempting to keep one variable—but they answer different questions, and
  // sanitise_pv() is exactly where they part company.
  //
  //   searched_eval  what alpha-beta actually returned. The next iteration's
  //                  aspiration window is centred on THIS, because it is the
  //                  search's own opinion of the position and therefore the
  //                  best prediction of what the next, deeper search will say.
  //
  //   reported_eval  what we tell the GUI, and what SearchResult carries.
  //                  sanitise_pv() rewrites it to a draw when the principal
  //                  variation walks into a repetition or the fifty-move rule,
  //                  because claiming "+3" for a line that ends in a handshake
  //                  is a lie however good the position looks.
  //
  // Centring the window on the reported score would aim the next iteration at
  // "equal" while the search still believes it is winning: every attempt would
  // fail high, and each one costs a full re-search of the whole tree.
  int searched_eval = 0;
  int reported_eval = 0;
  MoveList best_pv;
  std::uint8_t best_depth = 0;

  // How long the iteration that just finished took. It is the only estimate we
  // have of what the next one will cost.
  auto last_iteration = std::chrono::steady_clock::duration::zero();

  for (std::uint8_t depth = 1; depth <= max_depth; ++depth) {
    const auto iteration_started = std::chrono::steady_clock::now();
    MoveList pv;

    const bool do_aspiration =
        depth >= ASPIRATION_WINDOW_MIN_DEPTH && std::abs(searched_eval) < CENTIPAWN_MATE_THRESHOLD;

    int delta_low = ASPIRATION_WINDOW_INITIAL_DELTA;
    int delta_high = ASPIRATION_WINDOW_INITIAL_DELTA;

    int alpha = do_aspiration ? std::max(CENTIPAWN_MIN, searched_eval - delta_low) : CENTIPAWN_MIN;
    int beta = do_aspiration ? std::min(CENTIPAWN_MAX, searched_eval + delta_high) : CENTIPAWN_MAX;

    int eval_final = 0;
    std::uint8_t retries = 0;
    bool using_full_window = !do_aspiration;

    while (true) {
      // Each attempt rebuilds the line from scratch. Without this, a failed
      // narrow window would leave its half-finished PV behind and the retry
      // could report a mixture of two different searches.
      pv.clear();

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
        alpha = std::max(CENTIPAWN_MIN, searched_eval - delta_low);
      } else if (eval >= beta) {
        delta_high *= ASPIRATION_WINDOW_EXPANSION_FACTOR;
        beta = std::min(CENTIPAWN_MAX, searched_eval + delta_high);
      }
    }

    if (stopper.should_stop(report)) {
      break;
    }

    // Sanitise the PV to detect draws and adjust the REPORTED eval accordingly.
    // The searched eval is kept as it was: see the note where both are declared.
    auto [sanitised_pv, sanitised_eval] = sanitise_pv(pos, pv, eval_final);

    searched_eval = eval_final;
    reported_eval = sanitised_eval;
    best_pv = sanitised_pv;
    best_depth = depth;

    report.depth = depth;
    report.pv = std::make_pair(sanitised_pv, sanitised_eval);
    report.tt_stats = {tt.usage(), tt.capacity()};
    reporter.send(report);

    last_iteration = std::chrono::steady_clock::now() - iteration_started;

    // Both time rules—"the budget is spent" and "the next ply will not fit"—
    // are decided together in should_continue_deepening.
    if (!detail::should_continue_deepening(report.elapsed(), last_iteration, soft_time,
                                           hard_time)) {
      break;
    }
  }

  SearchResult result;
  result.depth = best_depth;
  result.eval = reported_eval;
  result.pv = best_pv;
  result.nodes = report.nodes;
  result.hashfull = tt.hashfull();

  return result;
}

SearchResult search(Position& pos, const Limits& limits, Reporter& reporter,
                    std::shared_ptr<std::atomic_bool> stop_signal) {
  // Test-only: a table built here and thrown away when the function returns.
  // This search starts from zero knowledge and pays the whole allocation cost
  // up front—see the header for why nothing that plays chess may use it.
  TranspositionTable tt;
  return search(pos, tt, limits, reporter, std::move(stop_signal));
}

SearchResult search(Position& pos, std::uint8_t depth) {
  NullReporter reporter;
  Limits limits;
  limits.depth = depth;
  return search(pos, limits, reporter, nullptr);
}

} // namespace c3::search
