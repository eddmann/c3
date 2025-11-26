#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "c3/bitboard.hpp"
#include "c3/colour.hpp"

namespace c3 {

class Square {
public:
  // Named squares for convenience.
  static const Square A1;
  static const Square B1;
  static const Square C1;
  static const Square D1;
  static const Square E1;
  static const Square F1;
  static const Square G1;
  static const Square H1;

  static const Square A2;
  static const Square B2;
  static const Square C2;
  static const Square D2;
  static const Square E2;
  static const Square F2;
  static const Square G2;
  static const Square H2;

  static const Square A3;
  static const Square B3;
  static const Square C3;
  static const Square D3;
  static const Square E3;
  static const Square F3;
  static const Square G3;
  static const Square H3;

  static const Square A4;
  static const Square B4;
  static const Square C4;
  static const Square D4;
  static const Square E4;
  static const Square F4;
  static const Square G4;
  static const Square H4;

  static const Square A5;
  static const Square B5;
  static const Square C5;
  static const Square D5;
  static const Square E5;
  static const Square F5;
  static const Square G5;
  static const Square H5;

  static const Square A6;
  static const Square B6;
  static const Square C6;
  static const Square D6;
  static const Square E6;
  static const Square F6;
  static const Square G6;
  static const Square H6;

  static const Square A7;
  static const Square B7;
  static const Square C7;
  static const Square D7;
  static const Square E7;
  static const Square F7;
  static const Square G7;
  static const Square H7;

  static const Square A8;
  static const Square B8;
  static const Square C8;
  static const Square D8;
  static const Square E8;
  static const Square F8;
  static const Square G8;
  static const Square H8;

  constexpr Square() noexcept : index_(0) {}

  [[nodiscard]] static constexpr Square from_index(std::uint8_t index) noexcept {
    return Square(index);
  }

  [[nodiscard]] static constexpr Square from_file_and_rank(std::uint8_t file,
                                                           std::uint8_t rank) noexcept {
    return Square(static_cast<std::uint8_t>((rank << 3) | file));
  }

  /// Returns the square corresponding to the least significant set bit in the bitboard.
  /// Precondition: bitboard != 0.
  [[nodiscard]] static Square first_occupied(Bitboard bitboard) {
    return Square(static_cast<std::uint8_t>(std::countr_zero(bitboard)));
  }

  /// Returns the square corresponding to the most significant set bit in the bitboard.
  /// Precondition: bitboard != 0.
  [[nodiscard]] static Square last_occupied(Bitboard bitboard) {
    return Square(static_cast<std::uint8_t>(63 - std::countl_zero(bitboard)));
  }

  /// Removes and returns the square corresponding to the least significant set bit.
  /// Modifies the bitboard by clearing that bit.
  /// Precondition: bitboard != 0.
  [[nodiscard]] static Square pop_first_occupied(Bitboard& bitboard) {
    const Square square = first_occupied(bitboard);
    bitboard ^= square;
    return square;
  }

  [[nodiscard]] constexpr std::uint8_t index() const noexcept { return index_; }

  /// Returns a bitboard with only this square's bit set.
  [[nodiscard]] constexpr Bitboard to_bitboard() const noexcept { return Bitboard{1} << index_; }

  constexpr operator Bitboard() const noexcept { return Bitboard{1} << index_; }

  [[nodiscard]] constexpr std::uint8_t file() const noexcept {
    return static_cast<std::uint8_t>(index_ & 7u);
  }

  [[nodiscard]] constexpr std::uint8_t rank() const noexcept {
    return static_cast<std::uint8_t>(index_ >> 3);
  }

  [[nodiscard]] constexpr std::uint8_t file_diff(Square other) const noexcept {
    return static_cast<std::uint8_t>(file() > other.file() ? file() - other.file()
                                                           : other.file() - file());
  }

  [[nodiscard]] constexpr std::uint8_t rank_diff(Square other) const noexcept {
    return static_cast<std::uint8_t>(rank() > other.rank() ? rank() - other.rank()
                                                           : other.rank() - rank());
  }

  [[nodiscard]] constexpr Square advance(Colour colour) const noexcept {
    return colour == Colour::White ? Square(index_ + 8) : Square(index_ - 8);
  }

  [[nodiscard]] constexpr bool is_back_rank() const noexcept {
    return (Bitboard(*this) & BACK_RANKS) != 0;
  }

  [[nodiscard]] constexpr bool is_corner() const noexcept {
    return (Bitboard(*this) & CORNERS) != 0;
  }

  [[nodiscard]] std::string to_string() const {
    const char file_char = static_cast<char>('a' + file());
    const char rank_char = static_cast<char>('1' + rank());
    return std::string{file_char, rank_char};
  }

  [[nodiscard]] static std::optional<Square> parse(std::string_view algebraic) noexcept {
    if (algebraic.size() != 2) {
      return std::nullopt;
    }

    const char file_char = algebraic[0];
    const char rank_char = algebraic[1];

    if (file_char < 'a' || file_char > 'h' || rank_char < '1' || rank_char > '8') {
      return std::nullopt;
    }

    const std::uint8_t file = static_cast<std::uint8_t>(file_char - 'a');
    const std::uint8_t rank = static_cast<std::uint8_t>(rank_char - '1');
    return from_file_and_rank(file, rank);
  }

  friend constexpr bool operator==(Square lhs, Square rhs) noexcept {
    return lhs.index_ == rhs.index_;
  }
  friend constexpr bool operator!=(Square lhs, Square rhs) noexcept { return !(lhs == rhs); }

private:
  explicit constexpr Square(std::uint8_t index) noexcept : index_(index) {}

  std::uint8_t index_;
};

inline std::ostream& operator<<(std::ostream& os, Square square) {
  return os << square.to_string();
}

} // namespace c3

// Inline definitions of named squares (indices 0..63).
inline constexpr c3::Square c3::Square::A1{0};
inline constexpr c3::Square c3::Square::B1{1};
inline constexpr c3::Square c3::Square::C1{2};
inline constexpr c3::Square c3::Square::D1{3};
inline constexpr c3::Square c3::Square::E1{4};
inline constexpr c3::Square c3::Square::F1{5};
inline constexpr c3::Square c3::Square::G1{6};
inline constexpr c3::Square c3::Square::H1{7};

inline constexpr c3::Square c3::Square::A2{8};
inline constexpr c3::Square c3::Square::B2{9};
inline constexpr c3::Square c3::Square::C2{10};
inline constexpr c3::Square c3::Square::D2{11};
inline constexpr c3::Square c3::Square::E2{12};
inline constexpr c3::Square c3::Square::F2{13};
inline constexpr c3::Square c3::Square::G2{14};
inline constexpr c3::Square c3::Square::H2{15};

inline constexpr c3::Square c3::Square::A3{16};
inline constexpr c3::Square c3::Square::B3{17};
inline constexpr c3::Square c3::Square::C3{18};
inline constexpr c3::Square c3::Square::D3{19};
inline constexpr c3::Square c3::Square::E3{20};
inline constexpr c3::Square c3::Square::F3{21};
inline constexpr c3::Square c3::Square::G3{22};
inline constexpr c3::Square c3::Square::H3{23};

inline constexpr c3::Square c3::Square::A4{24};
inline constexpr c3::Square c3::Square::B4{25};
inline constexpr c3::Square c3::Square::C4{26};
inline constexpr c3::Square c3::Square::D4{27};
inline constexpr c3::Square c3::Square::E4{28};
inline constexpr c3::Square c3::Square::F4{29};
inline constexpr c3::Square c3::Square::G4{30};
inline constexpr c3::Square c3::Square::H4{31};

inline constexpr c3::Square c3::Square::A5{32};
inline constexpr c3::Square c3::Square::B5{33};
inline constexpr c3::Square c3::Square::C5{34};
inline constexpr c3::Square c3::Square::D5{35};
inline constexpr c3::Square c3::Square::E5{36};
inline constexpr c3::Square c3::Square::F5{37};
inline constexpr c3::Square c3::Square::G5{38};
inline constexpr c3::Square c3::Square::H5{39};

inline constexpr c3::Square c3::Square::A6{40};
inline constexpr c3::Square c3::Square::B6{41};
inline constexpr c3::Square c3::Square::C6{42};
inline constexpr c3::Square c3::Square::D6{43};
inline constexpr c3::Square c3::Square::E6{44};
inline constexpr c3::Square c3::Square::F6{45};
inline constexpr c3::Square c3::Square::G6{46};
inline constexpr c3::Square c3::Square::H6{47};

inline constexpr c3::Square c3::Square::A7{48};
inline constexpr c3::Square c3::Square::B7{49};
inline constexpr c3::Square c3::Square::C7{50};
inline constexpr c3::Square c3::Square::D7{51};
inline constexpr c3::Square c3::Square::E7{52};
inline constexpr c3::Square c3::Square::F7{53};
inline constexpr c3::Square c3::Square::G7{54};
inline constexpr c3::Square c3::Square::H7{55};

inline constexpr c3::Square c3::Square::A8{56};
inline constexpr c3::Square c3::Square::B8{57};
inline constexpr c3::Square c3::Square::C8{58};
inline constexpr c3::Square c3::Square::D8{59};
inline constexpr c3::Square c3::Square::E8{60};
inline constexpr c3::Square c3::Square::F8{61};
inline constexpr c3::Square c3::Square::G8{62};
inline constexpr c3::Square c3::Square::H8{63};
