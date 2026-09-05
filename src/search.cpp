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
#include <span>
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

// A move is "quiet" when it neither captures nor promotes—nothing on the board
// changes hands. Quiet moves are the ones killers, history, counter-moves,
// futility pruning and late move reductions all talk about, because they are
// the ones that look alike and the ones that are usually pointless.
bool is_quiet(const Move& mv) {
  return !mv.captured_piece.has_value() && !mv.promotion_piece.has_value();
}

// HOW MUCH A CUTOFF IS WORTH
// A cutoff at depth 8 was expensive to find and speaks about a large subtree; a
// cutoff at depth 1 is cheap and local. Scaling with depth squared says so, and
// is the shape every engine settles on. The cap keeps a single very deep cutoff
// from saturating an entry on its own, which would freeze that move at the
// front of the quiet moves no matter what happened afterwards.
constexpr int HISTORY_BONUS_SCALE = 16;
constexpr int HISTORY_BONUS_CAP = 1'200;

int history_bonus(std::uint8_t depth) {
  const int d = depth;
  return std::min(HISTORY_BONUS_SCALE * d * d, HISTORY_BONUS_CAP);
}

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
constexpr std::array<int, 3> FUTILITY_MARGIN = {0, 100, 300}; // margins for depth 0, 1, 2
constexpr int FUTILITY_DEPTH = 2;

// =============================================================================
// DELTA PRUNING
// =============================================================================
// Futility pruning's idea, moved into quiescence. There the moves all capture
// something, so the question is not "can a quiet move help?" but "can THIS
// capture help?", and the most optimistic answer is exactly computable: the
// side to move keeps its stand-pat score and wins the whole victim, free.
//
//     stand_pat + what the capture wins + margin < alpha  ->  skip it
//
// If even that best case does not reach alpha, searching the capture cannot
// change this node's score. The margin is the admission that "stand pat plus
// the victim" is not really the ceiling—a recapture can be answered, a check
// can follow—so two hundred centipawns of benefit of the doubt are added
// before anything is thrown away.
//
// TWO EXEMPTIONS, both places where the arithmetic stops describing the
// position:
//   - IN CHECK. There is no stand-pat score to build on, because the side to
//     move is not allowed to stand pat.
//   - LATE ENDGAMES. With no pieces left beyond pawns, a single promotion is
//     worth more than the entire margin, and a static evaluation that has not
//     seen the pawn race is a poor thing to prune on. The classic condition,
//     and the one place delta pruning is known to lose games.
//
// FAILURE MODE: like every form of pruning, this is a claim about material
// made where material is not the point. A capture that walks into a mating net
// or a perpetual is pruned on the grounds that it does not win enough wood.
// =============================================================================
constexpr int DELTA_PRUNING_MARGIN = 200;

// What a noisy move wins at its most optimistic: the piece it takes, plus the
// difference between the piece a promoting pawn becomes and the pawn itself.
int optimistic_gain(const Move& mv) {
  int gain = 0;
  if (mv.captured_piece.has_value()) {
    gain += PIECE_VALUES[static_cast<std::size_t>(*mv.captured_piece)];
  }
  if (mv.promotion_piece.has_value()) {
    gain += PIECE_VALUES[static_cast<std::size_t>(*mv.promotion_piece)] -
            PIECE_VALUES[static_cast<std::size_t>(mv.piece)];
  }
  return gain;
}

// =============================================================================
// LATE MOVE REDUCTIONS (LMR)
// =============================================================================
// Move ordering is a claim: the moves at the front of the list are the ones
// worth looking at. Alpha-beta only exploits half of that claim—it searches the
// promising moves first and hopes for a cutoff. LMR exploits the other half: if
// we really believe a quiet move ranked twentieth is unlikely to be best, we
// should not spend the same depth on it as on the first move.
//
// So a late quiet move is searched SHALLOWER, with a zero window. Almost always
// it fails low, which is what ordering predicted, and the node has bought a
// whole subtree at a fraction of the price. When it does not—when the reduced
// search beats alpha—the move is searched again at full depth, and if it is
// still inside the window, once more with the full window to get its true score
// and its line. Being wrong therefore costs a re-search, not a wrong answer,
// which is why reductions can be aggressive in a way that pruning cannot.
//
// THE SHAPE OF THE TABLE
//   reduction = floor(0.75 + ln(depth) * ln(move number) / 2.25)
// Both logs matter. Deeper searches can afford to give up more plies, because
// what is left is still a real search; and confidence that a move is bad grows
// with how far down the list ordering put it, but only slowly—the fiftieth move
// is not ten times more hopeless than the fifth. The constants are the ones the
// engines that measured them settled on.
//
// WHO IS EXEMPT, AND WHY
//   - The first few moves. The whole point is that they are the likely best.
//   - Captures and promotions. Material swings are exactly what a shallow
//     search mishandles.
//   - Moves made or given in check. A forcing line is short and must be seen
//     to its end; reducing it is how an engine walks into a mate it had time
//     to see.
//   - Killers and counter-moves. These are quiet moves the search has specific
//     evidence for, and evidence is what reductions are supposed to respect.
//   - PV nodes reduce one ply less. A PV node's job is to produce the true
//     score and the line behind it, and it is the node whose mistakes are most
//     expensive: an error there changes the move we play, while an error in a
//     zero-window node usually only costs a re-search.
//
// AND WHAT THE TABLE ENDS UP BELIEVING. A move that fails low after a reduced
// search is never looked at again, so the node's score—and the upper bound it
// writes to the transposition table—is stamped with the node's FULL depth even
// though part of the tree behind it was searched shallower. That is the
// standard optimism: the bound is treated as if it had been earned at full
// depth, and a later search that trusts it inherits whatever the reduction
// missed. It is the price of the saving, and it is why the exemptions matter.
//
// FAILURE MODE: TACTICAL BLINDNESS. A quiet move can be a quiet SACRIFICE
// setup, a mating net, a zugzwang move—brilliant precisely because it looks
// like nothing. Reduced by three plies, its refutation-or-vindication may lie
// past the horizon, the reduced search fails low, and nothing triggers the
// re-search that would have found it. The exemptions above are the cheap
// insurance against the common cases; the rest is a bet that the moves ordering
// ranks last are usually as bad as they look, and it is a bet that pays.
// =============================================================================

constexpr std::uint8_t LMR_MIN_DEPTH = 3;

// How many moves at the front of a node's list are searched at full depth. The
// name says what the number IS rather than where it is compared: the first
// three moves searched are never reduced, so the fourth is the first that can
// be. Spelling it as a minimum move number invited the off-by-one of reading it
// as "the first reduced move is number 3".
constexpr std::size_t LMR_UNREDUCED_MOVES = 3;

constexpr double LMR_BASE = 0.75;
constexpr double LMR_DIVISOR = 2.25;

// The table is consulted with clamped indices, so these bound the arithmetic,
// not the search: beyond them the reduction simply stops growing.
constexpr std::size_t LMR_TABLE_DEPTHS = 64;
constexpr std::size_t LMR_TABLE_MOVES = 64;

// Logarithms are not constexpr in this standard, so the table is filled once at
// start-up rather than at compile time. It is read-only from then on.
struct LmrTable {
  std::array<std::array<std::uint8_t, LMR_TABLE_MOVES>, LMR_TABLE_DEPTHS> reductions{};

  LmrTable() {
    for (std::size_t depth = 1; depth < LMR_TABLE_DEPTHS; ++depth) {
      for (std::size_t move_number = 1; move_number < LMR_TABLE_MOVES; ++move_number) {
        const double reduction =
            LMR_BASE + (std::log(static_cast<double>(depth)) *
                        std::log(static_cast<double>(move_number)) / LMR_DIVISOR);
        reductions[depth][move_number] =
            static_cast<std::uint8_t>(std::max(0.0, std::floor(reduction)));
      }
    }
  }
};

const LmrTable LMR_TABLE;

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
// History heuristic
// ---------------------------------------------------------------------------

namespace {

std::size_t history_colour_index(const Move& mv) {
  return static_cast<std::size_t>(colour(mv.piece));
}

} // namespace

int HistoryTable::probe(const Move& mv) const {
  return scores_[history_colour_index(mv)][mv.from.index()][mv.to.index()];
}

void HistoryTable::update(const Move& mv, int bonus) {
  // A bonus larger than the ceiling would make the gravity term overshoot and
  // push the score the wrong way, so it is clamped before anything else.
  const int clamped = std::clamp(bonus, -HISTORY_MAX, HISTORY_MAX);
  int& score = scores_[history_colour_index(mv)][mv.from.index()][mv.to.index()];

  // The gravity term: nothing while the score is small, exactly -bonus once the
  // score reaches HISTORY_MAX. See the header for why this shape is wanted.
  score += clamped - ((score * std::abs(clamped)) / HISTORY_MAX);
}

void HistoryTable::clear() {
  for (auto& by_colour : scores_) {
    for (auto& by_from : by_colour) {
      by_from.fill(0);
    }
  }
}

// ---------------------------------------------------------------------------
// Counter moves
// ---------------------------------------------------------------------------

std::optional<Move> CounterMoves::probe(const Move& previous) const {
  return moves_[static_cast<std::size_t>(previous.piece)][previous.to.index()];
}

void CounterMoves::store(const Move& previous, const Move& refutation) {
  moves_[static_cast<std::size_t>(previous.piece)][previous.to.index()] = refutation;
}

void CounterMoves::clear() {
  for (auto& by_piece : moves_) {
    by_piece.fill(std::nullopt);
  }
}

// ---------------------------------------------------------------------------
// Search context
// ---------------------------------------------------------------------------

// The rows are allocated—and zeroed—once, here, rather than a frame at a time
// inside the recursion. See WHY THE SCRATCH SPACE LIVES HERE in the header.
SearchContext::SearchContext()
    : plies_(std::make_unique<std::array<PlyScratch, MAX_DEPTH + 1>>()),
      quiescence_(std::make_unique<std::array<MoveScratch, QUIESCENCE_MAX_DEPTH>>()) {}

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
//   5. Counter-move       the quiet move that last refuted the move the
//                         opponent has just played
//   6. Everything else    quiet moves, ranked by their history score
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
constexpr int ORDER_COUNTER_MOVE = 700'000;

// Quiet moves are ranked by history alone, which saturates at ±HISTORY_MAX and
// so can never climb into the counter-move band above.
static_assert(HISTORY_MAX < ORDER_COUNTER_MOVE, "history must not outrank a counter-move");

// The busiest legal position anyone has constructed offers 218 moves, and 256
// is the round number engines conventionally allow for. Nothing is written past
// it: a position that somehow offered more would simply have its surplus moves
// searched in generation order (see select()), never scored out of bounds.
constexpr std::size_t MAX_SCORED_MOVES = MOVE_LIST_RESERVE;

// The scores live in a row the caller owns—a SearchContext's per-ply scratch
// during a search—rather than in this object. A quarter-kilobyte array of ints
// is small in isolation and ruinous 255 frames deep; see WHY THE SCRATCH SPACE
// LIVES HERE in search.hpp.
class OrderedMoves {
public:
  // Main search: the full priority list above.
  OrderedMoves(MoveList& moves, std::span<int> scores, const SearchContext& ctx, std::uint8_t ply,
               const std::optional<Move>& hash_move, const std::optional<Move>& previous_move)
      : moves_(moves), scores_(scores),
        scored_(std::min({moves.size(), scores.size(), MAX_SCORED_MOVES})),
        killer1_(ctx.killers.probe(ply, 0)), killer2_(ctx.killers.probe(ply, 1)),
        counter_(previous_move.has_value() ? ctx.counters.probe(*previous_move) : std::nullopt) {
    for (std::size_t i = 0; i < scored_; ++i) {
      scores_[i] = score(moves_[i], ctx.history, hash_move);
    }
  }

  // Quiescence: every move there is a capture or a promotion, so MVV-LVA is
  // the whole ordering—there are no killers or quiet moves to rank.
  OrderedMoves(MoveList& moves, std::span<int> scores)
      : moves_(moves), scores_(scores),
        scored_(std::min({moves.size(), scores.size(), MAX_SCORED_MOVES})) {
    for (std::size_t i = 0; i < scored_; ++i) {
      scores_[i] = noisy_move_score(moves_[i]);
    }
  }

  // The quiet moves this node has specific reason to believe in. Reductions
  // leave them alone: they are the moves most likely to be the exception that
  // a reduced search would miss.
  [[nodiscard]] bool is_refutation(const Move& mv) const {
    return (killer1_.has_value() && mv == *killer1_) || (killer2_.has_value() && mv == *killer2_) ||
           (counter_.has_value() && mv == *counter_);
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
  int score(const Move& mv, const HistoryTable& history,
            const std::optional<Move>& hash_move) const {
    // The hash move is the best move a previous (usually deeper) search found
    // here, so nothing outranks it—not even a queen capture.
    if (hash_move.has_value() && mv == *hash_move) {
      return ORDER_HASH_MOVE;
    }

    if (!is_quiet(mv)) {
      return ORDER_NOISY + noisy_move_score(mv);
    }

    if (killer1_.has_value() && mv == *killer1_) {
      return ORDER_KILLER_1;
    }

    if (killer2_.has_value() && mv == *killer2_) {
      return ORDER_KILLER_2;
    }

    if (counter_.has_value() && mv == *counter_) {
      return ORDER_COUNTER_MOVE;
    }

    // Everything left is a quiet move with nothing special about it, and
    // history is the only thing that distinguishes one from another. A move
    // that has been failing is ranked BELOW an untried one, which is the whole
    // point of the malus.
    return history.probe(mv);
  }

  MoveList& moves_;
  // Entries [0, scored_) are written by the constructor before anything reads
  // them, so nothing here depends on what the row held before.
  std::span<int> scores_;
  std::size_t scored_;
  std::optional<Move> killer1_;
  std::optional<Move> killer2_;
  std::optional<Move> counter_;
};

static_assert(MAX_ORDERED_MOVES >= MAX_SCORED_MOVES,
              "a scratch row must be long enough to score any move list");

} // namespace

void detail::order_moves(MoveList& moves, const SearchContext& ctx, std::uint8_t ply,
                         const std::optional<Move>& hash_move,
                         const std::optional<Move>& previous_move) {
  // The eager form: run the lazy selection all the way to the end. The search
  // itself never calls this—it asks for one move at a time and usually stops
  // after the first—but "the whole list, in order" is what a test can read.
  // Its score row is a local because nothing recursive calls this.
  std::array<int, MAX_ORDERED_MOVES> scores{};
  OrderedMoves ordering(moves, scores, ctx, ply, hash_move, previous_move);
  for (std::size_t i = 0; i < moves.size(); ++i) {
    ordering.select(i);
  }
}

void detail::order_quiescence_moves(MoveList& moves) {
  std::array<int, MAX_ORDERED_MOVES> scores{};
  OrderedMoves ordering(moves, scores);
  for (std::size_t i = 0; i < moves.size(); ++i) {
    ordering.select(i);
  }
}

std::uint8_t detail::lmr_reduction(std::uint8_t depth, std::size_t move_number, bool is_pv_node) {
  const auto depth_index = std::min<std::size_t>(depth, LMR_TABLE_DEPTHS - 1);
  const auto move_index = std::min<std::size_t>(move_number, LMR_TABLE_MOVES - 1);

  const std::uint8_t reduction = LMR_TABLE.reductions[depth_index][move_index];

  // A PV node is the one whose mistakes change the move we play, so it gives up
  // one ply less than the zero-window nodes around it.
  if (is_pv_node && reduction > 0) {
    return static_cast<std::uint8_t>(reduction - 1);
  }

  return reduction;
}

// ---------------------------------------------------------------------------
// STATIC EXCHANGE EVALUATION
// ---------------------------------------------------------------------------
// The swap list. Capturing on a square starts an argument: I take, you take
// back with your cheapest piece, I take back with mine, and so on until one
// side runs out of attackers or decides it would rather stop. see() plays that
// argument out on a private copy of the board and reports what the side that
// started it ends up with.
//
// TWO PASSES. Forward, we record what each capture wins if it happens:
// gain[d] = value of the piece standing on the square - gain[d - 1]. Backward,
// we let each side refuse: nobody is obliged to recapture, so
// gain[d - 1] = -max(-gain[d - 1], gain[d]) turns the list of "if everyone
// captures" numbers into "what actually happens when both sides play sensibly".
// The forward pass also stops early once neither side can profit from
// continuing, which is the same decision made one step sooner.
//
// CHEAPEST ATTACKER FIRST is not a heuristic here, it is the rule: recapturing
// with the queen when a pawn could do it hands the opponent a better trade, so
// the least valuable attacker is always the right one to use.
//
// X-RAYS COME FREE. Pieces are physically removed from the board copy as they
// capture, so a queen sitting behind a rook on the same file simply appears in
// the next get_attackers() call. That is why this works on a Board rather than
// on a precomputed set of attackers, and it is what makes a battery score the
// way a human would read it.
//
// See the header for what SEE deliberately does not know—pins, most of all.
// ---------------------------------------------------------------------------

namespace {

// The king is worth more than everything else on the board put together, so
// that "the king is the cheapest attacker left" can never look like a bargain.
// The other values are the plain middlegame ones the ordering already uses.
constexpr int SEE_KING_VALUE = 10'000;

int see_value(Piece piece) {
  return is_king(piece) ? SEE_KING_VALUE : PIECE_VALUES[static_cast<std::size_t>(piece)];
}

// One exchange per piece that can reach the square, and there are 32 pieces.
constexpr std::size_t SEE_MAX_SWAPS = 32;

// pieces_for() lists a colour's pieces in ascending value—pawn, knight, bishop,
// rook, queen, king—so the first one that appears among the attackers is the
// cheapest.
std::optional<Piece> least_valuable_attacker(Bitboard attackers, Colour side, const Board& board) {
  for (const auto piece : pieces_for(side)) {
    if ((attackers & board.pieces(piece)) != 0) {
      return piece;
    }
  }
  return std::nullopt;
}

} // namespace

int detail::see(const Position& pos, const Move& mv) {
  // Nothing changes hands, so there is no exchange to evaluate.
  if (is_quiet(mv)) {
    return 0;
  }

  const Square target = mv.to;
  Board board = pos.board;

  std::array<int, SEE_MAX_SWAPS> gain{};

  // What the move itself wins: whatever it captured, plus—if it promotes—the
  // difference between the piece the pawn becomes and the pawn it was.
  const int promotion_gain =
      mv.promotion_piece.has_value() ? see_value(*mv.promotion_piece) - see_value(mv.piece) : 0;
  gain[0] = (mv.captured_piece.has_value() ? see_value(*mv.captured_piece) : 0) + promotion_gain;

  // Play the move on the copy. capture_square() is the target for an ordinary
  // capture and the square behind it for en passant, which is exactly the
  // distinction that decides which piece leaves the board.
  if (const auto captured_square = mv.capture_square()) {
    board.remove_piece(*captured_square);
  }
  board.remove_piece(mv.from);
  Piece occupier = mv.promotion_piece.value_or(mv.piece);
  board.put_piece(occupier, target);

  Colour side = !colour(mv.piece);
  std::size_t depth = 0;

  while (true) {
    const Bitboard attackers = get_attackers(target, side, board);
    const auto attacker = least_valuable_attacker(attackers, side, board);
    if (!attacker.has_value()) {
      break;
    }

    // A king may only capture on a square the other side no longer defends, so
    // if anything still defends it the exchange stops here rather than ending
    // in an illegal move. (Read from the board as it stands, which misses the
    // rare defender that only appears once the king leaves its own square.)
    if (is_king(*attacker) && get_attackers(target, !side, board) != 0) {
      break;
    }

    if (depth + 1 >= SEE_MAX_SWAPS) {
      break;
    }
    ++depth;

    // What this capture wins, assuming it happens: the piece standing on the
    // square, less whatever the previous capture had already banked.
    gain[depth] = see_value(occupier) - gain[depth - 1];

    // Neither side can profit from carrying on—the one to move here would
    // rather stop, and so would the one before it. Everything past this point
    // would be refused by the backward pass anyway.
    if (std::max(-gain[depth - 1], gain[depth]) < 0) {
      break;
    }

    const Square from = Square::first_occupied(attackers & board.pieces(*attacker));
    board.remove_piece(target);
    board.remove_piece(from);
    board.put_piece(*attacker, target);
    occupier = *attacker;
    side = !side;
  }

  // Backward pass: at every step the side to move may decline the recapture, so
  // it takes the better of "stop here" and "carry on".
  while (depth > 0) {
    gain[depth - 1] = -std::max(-gain[depth - 1], gain[depth]);
    --depth;
  }

  return gain[0];
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
// EXCEPT IN CHECK, WHERE STANDING PAT IS A LIE
// "I do not have to capture" is the one claim a side in check cannot make: it
// must answer the check or the game is over. A quiescence node that stands pat
// in check therefore reports the static evaluation of a position the side to
// move may not be allowed to keep—and the position it is hiding may be
// checkmate, which no material count resembles. So in check quiescence takes
// no stand-pat score at all, generates every legal reply rather than only the
// captures (blocking and king moves are usually the only answers there are),
// and returns a mate score if none of them is legal. That makes a check node
// behave like a small alpha-beta node inside quiescence, which is what it is:
// the position is not quiet, and pretending otherwise is how an engine walks
// into a mate that was one ply past its horizon.
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

int detail::quiescence(Position& pos, int alpha, int beta, SearchContext& ctx, Report& report,
                       const Stopper& stopper, std::size_t quiescence_depth) {
  if (stopper.should_stop(report)) {
    return 0;
  }

  // THE QUIESCENCE DEPTH CAP
  // "Search captures until the position is quiet" is a promise with no bound in
  // it. In practice the bound is the board itself—every recursion has to find
  // another capture or promotion, and there are only so many pieces to trade—
  // but "in practice" is not a guarantee, and this recursion holds a move list
  // per level. Past the cap we simply return the static evaluation, which is
  // what a quiet position would have returned anyway. See QUIESCENCE_MAX_DEPTH.
  if (quiescence_depth >= QUIESCENCE_MAX_DEPTH) {
    return eval(pos);
  }

  // THIS position has already been counted—by the alphabeta leaf that handed
  // it over, or by the recursive call below. Quiescence counts each child it
  // decides to visit instead, which keeps every position worth exactly one
  // node whichever of the two searches resolves it.

  const Colour colour_to_move = pos.colour_to_move;
  const bool in_check = is_in_check(colour_to_move, pos.board);

  int stand_pat = 0;

  if (!in_check) {
    // Stand-pat: static evaluation as the fallback
    stand_pat = eval(pos);

    // Beta cutoff: position is already too good, opponent won't allow this
    if (stand_pat >= beta) {
      return beta;
    }

    // Update alpha: we can always achieve at least the stand-pat score
    alpha = std::max(alpha, stand_pat);
  }

  // Delta pruning is arithmetic about material, and in a position where the
  // only material left is pawns a promotion outweighs the whole margin. See
  // DELTA PRUNING for why this is where it is switched off.
  const bool late_endgame = !has_non_pawn_material(pos.board, colour_to_move);

  // Everything below is a bet that a capture cannot matter; a check makes every
  // move matter, so none of it applies there.
  const bool prune = ctx.quiescence_pruning_enabled && !in_check;

  // Normally only the noisy moves (captures and promotions); in check, every
  // legal reply, because the check has to be answered and the answer is often a
  // quiet block or a king step. The list and its ordering scores come from the
  // context's row for this quiescence depth, so this frame holds neither of
  // them. Ordering is MVV-LVA either way—among evasions that simply means
  // capturing the checker is tried first, which is usually right.
  MoveScratch& scratch = ctx.at_quiescence_depth(quiescence_depth);
  MoveList& moves = scratch.moves;
  moves = in_check ? pseudo_legal_moves(pos) : pseudo_legal_noisy_moves(pos);
  OrderedMoves ordering(moves, scratch.scores);

  std::size_t legal_moves = 0;

  for (std::size_t i = 0; i < moves.size(); ++i) {
    const Move mv = ordering.select(i);

    // ONLY QUEEN PROMOTIONS
    // A promotion is generated four times over, once per piece the pawn can
    // become, and three of those are almost always the wrong answer: if
    // promoting is good at all, the queen is what makes it good. The capture is
    // not lost with them—bxa8=Q takes the same rook bxa8=N would—so quiescence
    // sees the same material either way, three times cheaper.
    //
    // What IS given up is the underpromotion that is brilliant for a reason
    // material cannot see: the knight check that forks, the rook that promotes
    // to avoid stalemating the opponent. Those are left to the main search,
    // which generates all four and is where a tactic that subtle belongs.
    if (prune && mv.promotion_piece.has_value() && *mv.promotion_piece != queen(colour_to_move)) {
      continue;
    }

    // DELTA PRUNING: even winning the victim outright would not reach alpha.
    if (prune && !late_endgame && stand_pat + optimistic_gain(mv) + DELTA_PRUNING_MARGIN < alpha) {
      continue;
    }

    // LOSING CAPTURES: the exchange on that square comes out badly however it
    // is played, so searching it only rediscovers that. MVV-LVA cannot tell—it
    // ranks QxP above every quiet move whether or not the pawn is defended—and
    // this is the check it is missing. Only strictly losing captures go: an
    // even trade (SEE == 0) may still be exactly the move that resolves the
    // position, which is what quiescence is for.
    if (prune && detail::see(pos, mv) < 0) {
      continue;
    }

    pos.make_move(mv);

    // Skip illegal moves (leave king in check)
    if (is_in_check(colour_to_move, pos.board)) {
      pos.unmake_move(mv);
      continue;
    }

    ++legal_moves;

    // The child is a position we are about to enter and decide, so it is
    // counted here—the callee will not count itself.
    report.nodes += 1;

    // Negamax: negate score and swap alpha/beta
    const int score = -quiescence(pos, -beta, -alpha, ctx, report, stopper, quiescence_depth + 1);

    pos.unmake_move(mv);

    if (score >= beta) {
      return beta; // Beta cutoff
    }
    alpha = std::max(alpha, score);
  }

  // In check with nothing legal to play is checkmate, and it is only reachable
  // here because the in-check branch generated ALL the moves: with captures
  // alone an empty list would mean "quiet", not "over". Stalemate cannot land
  // here—a side with no legal move and no check is not in check by definition.
  //
  // The distance is counted from the root, so it is the alphabeta ply this
  // quiescence started from plus however deep into quiescence we now are.
  if (in_check && legal_moves == 0) {
    return -CENTIPAWN_MATE + static_cast<int>(report.ply) + static_cast<int>(quiescence_depth);
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
                      TranspositionTable& tt, SearchContext& ctx, Report& report,
                      const Stopper& stopper, const std::optional<Move>& previous_move) {
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
  report.max_ply = std::max(report.max_ply, report.ply);

  // Draw detection: 50-move rule or threefold repetition
  if (pos.is_fifty_move_draw() || pos.is_repetition_draw(report.ply)) {
    return CENTIPAWN_DRAW;
  }

  // THE PLY CEILING
  // Everything indexed by ply—killers, and this context's per-ply scratch
  // rows—has exactly MAX_DEPTH + 1 rows, and report.ply is a single byte, so
  // recursing from here would wrap round to ply 0 and start overwriting the
  // root's own tables. Quiescence still resolves the captures, so the answer
  // is a real one; it simply stops growing the tree in a direction that has
  // nowhere left to go. See THE PLY CEILING in search.hpp.
  if (report.ply >= MAX_DEPTH) {
    return quiescence(pos, alpha, beta, ctx, report, stopper);
  }

  // Leaf node: drop into quiescence search
  if (depth == 0) {
    if (!is_in_check(pos.colour_to_move, pos.board)) {
      return quiescence(pos, alpha, beta, ctx, report, stopper);
    }
    // CHECK EXTENSION: Don't stop search while in check (tactical danger).
    // This is the one place the recursion does not spend a ply of depth, so it
    // is also the only way ply can grow without bound. The ceiling above is
    // what stops it: we can only get here with report.ply < MAX_DEPTH, so the
    // child this extension creates still sits inside every per-ply table.
    depth = 1;
  }

  // PV NODES vs NON-PV NODES
  // A node searched with a full window (beta > alpha + 1) is a "PV node": it
  // is on the principal variation, and its job is not just to produce a score
  // but to produce the line of play behind that score. A node searched with a
  // zero window (beta == alpha + 1) is a non-PV node; it only ever answers
  // "better or worse than alpha?", so it has no line to report.
  const bool is_pv_node = beta - alpha > 1;

  // report.ply moves up and down around every child call below, so this node's
  // own ply is named once here: it is what the scratch rows are indexed by, and
  // what mate distances and killer slots are measured in.
  const std::uint8_t ply = report.ply;
  PlyScratch& scratch = ctx.at_ply(ply);

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
      const int tt_eval = eval_out(entry->score, ply);

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
    // Zero-window search: just checking if score >= beta
    // No previous move to answer: we did not make one. A counter-move keyed by
    // the opponent's last real move would be answering the wrong question.
    // The line it produces is thrown away—a zero window proves "better or worse
    // than beta" and nothing else—so it is written into the child's own row and
    // never read.
    const int null_eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - r - 1), -beta,
                                     -beta + 1, ctx.at_ply(static_cast<std::uint8_t>(ply + 1)).pv,
                                     tt, ctx, report, stopper, std::nullopt);

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
        tt.store(pos.key, depth, eval_in(null_bound, ply), Bound::Lower, TT_NO_MOVE);
      }
      return beta;
    }
  }

  // Static evaluation for futility pruning (only compute at shallow depths when not in check)
  const int static_eval = (depth <= FUTILITY_DEPTH && !in_check) ? eval(pos) : 0;

  MoveList& moves = scratch.ordering.moves;
  moves = pseudo_legal_moves(pos);

  // Turn the 16 packed bits back into a real Move by finding it among the
  // moves this position actually has. This is the only place a stored move is
  // allowed to become playable, so a corrupted or colliding entry can never
  // reach make_move(). `hash_move` is const on purpose: it is the move a
  // PREVIOUS search recommended, and confusing it with the best move THIS
  // search is finding would make us search the same move twice.
  const std::optional<Move> hash_move = decode_tt_move(hash_move_packed, moves);

  OrderedMoves ordering(moves, scratch.ordering.scores, ctx, ply, hash_move, previous_move);

  // The line this node's children report back into. One row per ply, so the
  // child's PV is written where the child's own row lives and this frame holds
  // no move list at all.
  MoveList& child_pv = ctx.at_ply(static_cast<std::uint8_t>(ply + 1)).pv;

  // How many legal moves this node has actually searched, so the move about to
  // be searched is number `moves_searched + 1`. Late move reductions are a
  // statement about that number, and futility pruning has always needed to know
  // whether anything had been searched at all.
  std::size_t moves_searched = 0;
  Bound tt_bound = Bound::Upper;

  // The best move THIS search finds, kept apart from `hash_move` above.
  std::optional<Move> best_move = std::nullopt;

  // The quiet moves searched here so far. If one of the moves after them causes
  // a cutoff, these are the ones that were tried first and did not work, and
  // history wants to hear about it.
  auto& searched_quiets = scratch.searched_quiets;
  std::size_t searched_quiet_count = 0;

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
    if (moves_searched > 0 && depth <= FUTILITY_DEPTH && !in_check && is_quiet(mv) &&
        static_eval + FUTILITY_MARGIN[static_cast<std::size_t>(depth)] <= alpha) {
      pos.unmake_move(mv);
      continue;
    }

    ++moves_searched;
    report.ply += 1;

    int eval;

    if (moves_searched == 1) {
      // First move: search with full window. Ordering says this is the likely
      // best move, and the score the rest are measured against has to come
      // from somewhere.
      //
      // child_pv is a shared row, so it is emptied before every search whose
      // line we might keep. A local list gave that for free; a reused one would
      // otherwise hand us the previous sibling's line.
      child_pv.clear();
      eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -beta, -alpha, child_pv, tt, ctx,
                        report, stopper, mv);
    } else {
      // LATE MOVE REDUCTIONS
      // A quiet move ordering ranked near the back is searched shallower. The
      // check for whether the move GIVES check is last because it costs a scan
      // of the board and the cheap conditions usually decide the question.
      std::uint8_t reduction = 0;
      if (ctx.reductions_enabled && depth >= LMR_MIN_DEPTH &&
          moves_searched > LMR_UNREDUCED_MOVES && is_quiet(mv) && !in_check &&
          !ordering.is_refutation(mv) && !is_in_check(pos.colour_to_move, pos.board)) {
        reduction = lmr_reduction(depth, moves_searched, is_pv_node);

        // Never reduce into quiescence: a move searched at depth 0 is a move
        // nobody looked at, which is pruning, not reducing. depth is at least
        // LMR_MIN_DEPTH here, so there is always at least one ply to keep.
        reduction = std::min(reduction, static_cast<std::uint8_t>(depth - 2));
      }

      // PRINCIPAL VARIATION SEARCH (PVS)
      // After the first move, search the rest with a "zero window"
      // (alpha, alpha+1). That only proves "worse than alpha" or "better", but
      // it proves it far more cheaply than a real search.
      //
      // A zero-window search has no line to report—it answers a yes/no question
      // about alpha—so these two calls write into the same shared row as the
      // full-window search below, and whatever they leave there is overwritten
      // by the clear() before that search. Giving each of them a list of its
      // own is what used to put three move lists in this frame.
      eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1 - reduction), -alpha - 1, -alpha,
                        child_pv, tt, ctx, report, stopper, mv);

      // The reduced search says the move is better than alpha, and a reduced
      // search is not entitled to that opinion: verify it at full depth, still
      // with the zero window.
      if (reduction > 0 && eval > alpha) {
        eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -alpha - 1, -alpha, child_pv,
                          tt, ctx, report, stopper, mv);
      }

      // Still looking like a new best move, so pay for the real thing: the full
      // window is what produces a true score and the line behind it.
      if (eval > alpha && eval < beta) {
        child_pv.clear();
        eval = -alphabeta(pos, static_cast<std::uint8_t>(depth - 1), -beta, -alpha, child_pv, tt,
                          ctx, report, stopper, mv);
      }
    }

    report.ply -= 1;
    pos.unmake_move(mv);

    // BETA CUTOFF: This move is too good—opponent won't allow this position
    if (eval >= beta) {
      // A quiet move that causes a cutoff is the only kind worth remembering.
      // Captures need no help: MVV-LVA already ranks them, and recording them
      // here would crowd out the quiet moves these tables exist to rescue from
      // the back of the list.
      if (is_quiet(mv)) {
        ctx.killers.store(ply, mv);

        const int bonus = history_bonus(depth);
        ctx.history.update(mv, bonus);

        // ...and the quiet moves that were searched before it are evidence
        // against themselves: they were ranked ahead of the move that actually
        // worked, and the malus is what moves them back.
        for (std::size_t q = 0; q < searched_quiet_count; ++q) {
          ctx.history.update(searched_quiets[q], -bonus);
        }

        if (previous_move.has_value()) {
          ctx.counters.store(*previous_move, mv);
        }
      }

      // Store in TT as a lower bound (actual score might be even higher)
      if (!stopper.has_stopped()) {
        tt.store(pos.key, depth, eval_in(eval, ply), Bound::Lower, encode_tt_move(mv));
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

    if (is_quiet(mv) && searched_quiet_count < searched_quiets.size()) {
      searched_quiets[searched_quiet_count] = mv;
      ++searched_quiet_count;
    }
  }

  // No legal moves: either checkmate or stalemate
  if (moves_searched == 0) {
    // Checkmate: return negative mate score (we're getting mated)
    // Stalemate: draw
    return in_check ? -CENTIPAWN_MATE + ply : CENTIPAWN_DRAW;
  }

  // Store the result in the transposition table for future use.
  // If the stopper fired somewhere below us, `alpha` was assembled from
  // children that returned a placeholder 0 instead of a real score. Writing
  // that into the table would poison the position with a fabricated "equal"
  // evaluation that outlives the aborted search, so we say nothing.
  if (!stopper.has_stopped()) {
    tt.store(pos.key, depth, eval_in(alpha, ply), tt_bound,
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

  // Everything this search learns about move ordering, built here and thrown
  // away on return. See SearchContext in the header for why it does not outlive
  // one search the way the transposition table does.
  SearchContext ctx;
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

      const int eval = detail::alphabeta(pos, depth, alpha, beta, pv, tt, ctx, report, stopper);

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
