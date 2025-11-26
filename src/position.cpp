// =============================================================================
// POSITION STATE AND MOVE EXECUTION
// =============================================================================
//
// A chess position is more than just piece placement—it also includes:
//   - Side to move (white or black)
//   - Castling rights (which castles are still legal)
//   - En passant square (if a pawn just moved two squares)
//   - Move clocks (for 50-move rule and move counting)
//   - Zobrist hash key (for transposition table lookups)
//
// This file handles two critical operations:
//
// 1. MAKE/UNMAKE MOVES
//    Making a move updates the board AND all auxiliary state. Unmaking restores
//    the previous state. This must be fast since search explores millions of
//    positions per second.
//
// 2. INCREMENTAL ZOBRIST HASHING
//    Rather than recompute the hash from scratch after each move (expensive),
//    we update it incrementally using XOR's self-inverse property:
//      - XOR out the old piece position
//      - XOR in the new piece position
//    This is O(1) per move instead of O(number_of_pieces).
//
// 3. REPETITION DETECTION
//    Chess is drawn if the same position occurs three times. We detect this
//    by comparing Zobrist keys in the move history. This is also used during
//    search to avoid infinite loops.
//
// =============================================================================

#include "c3/position.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

#include "c3/attacks.hpp"
#include "c3/bitboard.hpp"
#include "c3/piece.hpp"
#include "c3/zobrist.hpp"

namespace c3 {

namespace {

constexpr std::size_t piece_index(Piece piece) {
  return static_cast<std::size_t>(piece);
}

} // namespace

Position::Position() {
  history_.reserve(MAX_HISTORY);
  key = compute_key();
}

Position::Position(Board board_, Colour colour_to_move_, CastlingRights castling_rights_,
                   std::optional<Square> en_passant_square_, std::uint8_t half_move_clock_,
                   std::uint8_t full_move_counter_)
    : board(board_), colour_to_move(colour_to_move_), castling_rights(castling_rights_),
      en_passant_square(en_passant_square_), half_move_clock(half_move_clock_),
      full_move_counter(full_move_counter_) {
  history_.reserve(MAX_HISTORY);
  key = compute_key();
}

// Compute the full Zobrist hash from scratch.
// Used for initialization; during play, we maintain the hash incrementally.
// This is O(number_of_pieces) and serves as a correctness check in debug builds.
std::uint64_t Position::compute_key() const {
  std::uint64_t result = 0;

  // XOR in all pieces
  for (const auto piece : all_pieces()) {
    Bitboard bitboard = board.pieces(piece);
    while (bitboard != 0) {
      const Square square = Square::pop_first_occupied(bitboard);
      result ^= ZOBRIST.piece_square[static_cast<std::size_t>(piece)][square.index()];
    }
  }

  // XOR in side to move (only for black; white positions don't include this)
  if (colour_to_move == Colour::Black) {
    result ^= ZOBRIST.colour_to_move;
  }

  // XOR in castling rights (16 possible combinations → 16 precomputed values)
  result ^= ZOBRIST.castling_rights[castling_rights.value()];

  // XOR in en passant file ONLY if a pawn can actually capture en passant.
  // This ensures positions that differ only in "phantom" en passant squares
  // (where no capture is possible) hash identically—important for transpositions.
  if (en_passant_square.has_value() &&
      en_passant_sources(*en_passant_square, colour_to_move, board) != 0) {
    result ^= ZOBRIST.en_passant_files[en_passant_square->file()];
  }

  return result;
}

// =============================================================================
// MAKE MOVE: Execute a move with incremental state updates
// =============================================================================
// This is the core of position manipulation. We:
//   1. Save irreversible state to history (for unmake)
//   2. Update the Zobrist hash incrementally (XOR out old, XOR in new)
//   3. Update the board and all auxiliary state
//
// The history stack stores state that can't be reconstructed from the move
// alone: castling rights, en passant, half-move clock, and the previous hash.
// =============================================================================

void Position::make_move(const Move& mv) {
  // Save state that can't be derived from the move alone.
  // This enables unmake_move to restore the exact previous position.
  const detail::HistoryEntry history{
      .castling_rights = castling_rights,
      .en_passant_square = en_passant_square,
      .half_move_clock = half_move_clock,
      .key = key,
  };
  history_.push_back(history);
  assert(history_.size() <= MAX_HISTORY);

  if (en_passant_square.has_value() &&
      en_passant_sources(*en_passant_square, colour_to_move, board) != 0) {
    key ^= ZOBRIST.en_passant_files[en_passant_square->file()];
  }

  en_passant_square = std::nullopt;
  ++half_move_clock;

  if (const auto capture_square = mv.capture_square()) {
    half_move_clock = 0;
    board.remove_piece(*capture_square);
    key ^= ZOBRIST.piece_square[piece_index(*mv.captured_piece)][capture_square->index()];
  }

  if (is_pawn(mv.piece)) {
    half_move_clock = 0;

    if (mv.rank_diff() == 2) {
      const Square square = mv.from.advance(colour_to_move);
      en_passant_square = square;

      if (en_passant_sources(square, opponent_colour(), board) != 0) {
        key ^= ZOBRIST.en_passant_files[square.file()];
      }
    }
  }

  if (is_king(mv.piece)) {
    castling_rights.remove_for_colour(colour_to_move);

    if (mv.is_castling()) {
      const Piece rook_piece = rook(colour_to_move);

      if (mv.to == Square::C1 || mv.to == Square::C8) {
        const Square rook_to = Square::from_file_and_rank(3, mv.to.rank());
        const Square rook_from = Square::from_file_and_rank(0, mv.to.rank());

        board.put_piece(rook_piece, rook_to);
        board.remove_piece(rook_from);

        key ^= ZOBRIST.piece_square[piece_index(rook_piece)][rook_to.index()];
        key ^= ZOBRIST.piece_square[piece_index(rook_piece)][rook_from.index()];
      } else if (mv.to == Square::G1 || mv.to == Square::G8) {
        const Square rook_to = Square::from_file_and_rank(5, mv.to.rank());
        const Square rook_from = Square::from_file_and_rank(7, mv.to.rank());

        board.put_piece(rook_piece, rook_to);
        board.remove_piece(rook_from);

        key ^= ZOBRIST.piece_square[piece_index(rook_piece)][rook_to.index()];
        key ^= ZOBRIST.piece_square[piece_index(rook_piece)][rook_from.index()];
      }
    }
  }

  if (mv.from.is_corner()) {
    castling_rights.remove_for_square(mv.from);
  }

  if (mv.to.is_corner()) {
    castling_rights.remove_for_square(mv.to);
  }

  key ^= ZOBRIST.castling_rights[castling_rights.value()];
  key ^= ZOBRIST.castling_rights[history.castling_rights.value()];

  const Piece to_piece = mv.promotion_piece.value_or(mv.piece);
  board.put_piece(to_piece, mv.to);
  board.remove_piece(mv.from);

  key ^= ZOBRIST.piece_square[piece_index(to_piece)][mv.to.index()];
  key ^= ZOBRIST.piece_square[piece_index(mv.piece)][mv.from.index()];

  if (colour_to_move == Colour::Black) {
    ++full_move_counter;
  }

  colour_to_move = opponent_colour();
  key ^= ZOBRIST.colour_to_move;

#ifndef NDEBUG
  assert(key == compute_key());
#endif
}

void Position::unmake_move(const Move& mv) {
  assert(!history_.empty());
  const auto history = history_.back();
  history_.pop_back();

  castling_rights = history.castling_rights;
  en_passant_square = history.en_passant_square;
  half_move_clock = history.half_move_clock;
  key = history.key;

  if (mv.is_castling()) {
    const Piece rook_piece = rook(opponent_colour());

    if (mv.to == Square::C1 || mv.to == Square::C8) {
      const Square rook_to = Square::from_file_and_rank(0, mv.to.rank());
      const Square rook_from = Square::from_file_and_rank(3, mv.to.rank());

      board.put_piece(rook_piece, rook_to);
      board.remove_piece(rook_from);
    } else if (mv.to == Square::G1 || mv.to == Square::G8) {
      const Square rook_to = Square::from_file_and_rank(7, mv.to.rank());
      const Square rook_from = Square::from_file_and_rank(5, mv.to.rank());

      board.put_piece(rook_piece, rook_to);
      board.remove_piece(rook_from);
    }
  }

  board.remove_piece(mv.to);
  board.put_piece(mv.piece, mv.from);

  if (const auto capture_square = mv.capture_square()) {
    board.put_piece(*mv.captured_piece, *capture_square);
  }

  colour_to_move = opponent_colour();

  if (colour_to_move == Colour::Black) {
    --full_move_counter;
  }

#ifndef NDEBUG
  assert(key == compute_key());
#endif
}

// =============================================================================
// NULL MOVE: Skip a turn (used in null-move pruning)
// =============================================================================
// A "null move" passes the turn without moving any piece. This is illegal in
// real chess but useful in search: if doing nothing still gives a good score,
// the position is probably winning and we can prune the search tree.
//
// We still need to update side-to-move and save history for unmake.
// =============================================================================

void Position::make_null_move() {
  const detail::HistoryEntry history{
      .castling_rights = castling_rights,
      .en_passant_square = en_passant_square,
      .half_move_clock = half_move_clock,
      .key = key,
  };
  history_.push_back(history);
  assert(history_.size() <= MAX_HISTORY);

  if (en_passant_square.has_value() &&
      en_passant_sources(*en_passant_square, colour_to_move, board) != 0) {
    key ^= ZOBRIST.en_passant_files[en_passant_square->file()];
  }

  en_passant_square = std::nullopt;
  ++half_move_clock;

  if (colour_to_move == Colour::Black) {
    ++full_move_counter;
  }

  colour_to_move = opponent_colour();
  key ^= ZOBRIST.colour_to_move;

#ifndef NDEBUG
  assert(key == compute_key());
#endif
}

void Position::unmake_null_move() {
  assert(!history_.empty());
  const auto history = history_.back();
  history_.pop_back();

  castling_rights = history.castling_rights;
  en_passant_square = history.en_passant_square;
  half_move_clock = history.half_move_clock;
  key = history.key;

  colour_to_move = opponent_colour();

  if (colour_to_move == Colour::Black) {
    --full_move_counter;
  }

#ifndef NDEBUG
  assert(key == compute_key());
#endif
}

// =============================================================================
// REPETITION DETECTION
// =============================================================================
// A position is drawn by threefold repetition if it occurs three times. We also
// treat repetition within the search tree specially: a single repetition during
// search is considered a draw to avoid infinite loops.
//
// Key insight: Same position = same Zobrist key (with high probability).
// We only compare keys at odd distances (same side to move) since the same
// board with different sides to move is a different position.
//
// The half_move_clock < 8 check is an optimization: a capture or pawn move
// resets irreversibility, so positions before that can't be repeated.
// =============================================================================

bool Position::is_repetition_draw(std::uint8_t search_ply) const {
  // Optimization: positions can only repeat if no captures/pawn moves happened
  if (half_move_clock < 8) {
    return false;
  }

  int counter = 0;
  const std::size_t limit = std::min<std::size_t>(half_move_clock, history_.size());

  for (std::size_t distance = 0; distance < limit; ++distance) {
    // Only check odd distances (same side to move) and skip first few plies
    if (distance < 3 || (distance % 2 == 0)) {
      continue;
    }

    const auto& entry = history_[history_.size() - 1 - distance];
    if (entry.key != key) {
      continue;
    }

    // Within the current search: single repetition = draw (avoid cycles)
    if (distance < static_cast<std::size_t>(search_ply)) {
      return true;
    }

    // Outside current search: need two repetitions for threefold
    ++counter;
    if (counter == 2) {
      return true;
    }
  }

  return false;
}

} // namespace c3
