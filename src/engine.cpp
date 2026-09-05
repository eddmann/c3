#include "c3/engine.hpp"

#include <utility>

namespace c3 {

Engine::Engine() : pos_(Position::startpos()) {}

void Engine::new_game() {
  pos_ = Position::startpos();
  // A new game means the stored positions describe a tree we will never visit
  // again, so the table is wiped. Note this keeps the allocation—only the
  // contents go.
  tt_.clear();
}

void Engine::set_position(const Position& pos) {
  pos_ = pos;
}

void Engine::set_position_from_fen(const std::string& fen) {
  pos_ = Position::from_fen(fen);
}

void Engine::apply_move(const Move& mv) {
  pos_.make_move(mv);
}

void Engine::apply_moves(const std::vector<Move>& moves) {
  for (const auto& mv : moves) {
    pos_.make_move(mv);
  }
}

search::SearchResult Engine::search(const search::Limits& limits, search::Reporter& reporter,
                                    std::shared_ptr<std::atomic_bool> stop_signal) {
  Position pos_copy = pos_;
  return search::search(pos_copy, tt_, limits, reporter, std::move(stop_signal));
}

void Engine::set_hash_size_mb(std::size_t size_mb) {
  tt_.resize(size_mb);

  // TRANSITIONAL. The UCI `go` handler does not yet search through this
  // Engine's table—it still builds a throwaway one per move—so resizing our
  // own table would leave "setoption name Hash" with no visible effect there.
  // Moving the process-wide default too keeps the option honest until UCI is
  // wired to the Engine, at which point this line should go.
  search::TranspositionTable::set_size_mb(size_mb);
}

} // namespace c3
