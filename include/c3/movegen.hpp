#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "c3/attacks.hpp"
#include "c3/move.hpp"
#include "c3/position.hpp"

namespace c3 {

// How much room to reserve up front for a move list.
//
// The busiest legal position anyone has constructed offers 218 moves (a board
// packed with promoted queens); a normal middlegame offers 30 to 40. This is a
// hint, not a limit: MoveList grows on demand, so a position that needed more
// would only cost a reallocation. We round the known 218 up to 256 so that the
// hint comfortably covers every position that can occur.
inline constexpr std::size_t MOVE_LIST_RESERVE = 256;
using MoveList = std::vector<Move>;

MoveList pseudo_legal_moves(const Position& pos);
MoveList pseudo_legal_noisy_moves(const Position& pos);
MoveList legal_moves(const Position& pos);
std::uint64_t perft(Position& pos, std::uint8_t depth);

} // namespace c3
