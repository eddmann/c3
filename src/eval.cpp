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

#include <array>

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
  const int score = material + psqt_score;

  // Flip sign if Black is to move (so positive always means "good for me")
  return pos.colour_to_move == Colour::White ? score : -score;
}

} // namespace c3
