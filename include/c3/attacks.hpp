#pragma once

#include <cstdint>

#include "c3/bitboard.hpp"
#include "c3/board.hpp"
#include "c3/colour.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {

Bitboard attacks_for(Piece piece, Square square, const Board& board);
Bitboard get_attackers(Square square, Colour colour, const Board& board);
bool is_attacked(Square square, Colour colour, const Board& board);
bool is_in_check(Colour colour, const Board& board);
Bitboard en_passant_sources(Square en_passant_square, Colour colour, const Board& board);

} // namespace c3
