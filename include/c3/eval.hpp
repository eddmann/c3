#pragma once

// =============================================================================
// POSITION EVALUATION: Estimating Who's Winning
// =============================================================================
//
// The evaluation function assigns a numerical score to a position: positive
// means white is better, negative means black is better, zero is equal.
//
// This is the "brain" of the chess engine—without search, the engine would
// just pick the move with the best immediate evaluation. Search allows it to
// look ahead and find moves that lead to good evaluations later.
//
// Our evaluation has four ingredients:
//
//   1. MATERIAL       how much wood each side owns (plus a bishop-pair bonus)
//   2. PIECE-SQUARE   a bonus/penalty for *where* each piece stands
//   3. GAME PHASE     how far the position has travelled from opening to endgame
//   4. DRAW KNOWLEDGE positions nobody can win are scored as dead equal
//
// Ingredients 1 and 2 are each computed TWICE—once with middlegame numbers and
// once with endgame numbers—and then blended according to ingredient 3. That
// blending is called a "tapered evaluation" and it is what lets one function
// give sensible advice in both a queen-filled middlegame and a bare-kings
// endgame. See the comments in src/eval.cpp for the full picture.
//
// More advanced engines add:
//   - Pawn structure (doubled, isolated, passed pawns)
//   - King safety (pawn shield, attacking pieces)
//   - Mobility (how many squares pieces control)
//   - Piece coordination
//
// But even simple evaluation + deep search = strong play. Search depth often
// compensates for evaluation simplicity.
//
// =============================================================================

#include "c3/board.hpp"
#include "c3/colour.hpp"
#include "c3/eval_terms.hpp"
#include "c3/position.hpp"

namespace c3 {

// =============================================================================
// EVALUATION SCALE: Centipawns
// =============================================================================
// All evaluations use "centipawns" (1/100th of a pawn). A pawn = 100 centipawns.
// This gives fine-grained precision without floating point math.
//
// Examples:
//   +100  = white is up one pawn
//   -330  = black has a bishop advantage
//   +9745 = white has mate in N moves (see mate score encoding below)
// =============================================================================

inline constexpr int CENTIPAWN_MAX = 10'000;
inline constexpr int CENTIPAWN_MIN = -CENTIPAWN_MAX;
inline constexpr int CENTIPAWN_DRAW = 0;

// =============================================================================
// MATE SCORE ENCODING
// =============================================================================
// Checkmate is represented as CENTIPAWN_MATE (10000) minus the number of plies
// until mate. This ensures "mate in 3" beats "mate in 5"—the engine prefers
// faster mates.
//
//   Mate in 1 ply  = 10000 - 1 = 9999
//   Mate in 3 plies = 10000 - 3 = 9997
//   Getting mated in 2 = -(10000 - 2) = -9998
//
// CENTIPAWN_MATE_THRESHOLD (9745) distinguishes mate scores from huge material
// advantages. Any score above this threshold is a forced mate.
//
// Because the search treats "above the threshold" as "mate found", the static
// evaluation must never reach it on its own. A position with a dozen promoted
// queens really can add up to more than 9745 centipawns, so eval() clamps its
// result to ±CENTIPAWN_EVAL_MAX: a colossal material lead is still only a
// material lead, never a claim of forced mate.
// =============================================================================

inline constexpr int CENTIPAWN_MATE = CENTIPAWN_MAX;
inline constexpr int CENTIPAWN_MATE_THRESHOLD = CENTIPAWN_MATE - 255;
inline constexpr int CENTIPAWN_EVAL_MAX = CENTIPAWN_MATE_THRESHOLD - 1;

// The game phase scale (PHASE_MAX, PHASE_WEIGHTS), the Phase enum and the two
// tables of piece values (PIECE_VALUES, PIECE_VALUES_ENDGAME) live in
// eval_terms.hpp, included above, alongside the piece-square tables built from
// them. They moved there so that Board can keep a running total of them; see
// that header for the numbers themselves and for why the total is maintained
// rather than recomputed.

// =============================================================================
// BISHOP PAIR
// =============================================================================
// Two bishops cover both square colours, so nothing on the board is permanently
// safe from them. The pair is worth noticeably more than the sum of its parts,
// and more still in an open endgame where the bishops' range matters most.
// The bonus is awarded once, to a side holding at least two bishops.
// =============================================================================

inline constexpr int BISHOP_PAIR_MIDDLEGAME = 30;
inline constexpr int BISHOP_PAIR_ENDGAME = 50;

// Non-pawn material left on the board, clamped to [0, PHASE_MAX].
// PHASE_MAX = full middlegame armies, 0 = bare kings.
[[nodiscard]] int game_phase(const Board& board) noexcept;

// True when neither side owns enough material to force checkmate, so the game
// is drawn no matter how well either player plays. See src/eval.cpp for the
// exact list of material combinations we recognise.
[[nodiscard]] bool has_insufficient_material(const Board& board) noexcept;

// Component scores for one colour, always positive-is-good-for-that-colour.
// Both take the phase explicitly so tests (and curious readers) can inspect
// either end of the taper.
//
// eval() no longer calls either of these: it reads the running totals the Board
// keeps instead. They remain as the from-scratch REFERENCE implementation—the
// slow, obviously-correct version that the tests and the Debug assertions in
// position.cpp measure the running totals against. Keeping a readable reference
// beside an optimised implementation is what makes the optimisation safe to
// trust; delete it and the only check left on the accumulator is that it agrees
// with itself.
[[nodiscard]] int eval_material(Colour colour, const Board& board, Phase phase) noexcept;
[[nodiscard]] int eval_psqt(Colour colour, const Board& board, Phase phase) noexcept;

// The whole evaluation, from the perspective of the side to move.
[[nodiscard]] int eval(const Position& pos) noexcept;

} // namespace c3
