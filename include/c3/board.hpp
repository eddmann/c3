#pragma once

// =============================================================================
// BOARD REPRESENTATION: Hybrid Mailbox + Bitboard
// =============================================================================
//
// Chess engines need to answer two types of questions efficiently:
//
//   1. "What piece is on square E4?" → Mailbox is O(1), bitboards are O(n)
//   2. "Where are all the white knights?" → Bitboards are O(1), mailbox is O(n)
//
// This engine uses BOTH representations simultaneously:
//
//   - squares_[64]: Array mapping each square to its piece (mailbox)
//   - pieces_[12]:  Bitboard for each piece type (WP, WN, WB, WR, WQ, WK, BP...)
//   - colours_[2]:  Bitboard for all white pieces, all black pieces
//   - occupancy_:   Bitboard for every occupied square, of either colour
//
// The mailbox array answers "what's on this square?" instantly. The bitboards
// answer "where are all pieces of type X?" instantly. We maintain both in sync.
//
// occupancy_ is the union of the two colour bitboards. It is stored rather than
// recomputed because sliding-piece attack lookups (magic bitboards) need it for
// every rook, bishop and queen on every node of the search: an OR is cheap, but
// not paying for it at all is cheaper, and put_piece/remove_piece already touch
// the same cache line.
//
// Why not just one representation?
//   - Mailbox alone: Finding all knights requires scanning 64 squares
//   - Bitboards alone: Finding what's on E4 requires checking 12 bitboards
//   - Hybrid: Both operations are O(1) at the cost of ~200 bytes extra memory
//
// The memory cost is negligible; the speed gain is significant.
//
// =============================================================================

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <optional>

#include "c3/bitboard.hpp"
#include "c3/colour.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {
class Board {
public:
  [[nodiscard]] static constexpr Board empty() noexcept { return Board{}; }

  [[nodiscard]] Bitboard pieces(Piece piece) const noexcept { return pieces_[piece_index(piece)]; }
  [[nodiscard]] Bitboard pieces_by_colour(Colour colour) const noexcept {
    return colours_[colour_index(colour)];
  }

  // Count pieces using std::popcount (population count / Hamming weight).
  // This maps to a single CPU instruction (POPCNT) on modern processors,
  // counting all set bits in one operation. Without this, you'd need a loop.
  [[nodiscard]] std::uint32_t count_pieces(Piece piece) const noexcept {
    return static_cast<std::uint32_t>(std::popcount(pieces(piece)));
  }

  // Precondition: the square is empty. Dropping a piece onto an occupant would
  // overwrite the mailbox entry while leaving the displaced piece's bit set in
  // its own bitboard—a "phantom" piece that the mailbox denies and the bitboards
  // insist on. Such a desync typically only surfaces plies later, in an
  // unrelated evaluation or attack lookup, so sanitizer builds stop here
  // instead. Callers that mean to replace a piece must remove_piece first.
  void put_piece(Piece piece, Square square) noexcept {
    assert(!has_piece_at(square) && "put_piece expects an empty square");

    const auto idx = piece_index(piece);
    const auto colour_idx = colour_index(colour(piece));

    squares_[square.index()] = piece;
    pieces_[idx] |= square;
    colours_[colour_idx] |= square;
    occupancy_ |= square;
  }

  [[nodiscard]] std::optional<Piece> piece_at(Square square) const noexcept {
    return squares_[square.index()];
  }
  [[nodiscard]] bool has_piece_at(Square square) const noexcept {
    return piece_at(square).has_value();
  }

  // Precondition: the square is occupied. Clearing an empty square is always a
  // caller error—unmaking a move that was never made, or removing the same
  // capture twice—so Debug builds abort rather than let the mistake pass as a
  // silent no-op. Release builds still no-op instead of corrupting the bitboards.
  void remove_piece(Square square) noexcept {
    const auto maybe_piece = piece_at(square);
    assert(maybe_piece.has_value() && "remove_piece expects an occupied square");
    if (!maybe_piece.has_value()) {
      return;
    }

    const auto idx = piece_index(*maybe_piece);
    const auto colour_idx = colour_index(colour(*maybe_piece));

    squares_[square.index()] = std::nullopt;
    pieces_[idx] &= ~Bitboard(square);
    colours_[colour_idx] &= ~Bitboard(square);
    occupancy_ &= ~Bitboard(square);
  }

  [[nodiscard]] Bitboard occupancy() const noexcept { return occupancy_; }
  [[nodiscard]] bool has_occupancy_at(Bitboard squares) const noexcept {
    return (occupancy() & squares) != 0;
  }

private:
  static constexpr std::size_t piece_index(Piece piece) noexcept {
    return static_cast<std::size_t>(piece);
  }

  static constexpr std::size_t colour_index(Colour colour) noexcept {
    return static_cast<std::size_t>(colour);
  }

  // The four representations below are kept in sync by put_piece/remove_piece.
  // This redundancy is intentional: different queries are fast with different
  // representations, and the synchronization cost is minimal.

  std::array<std::optional<Piece>, 64> squares_{}; // Mailbox: square → piece
  std::array<Bitboard, 12> pieces_{};              // Bitboard per piece type
  std::array<Bitboard, 2> colours_{};              // Bitboard per colour
  Bitboard occupancy_{};                           // Union of both colour bitboards
};

} // namespace c3
