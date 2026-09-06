#pragma once

#include <cstdint>

#include "c3/bitboard.hpp"
#include "c3/board.hpp"
#include "c3/colour.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {

// SLIDING ATTACKS OVER AN OCCUPANCY THE CALLER CHOOSES.
//
// A sliding piece's attacks depend only on which squares are BLOCKED, and the
// board is not the only interesting answer to that question. Static exchange
// evaluation walks an exchange by removing pieces from a private occupancy mask
// as they capture, and asks these after every removal: that is how the queen
// standing behind a rook joins the exchange by itself once the rook has left,
// with no board to copy and no piece to move.
//
// The Board-taking forms below are these two, asked about board.occupancy().
Bitboard bishop_attacks(Square square, Bitboard occupancy);
Bitboard rook_attacks(Square square, Bitboard occupancy);

Bitboard attacks_for(Piece piece, Square square, const Board& board);
Bitboard get_attackers(Square square, Colour colour, const Board& board);
bool is_attacked(Square square, Colour colour, const Board& board);
bool is_in_check(Colour colour, const Board& board);
Bitboard en_passant_sources(Square en_passant_square, Colour colour, const Board& board);

} // namespace c3
