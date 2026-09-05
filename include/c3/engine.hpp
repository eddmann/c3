#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "c3/move.hpp"
#include "c3/position.hpp"
#include "c3/search.hpp"

namespace c3 {

// Engine façade that owns mutable game state and offers a simple search entry
// point. UCI or other frontends can build on this without needing to manage
// Position lifetimes directly.
class Engine {
public:
  Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Position& position() { return pos_; }
  const Position& position() const { return pos_; }

  void new_game();
  void set_position(const Position& pos);
  void set_position_from_fen(const std::string& fen);

  void apply_move(const Move& mv);
  void apply_moves(const std::vector<Move>& moves);

  search::SearchResult search(const search::Limits& limits, search::Reporter& reporter,
                              std::shared_ptr<std::atomic_bool> stop_signal = nullptr);

  // Resizing throws away the table's contents, so this is deliberately an
  // explicit user action ("setoption name Hash") rather than something the
  // search does per move.
  void set_hash_size_mb(std::size_t size_mb);

  // Exposed so callers (and tests) can inspect or reuse the persistent table.
  search::TranspositionTable& transposition_table() { return tt_; }
  const search::TranspositionTable& transposition_table() const { return tt_; }

private:
  Position pos_;

  // The transposition table belongs to the Engine, not to a single search:
  // it is expensive to allocate and the knowledge in it stays valid from one
  // move to the next. new_game() is the only thing that clears it.
  search::TranspositionTable tt_;
};

} // namespace c3
