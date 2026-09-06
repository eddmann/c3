#pragma once

#include <cstdint>

#include "c3/attacks.hpp"
#include "c3/move.hpp"
#include "c3/move_list.hpp"
#include "c3/position.hpp"

namespace c3 {

MoveList pseudo_legal_moves(const Position& pos);
MoveList pseudo_legal_noisy_moves(const Position& pos);
MoveList legal_moves(const Position& pos);
std::uint64_t perft(Position& pos, std::uint8_t depth);

} // namespace c3
