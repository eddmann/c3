// =============================================================================
// STATIC EVALUATION IMPLEMENTATION
// =============================================================================
//
// This file implements position evaluation using two components:
//
// 1. MATERIAL COUNT
//    Sum up piece values for each side. Simple but fundamental—being a queen
//    up is almost always winning.
//
// 2. PIECE-SQUARE TABLES (PSQT)
//    Bonuses/penalties based on where pieces are located. Encodes positional
//    knowledge like:
//      - Central control is valuable (knights/bishops in center)
//      - Pawns should advance (especially in endgames)
//      - Rooks belong on open files and the 7th rank
//      - King safety matters (stay castled in middlegame)
//
// The evaluation is symmetric: we compute White's score, Black's score, and
// return the difference from the perspective of the side to move.
//
// =============================================================================

#include "c3/eval.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {
namespace {

// =============================================================================
// PIECE-SQUARE TABLES
// =============================================================================
// Each table has 64 values (one per square) giving a bonus/penalty in centipawns
// for having that piece on that square. Tables are defined from WHITE's perspective
// (A1 is bottom-left), and flipped for black pieces.
//
// Positive values = good square for this piece
// Negative values = bad square for this piece
//
// These values come from chess knowledge and tuning. They're not perfect but
// capture important positional principles.
// =============================================================================

// clang-format off
constexpr std::array<std::array<int, 64>, 6> PIECE_SQUARE_BASE = {{
    // PAWN: Encourage advancement. Pawns on ranks 6-7 are close to promotion.
    // Center pawns (d4/e4) control key squares. Avoid blocking f2/c2 pawns.
    {
         0,   0,   0,   0,   0,   0,   0,   0,
        60,  60,  60,  60,  70,  60,  60,  60,
        40,  40,  40,  50,  60,  40,  40,  40,
        20,  20,  20,  40,  50,  20,  20,  20,
         5,   5,  15,  30,  40,  10,   5,   5,
         5,   5,  10,  20,  30,   5,   5,   5,
         5,   5,   5, -30, -30,   5,   5,   5,
         0,   0,   0,   0,   0,   0,   0,   0,
    },
    // KNIGHT: Strong in the center, weak on the rim ("knight on the rim is dim").
    // Knights need outposts and are poor in corners where they control few squares.
    {
        -20, -10, -10, -10, -10, -10, -10, -20,
        -10,  -5,  -5,  -5,  -5,  -5,  -5, -10,
        -10,  -5,  15,  15,  15,  15,  -5, -10,
        -10,  -5,  15,  15,  15,  15,  -5, -10,
        -10,  -5,  15,  15,  15,  15,  -5, -10,
        -10,  -5,  10,  15,  15,  15,  -5, -10,
        -10,  -5,  -5,  -5,  -5,  -5,  -5, -10,
        -20, -10, -10, -10, -10, -10, -10, -20,
    },
    // BISHOP: Long diagonals are powerful. Avoid edges where diagonal reach is limited.
    // The center squares let bishops influence both sides of the board.
    {
        -20,   0,   0,   0,   0,   0,   0, -20,
        -15,   0,   0,   0,   0,   0,   0, -15,
        -10,   0,   0,   5,   5,   0,   0, -10,
        -10,  10,  10,  30,  30,  10,  10, -10,
          5,   5,  10,  25,  25,  10,   5,   5,
          5,   5,   5,  10,  10,   5,   5,   5,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -20, -10, -10, -10, -10, -10, -10, -20,
    },
    // ROOK: The 7th rank is powerful (attacks pawns, traps king). Open files matter.
    // Centralized rooks can swing to either side. Small bonus for staying near center.
    {
         0,   0,   0,   0,   0,   0,   0,   0,
        15,  15,  15,  20,  20,  15,  15,  15,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,  10,  10,  10,   0,   0,
    },
    // QUEEN: Avoid early queen development (gets harassed). Central queens are strong
    // but vulnerable. Moderate bonuses for center control without overcommitting.
    {
        -30, -20, -10, -10, -10, -10, -20, -30,
        -20, -10,  -5,  -5,  -5,  -5, -10, -20,
        -10,  -5,  10,  10,  10,  10,  -5, -10,
        -10,  -5,  10,  20,  20,  10,  -5, -10,
        -10,  -5,  10,  20,  20,  10,  -5, -10,
        -10,  -5,  -5,  -5,  -5,  -5,  -5, -10,
        -20, -10,  -5,  -5,  -5,  -5, -10, -20,
        -30, -20, -10, -10, -10, -10, -20, -30,
    },
    // KING: In middlegame, stay castled and protected (penalties for center exposure).
    // The g1/c1 squares have bonuses for castled positions. Central king is dangerous
    // when queens are on the board. (Endgame would prefer a centralized king, but
    // this simple table doesn't distinguish game phases.)
    {
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0,  20,  20,   0,   0,   0,
         0,   0,   0,  20,  20,   0,   0,   0,
         0,   0,   0,   0,   0,   0,   0,   0,
         0,   0,   0, -10, -10,   0,   0,   0,
         0,   0,  20, -10, -10,   0,  20,   0,
    },
}};
// clang-format on

// =============================================================================
// RANK FLIP TABLE
// =============================================================================
// PSQTs above are defined from White's perspective (rank 1 at bottom, rank 8 at top).
// For Black, we need to flip vertically: Black's rank 1 should use White's rank 8 values.
//
// This table maps square indices to their "flipped" equivalents:
//   - For White: identity mapping (no flip needed)
//   - For Black: vertical mirror (A1↔A8, B2↔B7, etc.)
// =============================================================================

// clang-format off
constexpr std::array<std::array<std::size_t, 64>, 2> RANK_FLIP_TABLE = {{
    // White
    {{
        56, 57, 58, 59, 60, 61, 62, 63,
        48, 49, 50, 51, 52, 53, 54, 55,
        40, 41, 42, 43, 44, 45, 46, 47,
        32, 33, 34, 35, 36, 37, 38, 39,
        24, 25, 26, 27, 28, 29, 30, 31,
        16, 17, 18, 19, 20, 21, 22, 23,
         8,  9, 10, 11, 12, 13, 14, 15,
         0,  1,  2,  3,  4,  5,  6,  7,
    }},
    // Black
    {{
         0,  1,  2,  3,  4,  5,  6,  7,
         8,  9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23,
        24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39,
        40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55,
        56, 57, 58, 59, 60, 61, 62, 63,
    }},
}};
// clang-format on

// Build the final PSQT array (12 piece types × 64 squares) at compile time.
// Combines the base tables with rank flipping for each color.
constexpr std::array<std::array<int, 64>, 12> build_psqt() {
  std::array<std::array<int, 64>, 12> psqt{};

  for (const auto piece : all_pieces()) {
    const auto colour_index = static_cast<std::size_t>(colour(piece));
    const auto piece_index = static_cast<std::size_t>(piece);
    const auto white_piece_index = piece_index % 6;

    for (std::size_t square = 0; square < 64; ++square) {
      const auto mapped_square = RANK_FLIP_TABLE[colour_index][square];
      psqt[piece_index][square] = PIECE_SQUARE_BASE[white_piece_index][mapped_square];
    }
  }

  return psqt;
}

constexpr auto PIECE_SQUARE_TABLES = build_psqt();

// =============================================================================
// GAME PHASE CALCULATION
// =============================================================================
// Game phase helps scale evaluation terms appropriately:
//   - Middlegame (phase ~256): King safety is critical
//   - Endgame (phase ~0): King should centralize, safety less important
//
// Phase is calculated based on non-pawn material remaining.
// =============================================================================

constexpr int PHASE_KNIGHT = 1;
constexpr int PHASE_BISHOP = 1;
constexpr int PHASE_ROOK = 2;
constexpr int PHASE_QUEEN = 4;
constexpr int PHASE_TOTAL = 4 * PHASE_KNIGHT + 4 * PHASE_BISHOP + 4 * PHASE_ROOK + 2 * PHASE_QUEEN;

int calculate_game_phase(const Board& board) {
  int phase = 0;
  phase += PHASE_KNIGHT * static_cast<int>(std::popcount(board.pieces(Piece::WN)));
  phase += PHASE_KNIGHT * static_cast<int>(std::popcount(board.pieces(Piece::BN)));
  phase += PHASE_BISHOP * static_cast<int>(std::popcount(board.pieces(Piece::WB)));
  phase += PHASE_BISHOP * static_cast<int>(std::popcount(board.pieces(Piece::BB)));
  phase += PHASE_ROOK * static_cast<int>(std::popcount(board.pieces(Piece::WR)));
  phase += PHASE_ROOK * static_cast<int>(std::popcount(board.pieces(Piece::BR)));
  phase += PHASE_QUEEN * static_cast<int>(std::popcount(board.pieces(Piece::WQ)));
  phase += PHASE_QUEEN * static_cast<int>(std::popcount(board.pieces(Piece::BQ)));

  // Normalize to 0-256 range (256 = full middlegame, 0 = pure endgame)
  return (phase * 256 + PHASE_TOTAL / 2) / PHASE_TOTAL;
}

// =============================================================================
// KING SAFETY EVALUATION CONSTANTS
// =============================================================================
// King safety is evaluated based on:
//   1. Pawn shield - bonuses for pawns protecting the king
//   2. Open files - penalties for open/semi-open files near king
//   3. Attack zone - penalties for enemy pieces attacking king zone
//   4. Tropism - distance-based penalties for enemy pieces near king
//
// All values are scaled by game phase (more important in middlegame).
// =============================================================================

// Pawn shield bonuses (centipawns) for pawns on ranks 2, 3, and 4+ respectively
// Closer pawns provide better protection
constexpr std::array<int, 3> PAWN_SHIELD_BONUS = {12, 8, 4};

// Penalties for missing critical pawns in the shield
// f-pawn is most critical as it protects key diagonal squares around castled king
constexpr int MISSING_F_PAWN_PENALTY = -25;
constexpr int MISSING_G_PAWN_PENALTY = -15;
constexpr int MISSING_H_PAWN_PENALTY = -8;

// Open file penalties (centipawns)
constexpr int OPEN_FILE_PENALTY = -20;      // No pawns on file near king
constexpr int SEMI_OPEN_FILE_PENALTY = -10; // Only enemy pawn on file

// Attack weights for pieces threatening king zone
// These are base values, scaled by attacker count for quadratic effect
constexpr std::array<int, 6> ATTACK_WEIGHTS = {
    0, // Pawn - handled separately
    2, // Knight
    2, // Bishop
    3, // Rook
    5, // Queen
    0  // King - doesn't attack like other pieces
};

// Tropism weights - penalty for enemy pieces being close to our king
constexpr std::array<int, 6> TROPISM_WEIGHTS = {
    0, // Pawn
    1, // Knight
    1, // Bishop
    2, // Rook
    3, // Queen
    0  // King
};

// Safety scaling when enemy has no queen (attacks are much less dangerous)
constexpr int NO_QUEEN_SAFETY_DIVISOR = 4;

// Get pawn shield files based on king position
// Returns array of {left_file, king_file, right_file} or -1 if off board
constexpr std::array<int, 3> shield_files(Square king_sq) {
  const int file = static_cast<int>(king_sq.file());
  return {file > 0 ? file - 1 : -1, file, file < 7 ? file + 1 : -1};
}

// Check if king is on kingside (files e-h)
constexpr bool is_kingside(Square king_sq) {
  return king_sq.file() >= 4;
}

// Evaluate pawn shield for one side
int eval_pawn_shield(Colour colour, Square king_sq, const Board& board) {
  const Bitboard own_pawns = board.pieces(pawn(colour));
  const auto files = shield_files(king_sq);

  // Determine shield rank based on colour (rank 2 for white, rank 7 for black)
  const int base_rank = colour == Colour::White ? 1 : 6;
  const int direction = colour == Colour::White ? 1 : -1;

  int score = 0;

  for (int i = 0; i < 3; ++i) {
    const int file = files[static_cast<std::size_t>(i)];
    if (file < 0) {
      continue;
    }

    // Check ranks 2, 3, 4 (or 7, 6, 5 for black) for shield pawns
    bool found_pawn = false;
    for (int rank_offset = 0; rank_offset < 3 && !found_pawn; ++rank_offset) {
      const int rank = base_rank + direction * rank_offset;
      if (rank < 0 || rank > 7) {
        continue;
      }

      const Square sq = Square::from_file_and_rank(static_cast<std::uint8_t>(file),
                                                   static_cast<std::uint8_t>(rank));
      if ((own_pawns & sq) != 0) {
        score += PAWN_SHIELD_BONUS[static_cast<std::size_t>(rank_offset)];
        found_pawn = true;
      }
    }

    // Apply penalties for missing critical pawns
    if (!found_pawn) {
      if (is_kingside(king_sq)) {
        // Kingside castled - f, g, h pawns are critical
        if (file == 5) {
          score += MISSING_F_PAWN_PENALTY;
        } else if (file == 6) {
          score += MISSING_G_PAWN_PENALTY;
        } else if (file == 7) {
          score += MISSING_H_PAWN_PENALTY;
        }
      } else {
        // Queenside castled - a, b, c pawns are critical (mirrored penalties)
        if (file == 2) {
          score += MISSING_F_PAWN_PENALTY; // c-pawn is like f-pawn
        } else if (file == 1) {
          score += MISSING_G_PAWN_PENALTY; // b-pawn is like g-pawn
        } else if (file == 0) {
          score += MISSING_H_PAWN_PENALTY; // a-pawn is like h-pawn
        }
      }
    }
  }

  return score;
}

// Evaluate open files near king
int eval_open_files(Colour colour, Square king_sq, const Board& board) {
  const Bitboard own_pawns = board.pieces(pawn(colour));
  const Bitboard enemy_pawns = board.pieces(pawn(!colour));
  const auto files = shield_files(king_sq);

  int score = 0;

  for (std::size_t i = 0; i < 3; ++i) {
    const int file = files[i];
    if (file < 0) {
      continue;
    }

    const Bitboard file_mask = FILE_MASKS[static_cast<std::size_t>(file)];
    const bool has_own_pawn = (own_pawns & file_mask) != 0;
    const bool has_enemy_pawn = (enemy_pawns & file_mask) != 0;

    if (!has_own_pawn && !has_enemy_pawn) {
      score += OPEN_FILE_PENALTY;
    } else if (!has_own_pawn && has_enemy_pawn) {
      score += SEMI_OPEN_FILE_PENALTY;
    }
  }

  return score;
}

// Manhattan distance between two squares
constexpr int manhattan_distance(Square a, Square b) {
  return static_cast<int>(a.file_diff(b)) + static_cast<int>(a.rank_diff(b));
}

// Chebyshev distance (king distance) between two squares
constexpr int chebyshev_distance(Square a, Square b) {
  return std::max(static_cast<int>(a.file_diff(b)), static_cast<int>(a.rank_diff(b)));
}

// Find king square for a colour
Square find_king(Colour colour, const Board& board) {
  const Bitboard king_bb = board.pieces(king(colour));
  return Square::first_occupied(king_bb);
}

// Evaluate attack zone - enemy pieces threatening squares near our king
int eval_attack_zone(Colour colour, Square king_sq, const Board& board) {
  const Colour enemy = !colour;

  int attacker_count = 0;
  int attack_weight = 0;

  // Count enemy pieces that can attack the king zone
  // Knights
  Bitboard enemy_knights = board.pieces(knight(enemy));
  while (enemy_knights != 0) {
    const Square sq = Square::pop_first_occupied(enemy_knights);
    const int dist = chebyshev_distance(sq, king_sq);
    if (dist <= 2) { // Knight within striking range
      ++attacker_count;
      attack_weight += ATTACK_WEIGHTS[1];
    }
  }

  // Bishops
  Bitboard enemy_bishops = board.pieces(bishop(enemy));
  while (enemy_bishops != 0) {
    const Square sq = Square::pop_first_occupied(enemy_bishops);
    const int dist = chebyshev_distance(sq, king_sq);
    if (dist <= 3) {
      ++attacker_count;
      attack_weight += ATTACK_WEIGHTS[2];
    }
  }

  // Rooks
  Bitboard enemy_rooks = board.pieces(rook(enemy));
  while (enemy_rooks != 0) {
    const Square sq = Square::pop_first_occupied(enemy_rooks);
    const int dist = chebyshev_distance(sq, king_sq);
    if (dist <= 3) {
      ++attacker_count;
      attack_weight += ATTACK_WEIGHTS[3];
    }
  }

  // Queens
  Bitboard enemy_queens = board.pieces(queen(enemy));
  while (enemy_queens != 0) {
    const Square sq = Square::pop_first_occupied(enemy_queens);
    const int dist = chebyshev_distance(sq, king_sq);
    if (dist <= 4) {
      ++attacker_count;
      attack_weight += ATTACK_WEIGHTS[4];
    }
  }

  // Quadratic scaling: more attackers = exponentially worse
  // Score = -attack_weight * attacker_count (scaled)
  if (attacker_count == 0) {
    return 0;
  }

  return -attack_weight * attacker_count * 3;
}

// Evaluate tropism - distance of enemy pieces to our king
int eval_tropism(Colour colour, Square king_sq, const Board& board) {
  const Colour enemy = !colour;
  int score = 0;

  // For each enemy piece type, penalize based on proximity
  // Knights
  Bitboard enemy_knights = board.pieces(knight(enemy));
  while (enemy_knights != 0) {
    const Square sq = Square::pop_first_occupied(enemy_knights);
    const int dist = manhattan_distance(sq, king_sq);
    score -= TROPISM_WEIGHTS[1] * (14 - dist) / 2; // Max dist is 14
  }

  // Bishops
  Bitboard enemy_bishops = board.pieces(bishop(enemy));
  while (enemy_bishops != 0) {
    const Square sq = Square::pop_first_occupied(enemy_bishops);
    const int dist = manhattan_distance(sq, king_sq);
    score -= TROPISM_WEIGHTS[2] * (14 - dist) / 2;
  }

  // Rooks
  Bitboard enemy_rooks = board.pieces(rook(enemy));
  while (enemy_rooks != 0) {
    const Square sq = Square::pop_first_occupied(enemy_rooks);
    const int dist = manhattan_distance(sq, king_sq);
    score -= TROPISM_WEIGHTS[3] * (14 - dist) / 2;
  }

  // Queens
  Bitboard enemy_queens = board.pieces(queen(enemy));
  while (enemy_queens != 0) {
    const Square sq = Square::pop_first_occupied(enemy_queens);
    const int dist = manhattan_distance(sq, king_sq);
    score -= TROPISM_WEIGHTS[4] * (14 - dist) / 2;
  }

  return score;
}

} // namespace

// Count total material value for one side.
int eval_material(Colour colour, const Board& board) noexcept {
  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    total +=
        PIECE_VALUES[static_cast<std::size_t>(piece)] * static_cast<int>(board.count_pieces(piece));
  }
  return total;
}

// Sum PSQT bonuses for all pieces of one side.
int eval_psqt(Colour colour, const Board& board) noexcept {
  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    Bitboard piece_squares = board.pieces(piece);
    while (piece_squares != 0) {
      const Square square = Square::pop_first_occupied(piece_squares);
      total += PIECE_SQUARE_TABLES[static_cast<std::size_t>(piece)][square.index()];
    }
  }
  return total;
}

// =============================================================================
// KING SAFETY EVALUATION
// =============================================================================
// Evaluates king safety for one side. Combines:
//   - Pawn shield quality
//   - Open file penalties near king
//   - Enemy piece proximity (attack zone)
//   - Tropism (distance-based threats)
//
// Returns positive values for good safety, negative for danger.
// =============================================================================

int eval_king_safety(Colour colour, const Board& board) noexcept {
  // Find the king
  const Bitboard king_bb = board.pieces(king(colour));
  if (king_bb == 0) {
    return 0; // No king on board (shouldn't happen in normal play)
  }

  const Square king_sq = find_king(colour, board);

  // Calculate game phase for scaling (king safety matters more in middlegame)
  const int phase = calculate_game_phase(board);

  // Check if enemy has a queen (attacks are much more dangerous with queen)
  const Colour enemy = !colour;
  const bool enemy_has_queen = board.pieces(queen(enemy)) != 0;

  int score = 0;

  // 1. Pawn shield evaluation
  score += eval_pawn_shield(colour, king_sq, board);

  // 2. Open file penalties
  score += eval_open_files(colour, king_sq, board);

  // 3. Attack zone evaluation
  score += eval_attack_zone(colour, king_sq, board);

  // 4. Tropism evaluation
  score += eval_tropism(colour, king_sq, board);

  // Scale by game phase (full value in middlegame, reduced in endgame)
  score = (score * phase) / 256;

  // Further reduce if enemy has no queen (attacks much less dangerous)
  if (!enemy_has_queen) {
    score /= NO_QUEEN_SAFETY_DIVISOR;
  }

  return score;
}

// =============================================================================
// MAIN EVALUATION FUNCTION
// =============================================================================
// Returns the position score from the perspective of the side to move:
//   Positive = good for side to move
//   Negative = bad for side to move
//   Zero = equal position
//
// This "side-to-move perspective" convention simplifies the search algorithm:
// the current player always wants to maximize the score, regardless of color.
// =============================================================================

int eval(const Position& pos) noexcept {
  // Compute from White's perspective first
  const int material =
      eval_material(Colour::White, pos.board) - eval_material(Colour::Black, pos.board);
  const int psqt_score = eval_psqt(Colour::White, pos.board) - eval_psqt(Colour::Black, pos.board);
  const int king_safety =
      eval_king_safety(Colour::White, pos.board) - eval_king_safety(Colour::Black, pos.board);
  const int score = material + psqt_score + king_safety;

  // Flip sign if Black is to move (so positive always means "good for me")
  return pos.colour_to_move == Colour::White ? score : -score;
}

} // namespace c3
