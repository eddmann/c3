#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "c3/attacks.hpp"
#include "c3/move.hpp"
#include "c3/position.hpp"

namespace c3 {

inline constexpr std::size_t MAX_LEGAL_MOVES = 128;
using MoveList = std::vector<Move>;

MoveList pseudo_legal_moves(const Position& pos);
MoveList pseudo_legal_noisy_moves(const Position& pos);
std::uint64_t perft(Position& pos, std::uint8_t depth);

} // namespace c3
