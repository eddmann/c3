#pragma once

#include <array>
#include <ostream>
#include <string>

#include "c3/colour.hpp"

namespace c3 {

/// Chess pieces enumeration.
/// Naming convention: [Colour][Piece]
/// Colours: W = White, B = Black
/// Pieces: P = Pawn, N = Knight (N to avoid confusion with King),
///         B = Bishop, R = Rook, Q = Queen, K = King
enum class Piece : int {
  WP, // White Pawn
  WN, // White Knight
  WB, // White Bishop
  WR, // White Rook
  WQ, // White Queen
  WK, // White King
  BP, // Black Pawn
  BN, // Black Knight
  BB, // Black Bishop
  BR, // Black Rook
  BQ, // Black Queen
  BK, // Black King
};

// Factories by colour.
constexpr Piece pawn(Colour colour) {
  return colour == Colour::White ? Piece::WP : Piece::BP;
}
constexpr Piece knight(Colour colour) {
  return colour == Colour::White ? Piece::WN : Piece::BN;
}
constexpr Piece bishop(Colour colour) {
  return colour == Colour::White ? Piece::WB : Piece::BB;
}
constexpr Piece rook(Colour colour) {
  return colour == Colour::White ? Piece::WR : Piece::BR;
}
constexpr Piece queen(Colour colour) {
  return colour == Colour::White ? Piece::WQ : Piece::BQ;
}
constexpr Piece king(Colour colour) {
  return colour == Colour::White ? Piece::WK : Piece::BK;
}

inline constexpr std::array<Piece, 12> ALL_PIECES = {
    Piece::WP, Piece::WN, Piece::WB, Piece::WR, Piece::WQ, Piece::WK,
    Piece::BP, Piece::BN, Piece::BB, Piece::BR, Piece::BQ, Piece::BK,
};

inline constexpr std::array<std::array<Piece, 6>, 2> PIECES_BY_COLOUR = {{
    {Piece::WP, Piece::WN, Piece::WB, Piece::WR, Piece::WQ, Piece::WK},
    {Piece::BP, Piece::BN, Piece::BB, Piece::BR, Piece::BQ, Piece::BK},
}};

inline constexpr std::array<std::array<Piece, 4>, 2> PROMOTION_PIECES = {{
    {Piece::WN, Piece::WB, Piece::WR, Piece::WQ},
    {Piece::BN, Piece::BB, Piece::BR, Piece::BQ},
}};

constexpr const std::array<Piece, 12>& all_pieces() {
  return ALL_PIECES;
}

constexpr const std::array<Piece, 6>& pieces_for(Colour colour) {
  return PIECES_BY_COLOUR[static_cast<std::size_t>(colour)];
}

constexpr const std::array<Piece, 4>& promotions_for(Colour colour) {
  return PROMOTION_PIECES[static_cast<std::size_t>(colour)];
}

constexpr bool is_pawn(Piece piece) {
  return piece == Piece::WP || piece == Piece::BP;
}

constexpr bool is_king(Piece piece) {
  return piece == Piece::WK || piece == Piece::BK;
}

constexpr Colour colour(Piece piece) {
  return static_cast<int>(piece) <= static_cast<int>(Piece::WK) ? Colour::White : Colour::Black;
}

constexpr char to_char(Piece piece) {
  switch (piece) {
  case Piece::WP:
    return 'P';
  case Piece::WN:
    return 'N';
  case Piece::WB:
    return 'B';
  case Piece::WR:
    return 'R';
  case Piece::WQ:
    return 'Q';
  case Piece::WK:
    return 'K';
  case Piece::BP:
    return 'p';
  case Piece::BN:
    return 'n';
  case Piece::BB:
    return 'b';
  case Piece::BR:
    return 'r';
  case Piece::BQ:
    return 'q';
  case Piece::BK:
    return 'k';
  }
  return '?';
}

inline std::string to_string(Piece piece) {
  return std::string(1, to_char(piece));
}

inline std::ostream& operator<<(std::ostream& os, Piece piece) {
  os << to_char(piece);
  return os;
}

} // namespace c3
