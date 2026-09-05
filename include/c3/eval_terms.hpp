#pragma once

// =============================================================================
// EVALUATION TERMS: The Numbers, and the Running Total the Board Keeps
// =============================================================================
//
// This header holds the raw numbers the evaluation is built from—piece values,
// piece-square tables and game-phase weights—together with the small
// accumulator that keeps a running total of them.
//
// WHY A SEPARATE HEADER? Because the Board is the one place where pieces are
// actually placed and removed, and the Board is therefore the one place that
// can keep the running total honest. Board must see these numbers, and the
// evaluation in eval.hpp must see them too; putting them here lets both include
// the same definitions without either depending on the other.
//
// WHY A RUNNING TOTAL? A naive evaluation walks all 32 piece lists at every
// node of the search, and does it twice because the score is tapered between a
// middlegame and an endgame reading. The search calls eval() millions of times
// per second, so that walk is pure repeated work: a move changes at most four
// squares (a capture-promotion, or a castle moving king and rook), and every
// other piece contributes exactly what it contributed a moment ago.
//
// So instead of recomputing, we maintain. Every piece placement adds its terms
// to a total; every removal subtracts them. The totals are then correct by
// construction, and eval() only has to read three integers instead of walking
// the board. This is the same trick, and the same reasoning, as the incremental
// Zobrist key in position.cpp—see zobrist.hpp for that story.
//
// WHAT IS *NOT* ACCUMULATED. Two evaluation terms are deliberately left out:
//
//   - the bishop pair bonus, which is not a property of any single piece but of
//     a side owning two bishops on opposite square colours, and
//   - the insufficient-material draw, which is a verdict on the whole position.
//
// Both are cheap to test on demand and neither decomposes into "what this piece
// on this square is worth", which is the only shape an accumulator can hold.
//
// =============================================================================

#include <array>
#include <cstddef>

#include "c3/colour.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {

// =============================================================================
// GAME PHASE
// =============================================================================
// "Phase" answers the question: is this still a middlegame, or an endgame?
//
// We answer it by counting the non-pawn material left on the board, using the
// classic 24-point scale (knight 1, bishop 1, rook 2, queen 4, both colours
// summed). A full opening army is 4 minors + 4 rooks + 2 queens = 24 points; a
// bare-kings endgame is 0. Pawns are deliberately ignored: trading every piece
// but keeping all the pawns still gives you a king-and-pawn ENDGAME.
//
// The count is capped at PHASE_MAX because promotions can create more material
// than the game started with.
// =============================================================================

inline constexpr int PHASE_MAX = 24;
inline constexpr std::array<int, 6> PHASE_WEIGHTS = {
    0, // Pawn: pawns do not decide whether we are in an endgame
    1, // Knight
    1, // Bishop
    2, // Rook
    4, // Queen
    0, // King: always present, so it carries no information
};

// The two sets of numbers a tapered evaluation blends between.
enum class Phase : std::size_t { Middlegame, Endgame };

// =============================================================================
// MATERIAL VALUES
// =============================================================================
// Traditional piece values, in the "simplified evaluation function" flavour
// (Tomasz Michniewski, Chess Programming Wiki):
//   Pawn   = 100 (the unit of measurement)
//   Knight = 320 (≈3 pawns, good in closed positions)
//   Bishop = 330 (a shade above the knight, so the engine keeps the pair)
//   Rook   = 500 (≈5 pawns, dominates open files)
//   Queen  = 900 (≈9 pawns, strongest piece)
//   King   = 0   (priceless, but doesn't contribute to material count)
//
// PIECE_VALUES holds the middlegame numbers. The search also uses this array
// for MVV-LVA capture ordering ("most valuable victim, least valuable
// attacker"), which is why it keeps its 12-entry shape.
//
// PIECE_VALUES_ENDGAME is our own addition—the published set has a single list
// of values—and nudges the numbers for a bare board: a pawn is worth more once
// it has a realistic chance of queening, and the long-range pieces gain a little
// as the board opens up. Minor pieces stay put; they are the yardstick
// everything else is measured against.
// =============================================================================

inline constexpr std::array<int, 12> PIECE_VALUES = {
    100, 320, 330, 500, 900, 0, // White: P, N, B, R, Q, K
    100, 320, 330, 500, 900, 0  // Black: P, N, B, R, Q, K
};

inline constexpr std::array<int, 12> PIECE_VALUES_ENDGAME = {
    120, 320, 330, 520, 940, 0, // White: P, N, B, R, Q, K
    120, 320, 330, 520, 940, 0  // Black: P, N, B, R, Q, K
};

namespace detail {

// =============================================================================
// PIECE-SQUARE TABLES
// =============================================================================
// Each table has 64 values (one per square) giving a bonus/penalty in centipawns
// for having that piece on that square. Tables are written out as you would see
// the board from White's side: the FIRST row is rank 8 and the LAST row is
// rank 1. Black's view is produced by flipping vertically (see RANK_FLIP_TABLE).
//
// Positive values = good square for this piece
// Negative values = bad square for this piece
//
// SOURCE: these are the "Simplified Evaluation Function" tables by Tomasz
// Michniewski (Chess Programming Wiki), which pair with the 100/320/330/500/900
// piece values above. They were chosen over tuned tables (such as
// PeSTO's) because every number in them is explainable to a human: the knight
// table is literally "how many squares does a knight cover from here", and the
// king tables spell out castling. Three deliberate deviations from the
// published set:
//
//   - The published queen table has three rows whose right half does not mirror
//     its left half, an artifact of hand-typing. We mirror the left half onto
//     the right so that a position and its left-right mirror image—two
//     positions that are strategically identical—always score the same.
//   - The published set only distinguishes middlegame from endgame for the
//     king. We keep that for the pieces (a knight likes the centre whenever it
//     is on the board) but add an endgame pawn table of our own, because a
//     pawn's job changes completely once the pieces are gone: it stops fighting
//     for the centre and starts running for promotion.
//   - The published set has one list of piece values. PIECE_VALUES_ENDGAME in
//     eval.hpp is ours too, so that the taper can reprice a pawn as the board
//     empties.
// =============================================================================

using PieceSquareTable = std::array<int, 64>;

// -----------------------------------------------------------------------------
// Phase-independent tables: a knight's reach, a bishop's diagonals, a rook's
// files and a queen's centralisation do not change when the queens come off, so
// these four tables are declared once and used by BOTH phases below. Only the
// pawn and the king get a second table, because only their jobs change.
// -----------------------------------------------------------------------------

// clang-format off

  // KNIGHT: "a knight on the rim is dim". The numbers track how many squares a
  // knight attacks from each square: 8 in the centre, 4 on an edge, 2 in a corner.
  inline constexpr PieceSquareTable KNIGHT_TABLE = {
      -50, -40, -30, -30, -30, -30, -40, -50,
      -40, -20,   0,   0,   0,   0, -20, -40,
      -30,   0,  10,  15,  15,  10,   0, -30,
      -30,   5,  15,  20,  20,  15,   5, -30,
      -30,   0,  15,  20,  20,  15,   0, -30,
      -30,   5,  10,  15,  15,  10,   5, -30,
      -40, -20,   0,   5,   5,   0, -20, -40,
      -50, -40, -30, -30, -30, -30, -40, -50,
  };

  // BISHOP: long diagonals are everything; the edges halve a bishop's reach.
  // The +10 row on rank 3 rewards the fianchetto and the classic Bd3/Bc4 posts.
  inline constexpr PieceSquareTable BISHOP_TABLE = {
      -20, -10, -10, -10, -10, -10, -10, -20,
      -10,   0,   0,   0,   0,   0,   0, -10,
      -10,   0,   5,  10,  10,   5,   0, -10,
      -10,   5,   5,  10,  10,   5,   5, -10,
      -10,   0,  10,  10,  10,  10,   0, -10,
      -10,  10,  10,  10,  10,  10,  10, -10,
      -10,   5,   0,   0,   0,   0,   5, -10,
      -20, -10, -10, -10, -10, -10, -10, -20,
  };

  // ROOK: the 7th rank is the prize (it attacks pawns and cages the king).
  // The small d1/e1 bonus nudges rooks towards the central files after castling.
  inline constexpr PieceSquareTable ROOK_TABLE = {
       0,   0,   0,   0,   0,   0,   0,   0,
       5,  10,  10,  10,  10,  10,  10,   5,
      -5,   0,   0,   0,   0,   0,   0,  -5,
      -5,   0,   0,   0,   0,   0,   0,  -5,
      -5,   0,   0,   0,   0,   0,   0,  -5,
      -5,   0,   0,   0,   0,   0,   0,  -5,
      -5,   0,   0,   0,   0,   0,   0,  -5,
       0,   0,   0,   5,   5,   0,   0,   0,
  };

  // QUEEN: mildly prefers the centre, and mostly just avoids the edges and
  // corners where she is easy to harass with tempo.
  inline constexpr PieceSquareTable QUEEN_TABLE = {
      -20, -10, -10,  -5,  -5, -10, -10, -20,
      -10,   0,   0,   0,   0,   0,   0, -10,
      -10,   0,   5,   5,   5,   5,   0, -10,
       -5,   0,   5,   5,   5,   5,   0,  -5,
        0,   0,   5,   5,   5,   5,   0,   0,
      -10,   5,   5,   5,   5,   5,   5, -10,
      -10,   0,   5,   0,   0,   5,   0, -10,
      -20, -10, -10,  -5,  -5, -10, -10, -20,
  };

  // PAWN (middlegame): reward advancement, reward the big centre (d4/e4), and
  // discourage pushing the pawns in front of a castled king (the -20s on d2/e2
  // mean the engine advances its centre pawns rather than its own shelter).
  inline constexpr PieceSquareTable MIDDLEGAME_PAWN_TABLE = {
       0,   0,   0,   0,   0,   0,   0,   0,
      50,  50,  50,  50,  50,  50,  50,  50,
      10,  10,  20,  30,  30,  20,  10,  10,
       5,   5,  10,  25,  25,  10,   5,   5,
       0,   0,   0,  20,  20,   0,   0,   0,
       5,  -5, -10,   0,   0, -10,  -5,   5,
       5,  10,  10, -20, -20,  10,  10,   5,
       0,   0,   0,   0,   0,   0,   0,   0,
  };

  // PAWN (endgame): with the pieces gone, a pawn's only ambition is to queen, so
  // the bonus depends on the rank alone. Which file it stands on no longer
  // matters, and there is no castled king left whose shelter must be kept.
  inline constexpr PieceSquareTable ENDGAME_PAWN_TABLE = {
        0,   0,   0,   0,   0,   0,   0,   0,
       80,  80,  80,  80,  80,  80,  80,  80,
       50,  50,  50,  50,  50,  50,  50,  50,
       30,  30,  30,  30,  30,  30,  30,  30,
       15,  15,  15,  15,  15,  15,  15,  15,
        5,   5,   5,   5,   5,   5,   5,   5,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
  };

  // KING (middlegame): with enemy pieces swarming, the king wants a roof over its
  // head. Every square from rank 3 upwards is heavily punished; the +30 on b1/g1
  // and +20 on the corner squares are where castling actually lands.
  inline constexpr PieceSquareTable MIDDLEGAME_KING_TABLE = {
      -30, -40, -40, -50, -50, -40, -40, -30,
      -30, -40, -40, -50, -50, -40, -40, -30,
      -30, -40, -40, -50, -50, -40, -40, -30,
      -30, -40, -40, -50, -50, -40, -40, -30,
      -20, -30, -30, -40, -40, -30, -30, -20,
      -10, -20, -20, -20, -20, -20, -20, -10,
       20,  20,   0,   0,   0,   0,  20,  20,
       20,  30,  10,   0,   0,  10,  30,  20,
  };

  // KING (endgame): the opposite advice to the middlegame table. With no enemy
  // queen left to fear, a centralised king supports its own pawns, blocks the
  // enemy king and shepherds passers home, while a king stuck on h1 is simply out
  // of play. Driving the defending king towards a corner is also exactly how K+Q
  // and K+R deliver mate, so the same table teaches both sides of that ending.
  inline constexpr PieceSquareTable ENDGAME_KING_TABLE = {
      -50, -40, -30, -20, -20, -30, -40, -50,
      -30, -20, -10,   0,   0, -10, -20, -30,
      -30, -10,  20,  30,  30,  20, -10, -30,
      -30, -10,  30,  40,  40,  30, -10, -30,
      -30, -10,  30,  40,  40,  30, -10, -30,
      -30, -10,  20,  30,  30,  20, -10, -30,
      -30, -30,   0,   0,   0,   0, -30, -30,
      -50, -30, -30, -30, -30, -30, -30, -50,
  };
// clang-format on

// The two sets the taper blends between, in piece order (P, N, B, R, Q, K).
// Four of the six entries are literally the same table in both rows, which is
// what makes "these pieces are phase-independent" a fact rather than a promise.
inline constexpr std::array<std::array<PieceSquareTable, 6>, 2> PIECE_SQUARE_BASE = {{
    // Middlegame
    {MIDDLEGAME_PAWN_TABLE, KNIGHT_TABLE, BISHOP_TABLE, ROOK_TABLE, QUEEN_TABLE,
     MIDDLEGAME_KING_TABLE},
    // Endgame
    {ENDGAME_PAWN_TABLE, KNIGHT_TABLE, BISHOP_TABLE, ROOK_TABLE, QUEEN_TABLE, ENDGAME_KING_TABLE},
}};

// =============================================================================
// RANK FLIP TABLE
// =============================================================================
// PSQTs above are written rank-8-first, so reading them in memory order visits
// A8, B8, ... H1, while square indices run A1, B1, ... H8. This table performs
// that translation for White and, for Black, the additional vertical mirror
// that turns "White's rank 7" into "Black's rank 2":
//   - For White: square index → the table slot holding that square's value
//   - For Black: square index → the vertically mirrored slot (A1↔A8, B2↔B7, ...)
// =============================================================================

// clang-format off
  inline constexpr std::array<std::array<std::size_t, 64>, 2> RANK_FLIP_TABLE = {{
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
      // Black: the identity mapping. Read in memory order the tables already run
      // from Black's home rank outwards, so slot N is exactly what a black piece
      // standing on square N deserves—no translation needed.
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

// Fold the base tables into a lookup indexed by [phase][piece][square] at
// compile time, so the rank flipping above costs nothing at run time.
//
// That is a small saving next to the real cost: eval() walks both piece lists
// twice, once per phase, so a tapered evaluation is roughly twice the work of a
// single-phase one. We take that hit deliberately—reading two independent
// component scores is much easier to follow than the usual trick of updating a
// packed (middlegame, endgame) pair incrementally inside make_move, which is
// where a later performance pass would take this.
constexpr std::array<std::array<PieceSquareTable, 12>, 2> build_psqt() {
  std::array<std::array<PieceSquareTable, 12>, 2> psqt{};

  for (std::size_t phase = 0; phase < psqt.size(); ++phase) {
    for (const auto piece : all_pieces()) {
      const auto colour_index = static_cast<std::size_t>(colour(piece));
      const auto piece_index = static_cast<std::size_t>(piece);
      const auto white_piece_index = piece_index % 6;

      for (std::size_t square = 0; square < 64; ++square) {
        const auto mapped_square = RANK_FLIP_TABLE[colour_index][square];
        psqt[phase][piece_index][square] =
            PIECE_SQUARE_BASE[phase][white_piece_index][mapped_square];
      }
    }
  }

  return psqt;
}

inline constexpr auto PIECE_SQUARE_TABLES = build_psqt();
} // namespace detail

// The piece-square tables, indexed [phase][piece][square], already flipped for
// each colour. eval_psqt() reads them directly; EVAL_TERMS below folds them
// together with the piece values.
inline constexpr auto PIECE_SQUARE_TABLES = detail::build_psqt();

// =============================================================================
// PER-PIECE EVALUATION TERMS
// =============================================================================
// Everything one piece standing on one square contributes, gathered into a
// single struct so that placing or removing it is one table lookup rather than
// three. The material value and the piece-square bonus are summed here because
// the accumulator has no reason to keep them apart—only their total is ever
// read back.
//
// The phase weight rides along in the same struct purely for locality: it is
// indexed by piece alone, but keeping it here means a placement touches one
// cache line instead of two.
// =============================================================================

struct EvalTerm {
  int middlegame{}; // material value + middlegame piece-square bonus
  int endgame{};    // material value + endgame piece-square bonus
  int phase{};      // contribution to the 24-point game phase scale
};

constexpr std::array<std::array<EvalTerm, 64>, 12> build_eval_terms() {
  std::array<std::array<EvalTerm, 64>, 12> terms{};

  for (const auto piece : all_pieces()) {
    const auto piece_index = static_cast<std::size_t>(piece);
    const auto middlegame_index = static_cast<std::size_t>(Phase::Middlegame);
    const auto endgame_index = static_cast<std::size_t>(Phase::Endgame);

    for (std::size_t square = 0; square < 64; ++square) {
      terms[piece_index][square] = EvalTerm{
          .middlegame = PIECE_VALUES[piece_index] +
                        PIECE_SQUARE_TABLES[middlegame_index][piece_index][square],
          .endgame = PIECE_VALUES_ENDGAME[piece_index] +
                     PIECE_SQUARE_TABLES[endgame_index][piece_index][square],
          .phase = PHASE_WEIGHTS[piece_index % 6],
      };
    }
  }

  return terms;
}

inline constexpr auto EVAL_TERMS = build_eval_terms();

// =============================================================================
// THE ACCUMULATOR
// =============================================================================
// Three running totals per colour: what this side's pieces are worth by the
// middlegame numbers, what they are worth by the endgame numbers, and how much
// game phase they account for. Board::put_piece and Board::remove_piece are the
// only callers—they are the only two operations that can change any of these.
//
// The phase total is stored UNCAPPED. Promotions can push the real count past
// PHASE_MAX, and clamping on the way in would make add() and remove() stop
// being exact inverses of each other: a total that saturated on the way up
// would not come back down correctly. eval() applies the cap when it reads.
// =============================================================================

class EvalAccumulator {
public:
  constexpr void add(Piece piece, Square square) noexcept { apply(piece, square, 1); }

  constexpr void remove(Piece piece, Square square) noexcept { apply(piece, square, -1); }

  [[nodiscard]] constexpr int middlegame(Colour side) const noexcept {
    return middlegame_[static_cast<std::size_t>(side)];
  }

  [[nodiscard]] constexpr int endgame(Colour side) const noexcept {
    return endgame_[static_cast<std::size_t>(side)];
  }

  // Uncapped: see the note above. Callers that want the 0..PHASE_MAX scale
  // should use game_phase() in eval.hpp.
  [[nodiscard]] constexpr int phase() const noexcept { return phase_; }

  [[nodiscard]] constexpr bool operator==(const EvalAccumulator&) const noexcept = default;

private:
  constexpr void apply(Piece piece, Square square, int sign) noexcept {
    const auto& term = EVAL_TERMS[static_cast<std::size_t>(piece)][square.index()];
    const auto side = static_cast<std::size_t>(colour(piece));

    middlegame_[side] += sign * term.middlegame;
    endgame_[side] += sign * term.endgame;
    phase_ += sign * term.phase;
  }

  std::array<int, 2> middlegame_{};
  std::array<int, 2> endgame_{};
  int phase_{};
};

} // namespace c3
