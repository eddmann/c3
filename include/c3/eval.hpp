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
// Our evaluation is simple but effective:
//   score = material_balance + piece_square_bonuses
//
// More advanced engines add:
//   - Pawn structure (doubled, isolated, passed pawns)
//   - King safety (castling, pawn shield, attacking pieces)
//   - Mobility (how many squares pieces control)
//   - Piece coordination
//   - Endgame-specific knowledge
//
// But even simple evaluation + deep search = strong play. Search depth often
// compensates for evaluation simplicity.
//
// =============================================================================

#include <array>
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
//   -350  = black has a bishop advantage
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
// =============================================================================

inline constexpr int CENTIPAWN_MATE = CENTIPAWN_MAX;
inline constexpr int CENTIPAWN_MATE_THRESHOLD = CENTIPAWN_MATE - 255;

// =============================================================================
// MATERIAL VALUES
// =============================================================================
// Traditional piece values based on centuries of chess wisdom:
//   Pawn   = 100 (the unit of measurement)
//   Knight = 300 (≈3 pawns, good in closed positions)
//   Bishop = 350 (≈3.5 pawns, the "bishop pair" is strong)
//   Rook   = 500 (≈5 pawns, dominates open files)
//   Queen  = 900 (≈9 pawns, strongest piece)
//   King   = 0   (priceless, but doesn't contribute to material count)
// =============================================================================

inline constexpr std::array<int, 12> PIECE_VALUES = {
    100, 300, 350, 500, 900, 0, // White: P, N, B, R, Q, K
    100, 300, 350, 500, 900, 0  // Black: P, N, B, R, Q, K
};

[[nodiscard]] int eval_material(Colour colour, const Board& board) noexcept;
[[nodiscard]] int eval_psqt(Colour colour, const Board& board) noexcept;
[[nodiscard]] int eval(const Position& pos) noexcept;

} // namespace c3
