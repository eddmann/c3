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

#include <array>
#include <cstddef>
#include <cstdint>

#include "c3/board.hpp"
#include "c3/colour.hpp"
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

// =============================================================================
// GAME PHASE
// =============================================================================
// "Phase" answers the question: is this still a middlegame, or an endgame?
//
// We answer it by counting the non-pawn material left on the board, using the
// classic 24-point scale (knight 1, bishop 1, rook 2, queen 4, both colours
// summed). A full opening army is 4 minors + 4 rooks + 2 queens = 24 points; a
// bare-kings endgame is 0. Pawns are deliberately ignored: trading every piece
// but keeping all the pawns still gives you a king-and-pawn ENDGAME.
//
// The count is capped at PHASE_MAX because promotions can create more material
// than the game started with.
// =============================================================================

inline constexpr int PHASE_MAX = 24;
inline constexpr std::array<int, 6> PHASE_WEIGHTS = {
    0, // Pawn: pawns do not decide whether we are in an endgame
    1, // Knight
    1, // Bishop
    2, // Rook
    4, // Queen
    0, // King: always present, so it carries no information
};

// The two sets of numbers a tapered evaluation blends between.
enum class Phase : std::size_t { Middlegame, Endgame };

// =============================================================================
// MATERIAL VALUES
// =============================================================================
// Traditional piece values, in the "simplified evaluation function" flavour
// (Tomasz Michniewski, Chess Programming Wiki):
//   Pawn   = 100 (the unit of measurement)
//   Knight = 320 (≈3 pawns, good in closed positions)
//   Bishop = 330 (a shade above the knight, so the engine keeps the pair)
//   Rook   = 500 (≈5 pawns, dominates open files)
//   Queen  = 900 (≈9 pawns, strongest piece)
//   King   = 0   (priceless, but doesn't contribute to material count)
//
// PIECE_VALUES holds the middlegame numbers. The search also uses this array
// for MVV-LVA capture ordering ("most valuable victim, least valuable
// attacker"), which is why it keeps its 12-entry shape.
//
// PIECE_VALUES_ENDGAME nudges the numbers for a bare board: a pawn is worth
// more once it has a realistic chance of queening, and the long-range pieces
// gain a little as the board opens up. Minor pieces stay put—they are the
// yardstick everything else is measured against.
// =============================================================================

inline constexpr std::array<int, 12> PIECE_VALUES = {
    100, 320, 330, 500, 900, 0, // White: P, N, B, R, Q, K
    100, 320, 330, 500, 900, 0  // Black: P, N, B, R, Q, K
};

inline constexpr std::array<int, 12> PIECE_VALUES_ENDGAME = {
    120, 320, 330, 520, 940, 0, // White: P, N, B, R, Q, K
    120, 320, 330, 520, 940, 0  // Black: P, N, B, R, Q, K
};

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
[[nodiscard]] int eval_material(Colour colour, const Board& board, Phase phase) noexcept;
[[nodiscard]] int eval_psqt(Colour colour, const Board& board, Phase phase) noexcept;

// The whole evaluation, from the perspective of the side to move.
[[nodiscard]] int eval(const Position& pos) noexcept;

} // namespace c3
