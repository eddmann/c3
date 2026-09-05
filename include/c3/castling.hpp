#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

#include "c3/colour.hpp"
#include "c3/square.hpp"

namespace c3 {

enum class CastlingRight : std::uint8_t {
  WhiteKing = 1,
  WhiteQueen = 2,
  BlackKing = 4,
  BlackQueen = 8,
};

// =============================================================================
// CASTLING RIGHTS MASK: losing rights without branching
// =============================================================================
//
// Only six squares can ever cost a side its castling rights:
//
//   e1 / e8   the king's home square — once the king leaves, both of its
//             rights are gone forever
//   a1 / h1   the white rooks' home squares
//   a8 / h8   the black rooks' home squares
//
// A right dies whether the piece moved away from the square or was captured on
// it, so the same rule applies to a move's from-square and to-square alike.
// (Capturing a rook on h8 ends black's king-side right just as surely as moving
// that rook does.)
//
// Rather than asking "is this a corner? which one?" on every move, we precompute
// for each of the 64 squares the rights that SURVIVE a move touching it, then:
//
//   rights &= CASTLING_RIGHTS_MASK[move.from];
//   rights &= CASTLING_RIGHTS_MASK[move.to];
//
// The other 58 squares hold 0b1111 and therefore change nothing, so the update
// is two unconditional ANDs with no special cases to forget. This is the
// standard technique in production engines.
//
// Masking the to-square against e1/e8 looks too eager—surely a rook landing on
// e1 should not end white's rights?—but it cannot do harm. While a right is
// still held the king is standing on that square, so nothing else can move onto
// it; and once the king has left, the right is already gone. Position::from_fen
// enforces that invariant, rejecting castling rights that no king and rook back
// up. A Position built directly, field by field, can still violate it—but the
// mask only ever DROPS rights, never grants them, so the worst such a position
// can suffer is losing a right it had no business claiming.
inline constexpr std::array<std::uint8_t, 64> CASTLING_RIGHTS_MASK = [] {
  constexpr std::uint8_t ALL = 0b1111;
  std::array<std::uint8_t, 64> masks{};
  masks.fill(ALL);

  const auto clear = [&masks](Square square, CastlingRight right) {
    masks[square.index()] =
        static_cast<std::uint8_t>(masks[square.index()] & ~static_cast<std::uint8_t>(right));
  };

  clear(Square::A1, CastlingRight::WhiteQueen);
  clear(Square::H1, CastlingRight::WhiteKing);
  clear(Square::E1, CastlingRight::WhiteQueen);
  clear(Square::E1, CastlingRight::WhiteKing);

  clear(Square::A8, CastlingRight::BlackQueen);
  clear(Square::H8, CastlingRight::BlackKing);
  clear(Square::E8, CastlingRight::BlackQueen);
  clear(Square::E8, CastlingRight::BlackKing);

  return masks;
}();

class CastlingRights {
public:
  constexpr CastlingRights() = default;

  static constexpr CastlingRights none() { return CastlingRights(0); }
  static constexpr CastlingRights all() { return CastlingRights(0b1111); }

  static constexpr CastlingRights from(std::initializer_list<CastlingRight> rights) {
    auto result = CastlingRights::none();
    for (auto right : rights) {
      result.add(right);
    }
    return result;
  }

  constexpr bool has(CastlingRight right) const {
    return (mask_ & static_cast<std::uint8_t>(right)) != 0;
  }

  constexpr void add(CastlingRight right) { mask_ |= static_cast<std::uint8_t>(right); }

  constexpr void remove(CastlingRight right) { mask_ &= ~static_cast<std::uint8_t>(right); }

  constexpr void remove_for_colour(Colour colour) {
    switch (colour) {
    case Colour::White:
      remove(CastlingRight::WhiteKing);
      remove(CastlingRight::WhiteQueen);
      break;
    case Colour::Black:
      remove(CastlingRight::BlackKing);
      remove(CastlingRight::BlackQueen);
      break;
    }
  }

  // Drops whatever rights a move touching this square costs; a no-op for the
  // squares castling does not care about, so callers apply it unconditionally.
  constexpr void remove_for_square(Square square) {
    mask_ = static_cast<std::uint8_t>(mask_ & CASTLING_RIGHTS_MASK[square.index()]);
  }

  constexpr std::uint8_t value() const { return mask_; }
  constexpr explicit operator std::uint8_t() const { return mask_; }
  constexpr explicit operator std::size_t() const { return mask_; }

  friend constexpr bool operator==(CastlingRights lhs, CastlingRights rhs) = default;

  friend constexpr CastlingRights operator|(CastlingRights lhs, CastlingRight rhs) {
    lhs.add(rhs);
    return lhs;
  }

  friend constexpr CastlingRights& operator|=(CastlingRights& lhs, CastlingRight rhs) {
    lhs.add(rhs);
    return lhs;
  }

  friend constexpr bool operator&(CastlingRights lhs, CastlingRight rhs) { return lhs.has(rhs); }

private:
  explicit constexpr CastlingRights(std::uint8_t mask) : mask_{mask} {}

  std::uint8_t mask_{0};
};

} // namespace c3
