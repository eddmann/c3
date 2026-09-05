#pragma once

// =============================================================================
// PAWN GEOMETRY: The Masks Behind Pawn-Structure Evaluation
// =============================================================================
//
// "Is this pawn passed?", "is it isolated?", "is it doubled?" are all questions
// about which OTHER pawns sit on a handful of squares relative to this one. A
// bitboard engine answers them by intersecting the enemy (or friendly) pawn
// bitboard with a precomputed mask and asking whether the result is empty—one
// AND and one compare, instead of walking files and ranks square by square.
//
// The three masks below are the vocabulary the evaluation speaks:
//
//   ADJACENT_FILE_MASKS   the file(s) either side of a given file
//   FORWARD_FILE_MASKS    the squares straight ahead of a pawn, its own file
//   PASSED_PAWN_MASKS     the squares ahead of a pawn on its own AND both
//                         neighbouring files—the "no enemy pawn may stand here"
//                         zone that defines a passed pawn
//
// All three are built at compile time, so the run-time cost of a pawn-structure
// term is the AND and nothing else.
//
// WHY "AHEAD" NEEDS A COLOUR. Pawns are the only pieces that cannot move
// backwards, so every one of these questions is asked from the mover's point of
// view: ahead means towards rank 8 for White and towards rank 1 for Black. Each
// forward-looking table therefore has two halves, one per colour, and
// relative_rank() below turns a square into "how far has this pawn travelled",
// counting from its own back rank rather than from White's.
//
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>

#include "c3/bitboard.hpp"
#include "c3/colour.hpp"
#include "c3/square.hpp"

namespace c3 {

// Horizontal counterparts to FILE_MASKS in bitboard.hpp: RANK_MASKS[0] is
// rank 1 (where White's pieces start) and RANK_MASKS[7] is rank 8.
inline constexpr std::array<Bitboard, 8> RANK_MASKS = {
    0x0000'0000'0000'00FFULL, 0x0000'0000'0000'FF00ULL, 0x0000'0000'00FF'0000ULL,
    0x0000'0000'FF00'0000ULL, 0x0000'00FF'0000'0000ULL, 0x0000'FF00'0000'0000ULL,
    0x00FF'0000'0000'0000ULL, 0xFF00'0000'0000'0000ULL,
};

// How far a pawn of this colour has advanced, counted from its own home side:
// 0 is the back rank, 1 the starting rank, 6 the rank before promotion. Reading
// ranks this way lets one table of bonuses serve both colours.
[[nodiscard]] constexpr std::uint8_t relative_rank(Square square, Colour side) noexcept {
  return side == Colour::White ? square.rank() : static_cast<std::uint8_t>(7 - square.rank());
}

namespace detail {

constexpr std::array<Bitboard, 8> build_adjacent_file_masks() {
  std::array<Bitboard, 8> masks{};
  for (std::size_t file = 0; file < masks.size(); ++file) {
    if (file > 0) {
      masks[file] |= FILE_MASKS[file - 1];
    }
    if (file < 7) {
      masks[file] |= FILE_MASKS[file + 1];
    }
  }
  return masks;
}

// Every square strictly ahead of this one on the same file, from the given
// side's point of view.
constexpr std::array<std::array<Bitboard, 64>, 2> build_forward_file_masks() {
  std::array<std::array<Bitboard, 64>, 2> masks{};

  for (std::size_t index = 0; index < 64; ++index) {
    const auto square = Square::from_index(static_cast<std::uint8_t>(index));
    const Bitboard file = FILE_MASKS[square.file()];

    // Everything above this square, and everything below it: shifting a full
    // board of ones past the square is the cheapest way to say "strictly after"
    // and "strictly before" in bit order, and bit order runs A1 to H8. The two
    // guards keep us away from a shift of 64, which C++ leaves undefined.
    const Bitboard above = index == 63 ? Bitboard{0} : ~Bitboard{0} << (index + 1);
    const Bitboard below = index == 0 ? Bitboard{0} : ~Bitboard{0} >> (64 - index);

    masks[static_cast<std::size_t>(Colour::White)][index] = file & above;
    masks[static_cast<std::size_t>(Colour::Black)][index] = file & below;
  }

  return masks;
}

// The passed-pawn zone: the squares ahead of the pawn on its own file (where an
// enemy pawn would BLOCK it) plus the squares ahead on both neighbouring files
// (where an enemy pawn could CAPTURE it on the way, or be captured by it). If
// none of those squares holds an enemy pawn, nothing but pieces can stop the
// pawn from queening—which is what makes a passed pawn so valuable, and why the
// bonus grows so steeply with the rank it has reached.
constexpr std::array<std::array<Bitboard, 64>, 2> build_passed_pawn_masks() {
  const auto forward = build_forward_file_masks();
  const auto adjacent = build_adjacent_file_masks();

  std::array<std::array<Bitboard, 64>, 2> masks{};

  for (std::size_t side = 0; side < masks.size(); ++side) {
    for (std::size_t index = 0; index < 64; ++index) {
      const auto square = Square::from_index(static_cast<std::uint8_t>(index));
      const Bitboard ahead_on_file = forward[side][index];

      // "Ahead" on the neighbouring files means the same ranks, so widen the
      // own-file mask sideways rather than recomputing it.
      Bitboard ahead_on_neighbours = 0;
      for (std::size_t rank = 0; rank < 8; ++rank) {
        if ((ahead_on_file & RANK_MASKS[rank]) != 0) {
          ahead_on_neighbours |= RANK_MASKS[rank] & adjacent[square.file()];
        }
      }

      masks[side][index] = ahead_on_file | ahead_on_neighbours;
    }
  }

  return masks;
}

} // namespace detail

// Indexed by file: the neighbouring file(s). A pawn with no friendly pawn here
// is ISOLATED—no fellow pawn can ever defend it, so it has to be defended by
// pieces, which ties them down for the rest of the game.
inline constexpr auto ADJACENT_FILE_MASKS = detail::build_adjacent_file_masks();

// Indexed by [colour][square]: the squares ahead on the same file. A friendly
// pawn found here means this pawn is DOUBLED—two pawns on one file cannot
// defend each other and only cover the squares one of them already covered.
inline constexpr auto FORWARD_FILE_MASKS = detail::build_forward_file_masks();

// Indexed by [colour][square]: no enemy pawn in here means this pawn is PASSED.
inline constexpr auto PASSED_PAWN_MASKS = detail::build_passed_pawn_masks();

// Every square attacked by a whole set of pawns at once, computed with two
// shifts instead of a loop. The file masks stop a pawn on the a-file from
// "capturing" onto the h-file, which is what a naive shift would do on a board
// stored as one 64-bit word.
[[nodiscard]] constexpr Bitboard pawn_attack_span(Bitboard pawns, Colour side) noexcept {
  if (side == Colour::White) {
    return ((pawns & ~FILE_A) << 7) | ((pawns & ~FILE_H) << 9);
  }
  return ((pawns & ~FILE_H) >> 7) | ((pawns & ~FILE_A) >> 9);
}

} // namespace c3
