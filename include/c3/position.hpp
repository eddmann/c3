#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "c3/board.hpp"
#include "c3/castling.hpp"
#include "c3/colour.hpp"
#include "c3/move.hpp"
#include "c3/square.hpp"

namespace c3 {

namespace detail {

struct HistoryEntry {
  CastlingRights castling_rights;
  std::optional<Square> en_passant_square;
  std::uint8_t half_move_clock;
  std::uint64_t key;
};

} // namespace detail

// FEN-aware position container.
class Position {
public:
  inline static constexpr std::size_t MAX_HISTORY = 256;

  Board board{};
  Colour colour_to_move{Colour::White};
  CastlingRights castling_rights{CastlingRights::none()};
  std::optional<Square> en_passant_square{};
  std::uint8_t half_move_clock{0};
  std::uint8_t full_move_counter{1};
  std::uint64_t key{0};

  Position();

  Position(Board board, Colour colour_to_move, CastlingRights castling_rights,
           std::optional<Square> en_passant_square, std::uint8_t half_move_clock,
           std::uint8_t full_move_counter);

  // Parse a FEN string into a Position, throwing std::runtime_error on error.
  static Position from_fen(std::string_view fen);

  // Convenience: starting position constant.
  static Position startpos() { return from_fen(START_POS_FEN); }

  // Serialise position back to FEN.
  std::string to_fen() const;

  // Calculate Zobrist hash for current state.
  std::uint64_t compute_key() const;

  // Make/unmake moves with incremental Zobrist maintenance.
  void make_move(const Move& mv);
  void unmake_move(const Move& mv);

  // Null moves used in search heuristics.
  void make_null_move();
  void unmake_null_move();

  // Draw detection helpers.
  bool is_repetition_draw(std::uint8_t search_ply) const;
  bool is_fifty_move_draw() const { return half_move_clock >= 100; }

  // Convenience for consumers.
  Colour opponent_colour() const { return !colour_to_move; }

  // Standard starting position FEN.
  inline static constexpr std::string_view START_POS_FEN =
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

private:
  std::vector<detail::HistoryEntry> history_{};
};

// Helper used by tests and UCI printing.
std::string castling_rights_to_fen(CastlingRights rights);

} // namespace c3
