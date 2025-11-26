#pragma once

#include <cstdint>

namespace c3 {

enum class Colour : std::uint8_t { White = 0, Black = 1 };

constexpr Colour operator!(Colour colour) {
  return colour == Colour::White ? Colour::Black : Colour::White;
}

} // namespace c3
