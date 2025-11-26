#pragma once

#include <cstdint>
#include <optional>

#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {

struct Move {
  Piece piece{};
  Square from{};
  Square to{};
  std::optional<Piece> captured_piece{};
  std::optional<Piece> promotion_piece{};
  bool is_en_passant{false};

  [[nodiscard]] std::optional<Square> capture_square() const noexcept {
    if (!captured_piece.has_value()) {
      return std::nullopt;
    }

    if (is_en_passant) {
      return to.advance(colour(*captured_piece));
    }

    return to;
  }

  [[nodiscard]] bool is_castling() const noexcept { return is_king(piece) && file_diff() > 1; }

  [[nodiscard]] std::uint8_t file_diff() const noexcept { return from.file_diff(to); }
  [[nodiscard]] std::uint8_t rank_diff() const noexcept { return from.rank_diff(to); }

  friend constexpr bool operator==(const Move& lhs, const Move& rhs) noexcept {
    return lhs.piece == rhs.piece && lhs.from == rhs.from && lhs.to == rhs.to &&
           lhs.promotion_piece == rhs.promotion_piece && lhs.is_en_passant == rhs.is_en_passant;
  }

  friend constexpr bool operator!=(const Move& lhs, const Move& rhs) noexcept {
    return !(lhs == rhs);
  }
};

} // namespace c3
