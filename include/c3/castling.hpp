#pragma once

#include <cstdint>
#include <initializer_list>
#include <stdexcept>

#include "c3/colour.hpp"
#include "c3/square.hpp"

namespace c3 {

enum class CastlingRight : std::uint8_t {
  WhiteKing = 1,
  WhiteQueen = 2,
  BlackKing = 4,
  BlackQueen = 8,
};

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

  void remove_for_square(Square square) {
    switch (square.index()) {
    case 0: // a1
      remove(CastlingRight::WhiteQueen);
      break;
    case 7: // h1
      remove(CastlingRight::WhiteKing);
      break;
    case 56: // a8
      remove(CastlingRight::BlackQueen);
      break;
    case 63: // h8
      remove(CastlingRight::BlackKing);
      break;
    default:
      throw std::invalid_argument("cannot remove castling rights for square");
    }
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
