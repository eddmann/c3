// =============================================================================
// STATIC EVALUATION IMPLEMENTATION
// =============================================================================
//
// How a raw position becomes a single centipawn number:
//
//   1. DEAD DRAW?      If neither side can possibly deliver mate (K vs K, K+B
//                      vs K, ...), stop immediately and return 0.
//   2. MATERIAL        Add up piece values for each side.
//   3. PIECE-SQUARE    Add a bonus/penalty for every piece depending on the
//                      square it stands on (PSQT).
//   4. TWICE OVER      Steps 2 and 3 are run with two different sets of
//                      numbers: one tuned for the middlegame, one for the
//                      endgame.
//   5. BISHOP PAIR     Add a bonus to a side owning two bishops of opposite
//                      square colours.
//   6. TAPER           Blend the two totals according to the game phase.
//   7. CLAMP + FLIP    Keep the result below mate scores, then flip the sign
//                      if black is to move.
//
// Steps 2, 3 and 4 are not actually performed here. They decompose into "what
// is this piece on this square worth", so the Board keeps a running total of
// them (see eval_terms.hpp) and eval() reads it. eval_material() and
// eval_psqt() below still compute the same quantities the slow way: they are
// the reference implementation the tests—and the Debug assertions in
// position.cpp—measure the running total against.
//
// WHY TAPER? A single set of numbers cannot describe both halves of a chess
// game, and the king is the clearest example. With queens on the board the
// king wants to hide on g1 behind its pawns; once the queens are gone the same
// king is a strong piece that belongs in the centre, marching towards enemy
// pawns. A middlegame-only table would keep our king cowering in the corner
// for the whole endgame; an endgame-only table would walk it into a mating net
// on move 12. Tapering computes BOTH scores and mixes them in proportion to
// how much material is left, so the engine's opinion drifts smoothly from one
// to the other as pieces come off—no sudden jump at an arbitrary cutoff.
//
//   score = (middlegame_score * phase + endgame_score * (24 - phase)) / 24
//
// =============================================================================

#include "c3/eval.hpp"

#include <algorithm>
#include <array>

#include "c3/bitboard.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {
namespace {

// The piece-square tables, the piece values and the phase weights all live in
// eval_terms.hpp now, because Board keeps a running total of them and therefore
// has to see them too. What is left in this file is how those numbers are
// combined into a single score.

// The 32 squares of each colour. A bishop can never leave the square colour it
// started on, which is what makes some bishop endings impossible to win.
constexpr Bitboard DARK_SQUARES = 0xAA55'AA55'AA55'AA55ULL;
constexpr Bitboard LIGHT_SQUARES = ~DARK_SQUARES;

constexpr std::size_t phase_index(Phase phase) noexcept {
  return static_cast<std::size_t>(phase);
}

int count_of(const Board& board, Piece piece) noexcept {
  return static_cast<int>(board.count_pieces(piece));
}

// Two bishops are only a "pair" when they cover different square colours—that
// is what makes them able to hit every square on the board. Two bishops on one
// colour (which promotions can produce) still leave half the board untouched,
// so they earn nothing extra.
bool has_bishop_pair(const Board& board, Colour side) noexcept {
  const Bitboard bishops = board.pieces(bishop(side));
  return (bishops & LIGHT_SQUARES) != 0 && (bishops & DARK_SQUARES) != 0;
}

} // namespace

// Non-pawn material left on the board, on the 24-point scale described in eval_terms.hpp.
int game_phase(const Board& board) noexcept {
  int phase = 0;
  for (const auto piece : all_pieces()) {
    phase += PHASE_WEIGHTS[static_cast<std::size_t>(piece) % 6] * count_of(board, piece);
  }
  return std::min(phase, PHASE_MAX);
}

// =============================================================================
// INSUFFICIENT MATERIAL
// =============================================================================
// Some positions cannot be won by anybody: there is not enough material left on
// the board to construct a checkmate, however badly the defender plays. The
// rules of chess declare these positions drawn immediately, and an engine that
// does not know this will happily "win" a piece and then report +330 in a
// position that is stone dead—and, worse, steer towards it.
//
// We recognise the classic dead draws:
//   - K vs K            nothing to mate with
//   - K+N vs K          a lone knight cannot cover the squares a mate needs
//   - K+B vs K          a lone bishop only ever controls half the board
//   - one minor each    K+B vs K+B (same or opposite colours), K+B vs K+N and
//                       K+N vs K+N are all drawn: one minor piece plus a king
//                       simply cannot corner a king that is not helping
//   - K+N+N vs K        two knights cannot FORCE mate: mating positions exist,
//                       but the defender can always sidestep them
//
// The last two entries are "cannot be forced" rather than "cannot happen"—a
// helpless opponent could still walk into mate. Scoring them as draws is a
// deliberate simplification we share with most engines: it costs nothing real
// and stops the search chasing a mate that correct defence always avoids.
//
// Anything with a pawn (it can promote), a rook or a queen is winnable, so those
// positions are rejected up front.
// =============================================================================

bool has_insufficient_material(const Board& board) noexcept {
  const int winning_material = count_of(board, Piece::WP) + count_of(board, Piece::BP) +
                               count_of(board, Piece::WR) + count_of(board, Piece::BR) +
                               count_of(board, Piece::WQ) + count_of(board, Piece::BQ);
  if (winning_material > 0) {
    return false;
  }

  const int white_knights = count_of(board, Piece::WN);
  const int black_knights = count_of(board, Piece::BN);
  const int white_bishops = count_of(board, Piece::WB);
  const int black_bishops = count_of(board, Piece::BB);

  const int white_minors = white_knights + white_bishops;
  const int black_minors = black_knights + black_bishops;

  // Bare kings, or a single minor piece between them.
  if (white_minors + black_minors <= 1) {
    return true;
  }

  // One minor each: bishop against bishop (whatever colours they run on),
  // bishop against knight or knight against knight are all drawn.
  if (white_minors == 1 && black_minors == 1) {
    return true;
  }

  // Two knights against a bare king.
  if (white_knights == 2 && white_bishops == 0 && black_minors == 0) {
    return true;
  }
  if (black_knights == 2 && black_bishops == 0 && white_minors == 0) {
    return true;
  }

  return false;
}

// Total material value for one side, priced in the requested phase's currency.
int eval_material(Colour colour, const Board& board, Phase phase) noexcept {
  const auto& values = phase == Phase::Middlegame ? PIECE_VALUES : PIECE_VALUES_ENDGAME;

  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    total += values[static_cast<std::size_t>(piece)] * count_of(board, piece);
  }

  if (has_bishop_pair(board, colour)) {
    total += phase == Phase::Middlegame ? BISHOP_PAIR_MIDDLEGAME : BISHOP_PAIR_ENDGAME;
  }

  return total;
}

// Sum PSQT bonuses for all pieces of one side.
int eval_psqt(Colour colour, const Board& board, Phase phase) noexcept {
  const auto& tables = PIECE_SQUARE_TABLES[phase_index(phase)];

  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    Bitboard piece_squares = board.pieces(piece);
    while (piece_squares != 0) {
      const Square square = Square::pop_first_occupied(piece_squares);
      total += tables[static_cast<std::size_t>(piece)][square.index()];
    }
  }
  return total;
}

namespace {

// The bishop-pair bonus, as a signed White-minus-Black figure. This is the one
// material term the accumulator cannot carry: it belongs to a side owning two
// bishops on opposite square colours, not to either bishop individually, so
// there is no per-piece value to add and subtract.
int bishop_pair_advantage(const Board& board, Phase phase) noexcept {
  const int bonus = phase == Phase::Middlegame ? BISHOP_PAIR_MIDDLEGAME : BISHOP_PAIR_ENDGAME;

  int advantage = 0;
  if (has_bishop_pair(board, Colour::White)) {
    advantage += bonus;
  }
  if (has_bishop_pair(board, Colour::Black)) {
    advantage -= bonus;
  }
  return advantage;
}

} // namespace

// =============================================================================
// MAIN EVALUATION FUNCTION
// =============================================================================
// Returns the position score from the perspective of the side to move:
//   Positive = good for side to move
//   Negative = bad for side to move
//   Zero = equal position
//
// This "side-to-move perspective" convention simplifies the search algorithm:
// the current player always wants to maximize the score, regardless of colour.
// Note that the taper and the clamp both happen on the White-relative score and
// only the final sign flip depends on whose turn it is. That keeps evaluation
// exactly antisymmetric: the score of a position is minus the score of the same
// position with the other side to move.
// =============================================================================

int eval(const Position& pos) noexcept {
  if (has_insufficient_material(pos.board)) {
    return CENTIPAWN_DRAW;
  }

  // Material, piece-square bonuses and the game phase all come straight out of
  // the running totals the Board maintains, so nothing here walks the pieces.
  const auto& accumulator = pos.board.accumulator();
  const int phase = std::min(accumulator.phase(), PHASE_MAX);

  const int middlegame = accumulator.middlegame(Colour::White) -
                         accumulator.middlegame(Colour::Black) +
                         bishop_pair_advantage(pos.board, Phase::Middlegame);
  const int endgame = accumulator.endgame(Colour::White) - accumulator.endgame(Colour::Black) +
                      bishop_pair_advantage(pos.board, Phase::Endgame);

  // The taper: full armies (phase 24) return the middlegame score, bare kings
  // (phase 0) return the endgame score, and everything in between is a weighted
  // average that shifts a little further towards the endgame with every trade.
  const int score = (middlegame * phase + endgame * (PHASE_MAX - phase)) / PHASE_MAX;

  // A static score must never be mistaken for a forced mate by the search.
  const int bounded = std::clamp(score, -CENTIPAWN_EVAL_MAX, CENTIPAWN_EVAL_MAX);

  // Flip sign if Black is to move (so positive always means "good for me")
  return pos.colour_to_move == Colour::White ? bounded : -bounded;
}

} // namespace c3
