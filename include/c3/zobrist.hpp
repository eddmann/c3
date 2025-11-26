#pragma once

// =============================================================================
// ZOBRIST HASHING: Fingerprinting Chess Positions
// =============================================================================
//
// Problem: Chess search revisits the same position millions of times through
// different move orders. We need a way to recognize "we've seen this before"
// without comparing every piece on every square.
//
// Solution: Zobrist hashing assigns a random 64-bit number to each (piece, square)
// combination, plus values for side-to-move, castling rights, and en passant.
// The position's hash is computed by XORing together all relevant values.
//
// Why XOR is perfect for this:
//
//   1. Self-inverse: A ^ A = 0. To update the hash when moving a piece:
//      - XOR out the old (piece, from_square) → removes it from hash
//      - XOR in the new (piece, to_square)   → adds it to hash
//      This is O(1) per move, not O(64) to recompute the whole position.
//
//   2. Order-independent: A ^ B ^ C = C ^ A ^ B. The same position reached
//      via different move orders produces the same hash.
//
//   3. Low collision probability: With 64-bit random values, the chance of
//      two different positions hashing to the same value is ~1/2^64.
//
// The hash is used as a key in the transposition table (see search.hpp) to
// cache and retrieve previously computed position evaluations.
//
// =============================================================================

#include <array>
#include <cstdint>

#include "c3/piece.hpp"
#include "c3/rng.hpp"
#include "c3/square.hpp"

namespace c3 {

// The Zobrist table contains all random values used to compute position hashes.
// Each component of the position that affects game state has its own random values.
struct ZobristTable {
  // 12 piece types × 64 squares = 768 random values.
  // Hash contribution: XOR piece_square[piece][square] for each piece on board.
  std::array<std::array<std::uint64_t, 64>, 12> piece_square{};

  // XORed into hash when black is to move. White-to-move positions don't include this.
  // This ensures the same board with different sides to move produces different hashes.
  std::uint64_t colour_to_move{};

  // 16 values for all castling right combinations (2^4 = 16).
  // Indexed by the 4-bit castling rights value (K=1, Q=2, k=4, q=8).
  std::array<std::uint64_t, 16> castling_rights{};

  // 8 values, one per file, for en passant.
  // Only XORed in when en passant is actually possible (a pawn can capture).
  // This detail matters: two positions that differ only in "phantom" en passant
  // squares (where no pawn can capture) should hash identically.
  std::array<std::uint64_t, 8> en_passant_files{};
};

// Generate the Zobrist table at compile time using consteval.
// This means the random values are computed once by the compiler and embedded
// in the binary—no runtime initialization cost, and guaranteed consistency.
consteval ZobristTable make_zobrist_table() {
  ZobristTable table{};
  HashRng rng(HASH_SEED); // Deterministic: same seed → same table always

  for (const auto piece : all_pieces()) {
    for (std::uint8_t file = 0; file < 8; ++file) {
      for (std::uint8_t rank = 0; rank < 8; ++rank) {
        const auto square = Square::from_file_and_rank(file, rank);
        table.piece_square[static_cast<std::size_t>(piece)][square.index()] = rng.next();
      }
    }
  }

  table.colour_to_move = rng.next();

  for (auto& entry : table.castling_rights) {
    entry = rng.next();
  }

  for (auto& entry : table.en_passant_files) {
    entry = rng.next();
  }

  return table;
}

// Global Zobrist table, computed at compile time.
// All position hashing throughout the engine references this single table.
inline constexpr ZobristTable ZOBRIST = make_zobrist_table();

} // namespace c3
