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

  search::TranspositionTable& tt() { return tt_; }

  void new_game();
  void set_position(const Position& pos);
  void set_position_from_fen(const std::string& fen);

  void apply_move(const Move& mv);
  void apply_moves(const std::vector<Move>& moves);

  search::SearchResult search(const search::Limits& limits, search::Reporter& reporter,
                              std::shared_ptr<std::atomic_bool> stop_signal = nullptr) const;

  void set_hash_size_mb(std::size_t size_mb);

private:
  Position pos_;
  mutable search::TranspositionTable tt_;
};

} // namespace c3
