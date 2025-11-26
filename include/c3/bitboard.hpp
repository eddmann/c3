#pragma once

// =============================================================================
// BITBOARDS: The Foundation of Modern Chess Engines
// =============================================================================
//
// A bitboard is a 64-bit integer where each bit represents one square on the
// chessboard. This is the key insight that makes modern chess engines fast:
//
//   - Bit 0 = square A1, Bit 7 = square H1, Bit 63 = square H8
//   - A "1" bit means something is present (a piece, an attack, etc.)
//   - A "0" bit means nothing is present
//
// Why bitboards? They enable "parallel" operations on all 64 squares at once
// using CPU bitwise instructions. For example:
//
//   white_pieces & enemy_attacks  → all white pieces under attack (one AND op)
//   piece_moves & ~own_pieces     → legal destination squares (AND + NOT)
//   std::popcount(pawns)          → count all pawns instantly (CPU intrinsic)
//
// Without bitboards, you'd loop through 64 squares checking each one. With
// bitboards, a single CPU instruction processes all 64 squares simultaneously.
//
// The trade-off: bitboards require careful bit manipulation, which can be
// error-prone. But the performance gain is substantial—modern engines would
// be orders of magnitude slower without them.
//
// =============================================================================

#include <cstdint>

namespace c3 {

// A 64-bit integer representing 64 squares. Each bit position maps to a square:
// bit 0 = A1, bit 1 = B1, ..., bit 7 = H1, bit 8 = A2, ..., bit 63 = H8.
using Bitboard = std::uint64_t;

// Precomputed masks for common board regions. Using precomputed constants avoids
// recalculating these patterns every time they're needed.

// Ranks 1 and 8: where pawns promote and kings start. Used for promotion detection.
inline constexpr Bitboard BACK_RANKS = 0xFF00'0000'0000'00FFull;

// The four corner squares (A1, H1, A8, H8): where rooks start for castling.
inline constexpr Bitboard CORNERS = 0x8100'0000'0000'0081ull;

// File masks: vertical columns A through H.
//
// These are essential for preventing "wrap-around" bugs in move generation.
// When shifting bits left/right to calculate moves, pieces on edge files could
// incorrectly "wrap" to the opposite side of the board. File masks let us
// exclude edge squares before shifting.
//
// Example: A knight on A1 cannot move left, so we mask out file A before
// calculating leftward knight moves: (bb & ~FILE_A) >> 17
inline constexpr Bitboard FILE_MASKS[8] = {
    0x0101'0101'0101'0101ull, // File A
    0x0202'0202'0202'0202ull, // File B
    0x0404'0404'0404'0404ull, // File C
    0x0808'0808'0808'0808ull, // File D
    0x1010'1010'1010'1010ull, // File E
    0x2020'2020'2020'2020ull, // File F
    0x4040'4040'4040'4040ull, // File G
    0x8080'8080'8080'8080ull, // File H
};

inline constexpr Bitboard FILE_A = FILE_MASKS[0];
inline constexpr Bitboard FILE_B = FILE_MASKS[1];
inline constexpr Bitboard FILE_C = FILE_MASKS[2];
inline constexpr Bitboard FILE_D = FILE_MASKS[3];
inline constexpr Bitboard FILE_E = FILE_MASKS[4];
inline constexpr Bitboard FILE_F = FILE_MASKS[5];
inline constexpr Bitboard FILE_G = FILE_MASKS[6];
inline constexpr Bitboard FILE_H = FILE_MASKS[7];

} // namespace c3
