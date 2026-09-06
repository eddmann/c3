#pragma once

#include <cstdint>

#include "c3/attacks.hpp"
#include "c3/move.hpp"
#include "c3/move_list.hpp"
#include "c3/position.hpp"

namespace c3 {

// FILL A LIST THE CALLER ALREADY OWNS, rather than handing one back.
//
// A MoveList is a fixed-capacity array of 256 moves—two kilobytes—and that is
// the whole reason these exist. Returning one by value materialises those two
// kilobytes in the CALLER's stack frame, even when the caller means to move it
// straight into storage it already has: there is nothing to elide the copy
// into, because the destination is not a fresh local. In a recursion up to 255
// frames deep that is half a megabyte of stack spent on temporaries, which is
// more than the search thread has on macOS.
//
// So the search fills its own per-ply row through these, and its frames hold no
// move list at all. `moves` is cleared first, so a reused row starts empty.
void pseudo_legal_moves_into(const Position& pos, MoveList& moves);
void pseudo_legal_noisy_moves_into(const Position& pos, MoveList& moves);

// The same generators for callers off the hot path—tests, tools, the UCI
// frontend—where a returned list reads better than an out-parameter and the two
// kilobytes are paid once rather than at every node.
MoveList pseudo_legal_moves(const Position& pos);
MoveList pseudo_legal_noisy_moves(const Position& pos);
MoveList legal_moves(const Position& pos);
std::uint64_t perft(Position& pos, std::uint8_t depth);

} // namespace c3
