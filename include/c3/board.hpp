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
// recomputed on demand because every sliding-piece attack lookup (magic
// bitboards) needs it, for every rook, bishop and queen, on every node of the
// search. The OR it replaces is cheap, but not doing it at all is cheaper, and
// keeping it current costs one extra bitwise op inside put_piece/remove_piece.
//
// Why not just one representation?
//   - Mailbox alone: Finding all knights requires scanning 64 squares
//   - Bitboards alone: Finding what's on E4 requires checking 12 bitboards
//   - Hybrid: Both operations are O(1), for one extra copy of the position
//
// The Board also carries an EvalAccumulator: the running material, piece-square
// and game-phase totals that the evaluation reads instead of walking the piece
// lists at every node. It lives here rather than in Position because
// put_piece/remove_piece below are the ONLY way a piece ever appears on or
// leaves a square—FEN parsing, make_move, unmake_move and tests all funnel
// through them—so keeping the totals current there makes it impossible for them
// to drift out of sync. See eval_terms.hpp for what the totals contain and why.
//
// A Board is 272 bytes: 128 for the mailbox (std::optional<Piece> is 2 bytes
// now that Piece is a single byte—it used to be 8, which alone made a Board
// 624 bytes), 96 for the per-piece bitboards, 16 for the colour bitboards, 8
// for the occupancy and 20 for the accumulator, rounded up to a multiple of 8.
// Small enough that copying one is cheap and several fit in L1 alongside
// everything else the search is touching.
//
// =============================================================================

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <optional>

#include "c3/bitboard.hpp"
#include "c3/colour.hpp"
#include "c3/eval_terms.hpp"
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
  // unrelated evaluation or attack lookup, so Debug builds abort here instead.
  // Release builds do not check: the assert is the whole enforcement, so callers
  // that mean to replace a piece must remove_piece first either way.
  void put_piece(Piece piece, Square square) noexcept {
    assert(!has_piece_at(square) && "put_piece expects an empty square");

    const auto idx = piece_index(piece);
    const auto colour_idx = colour_index(colour(piece));

    squares_[square.index()] = piece;
    pieces_[idx] |= square;
    colours_[colour_idx] |= square;
    occupancy_ |= square;
    accumulator_.add(piece, square);
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
    accumulator_.remove(*maybe_piece, square);
  }

  [[nodiscard]] Bitboard occupancy() const noexcept { return occupancy_; }
  [[nodiscard]] bool has_occupancy_at(Bitboard squares) const noexcept {
    return (occupancy() & squares) != 0;
  }

  // The running evaluation totals, maintained by put_piece/remove_piece.
  [[nodiscard]] const EvalAccumulator& accumulator() const noexcept { return accumulator_; }

  // The same totals, rebuilt from the pieces actually on the board. This is the
  // slow, obviously-correct version: Debug builds compare the two after every
  // make_move and unmake_move, exactly as they compare the incremental Zobrist
  // key against compute_key(). An accumulator that silently drifts would poison
  // every evaluation from that node onwards, and the resulting bad move is
  // impossible to trace back to its cause—so we catch the drift instead.
  [[nodiscard]] EvalAccumulator compute_accumulator() const noexcept {
    EvalAccumulator rebuilt;
    for (std::uint8_t index = 0; index < 64; ++index) {
      const Square square = Square::from_index(index);
      if (const auto piece = piece_at(square)) {
        rebuilt.add(*piece, square);
      }
    }
    return rebuilt;
  }

private:
  static constexpr std::size_t piece_index(Piece piece) noexcept {
    return static_cast<std::size_t>(piece);
  }

  static constexpr std::size_t colour_index(Colour colour) noexcept {
    return static_cast<std::size_t>(colour);
  }

  // The four representations below are kept in sync by put_piece/remove_piece,
  // and so is the accumulator that rides along with them. This redundancy is
  // intentional: different queries are fast with different representations, and
  // the synchronization cost is minimal.

  std::array<std::optional<Piece>, 64> squares_{}; // Mailbox: square → piece
  std::array<Bitboard, 12> pieces_{};              // Bitboard per piece type
  std::array<Bitboard, 2> colours_{};              // Bitboard per colour
  Bitboard occupancy_{};                           // Union of both colour bitboards
  EvalAccumulator accumulator_{};                  // Running material/PSQT/phase totals
};

} // namespace c3
