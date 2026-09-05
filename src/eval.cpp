// =============================================================================
// STATIC EVALUATION IMPLEMENTATION
// =============================================================================
//
// How a raw position becomes a single centipawn number:
//
//   1. DEAD DRAW?      If neither side can possibly deliver mate (K vs K, K+B
//                      vs K, ...), stop immediately and return 0.
//   2. MATERIAL        Add up piece values for each side, plus a bonus for
//                      owning the bishop pair.
//   3. PIECE-SQUARE    Add a bonus/penalty for every piece depending on the
//                      square it stands on (PSQT).
//   4. TWICE OVER      Steps 2 and 3 are run with two different sets of
//                      numbers: one tuned for the middlegame, one for the
//                      endgame.
//   5. TAPER           Blend the two totals according to the game phase.
//   6. CLAMP + FLIP    Keep the result below mate scores, then flip the sign
//                      if black is to move.
//
// WHY TAPER? A single set of numbers cannot describe both halves of a chess
// game, and the king is the clearest example. With queens on the board the
// king wants to hide on g1 behind its pawns; once the queens are gone the same
// king is a strong piece that belongs in the centre, marching towards enemy
// pawns. A middlegame-only table would keep our king cowering in the corner
// for the whole endgame; an endgame-only table would walk it into a mating net
// on move 12. Tapering computes BOTH scores and mixes them in proportion to
// how much material is left, so the engine's opinion drifts smoothly from one
// to the other as pieces come off—no sudden jump at an arbitrary cutoff.
//
//   score = (middlegame_score * phase + endgame_score * (24 - phase)) / 24
//
// =============================================================================

#include "c3/eval.hpp"

#include <algorithm>
#include <array>

#include "c3/bitboard.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

namespace c3 {
namespace {

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
// piece values in eval.hpp. They were chosen over tuned tables (such as
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
constexpr PieceSquareTable KNIGHT_TABLE = {
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
constexpr PieceSquareTable BISHOP_TABLE = {
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
constexpr PieceSquareTable ROOK_TABLE = {
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
constexpr PieceSquareTable QUEEN_TABLE = {
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
constexpr PieceSquareTable MIDDLEGAME_PAWN_TABLE = {
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
constexpr PieceSquareTable ENDGAME_PAWN_TABLE = {
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
constexpr PieceSquareTable MIDDLEGAME_KING_TABLE = {
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
constexpr PieceSquareTable ENDGAME_KING_TABLE = {
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
constexpr std::array<std::array<PieceSquareTable, 6>, 2> PIECE_SQUARE_BASE = {{
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

constexpr auto PIECE_SQUARE_TABLES = build_psqt();

// The 32 squares of each colour. A bishop can never leave the square colour it
// started on, which is what makes some bishop endings impossible to win.
constexpr Bitboard DARK_SQUARES = 0xAA55'AA55'AA55'AA55ULL;
constexpr Bitboard LIGHT_SQUARES = ~DARK_SQUARES;

constexpr std::size_t phase_index(Phase phase) noexcept {
  return static_cast<std::size_t>(phase);
}

int count_of(const Board& board, Piece piece) noexcept {
  return static_cast<int>(board.count_pieces(piece));
}

// Two bishops are only a "pair" when they cover different square colours—that
// is what makes them able to hit every square on the board. Two bishops on one
// colour (which promotions can produce) still leave half the board untouched,
// so they earn nothing extra.
bool has_bishop_pair(const Board& board, Colour side) noexcept {
  const Bitboard bishops = board.pieces(bishop(side));
  return (bishops & LIGHT_SQUARES) != 0 && (bishops & DARK_SQUARES) != 0;
}

} // namespace

// Non-pawn material left on the board, on the 24-point scale described in eval.hpp.
int game_phase(const Board& board) noexcept {
  int phase = 0;
  for (const auto piece : all_pieces()) {
    phase += PHASE_WEIGHTS[static_cast<std::size_t>(piece) % 6] * count_of(board, piece);
  }
  return std::min(phase, PHASE_MAX);
}

// =============================================================================
// INSUFFICIENT MATERIAL
// =============================================================================
// Some positions cannot be won by anybody: there is not enough material left on
// the board to construct a checkmate, however badly the defender plays. The
// rules of chess declare these positions drawn immediately, and an engine that
// does not know this will happily "win" a piece and then report +330 in a
// position that is stone dead—and, worse, steer towards it.
//
// We recognise the classic dead draws:
//   - K vs K            nothing to mate with
//   - K+N vs K          a lone knight cannot cover the squares a mate needs
//   - K+B vs K          a lone bishop only ever controls half the board
//   - one minor each    K+B vs K+B (same or opposite colours), K+B vs K+N and
//                       K+N vs K+N are all drawn: one minor piece plus a king
//                       simply cannot corner a king that is not helping
//   - K+N+N vs K        two knights cannot FORCE mate: mating positions exist,
//                       but the defender can always sidestep them
//
// The last two entries are "cannot be forced" rather than "cannot happen"—a
// helpless opponent could still walk into mate. Scoring them as draws is a
// deliberate simplification we share with most engines: it costs nothing real
// and stops the search chasing a mate that correct defence always avoids.
//
// Anything with a pawn (it can promote), a rook or a queen is winnable, so those
// positions are rejected up front.
// =============================================================================

bool has_insufficient_material(const Board& board) noexcept {
  const int winning_material = count_of(board, Piece::WP) + count_of(board, Piece::BP) +
                               count_of(board, Piece::WR) + count_of(board, Piece::BR) +
                               count_of(board, Piece::WQ) + count_of(board, Piece::BQ);
  if (winning_material > 0) {
    return false;
  }

  const int white_knights = count_of(board, Piece::WN);
  const int black_knights = count_of(board, Piece::BN);
  const int white_bishops = count_of(board, Piece::WB);
  const int black_bishops = count_of(board, Piece::BB);

  const int white_minors = white_knights + white_bishops;
  const int black_minors = black_knights + black_bishops;

  // Bare kings, or a single minor piece between them.
  if (white_minors + black_minors <= 1) {
    return true;
  }

  // One minor each: bishop against bishop (whatever colours they run on),
  // bishop against knight or knight against knight are all drawn.
  if (white_minors == 1 && black_minors == 1) {
    return true;
  }

  // Two knights against a bare king.
  if (white_knights == 2 && white_bishops == 0 && black_minors == 0) {
    return true;
  }
  if (black_knights == 2 && black_bishops == 0 && white_minors == 0) {
    return true;
  }

  return false;
}

// Total material value for one side, priced in the requested phase's currency.
int eval_material(Colour colour, const Board& board, Phase phase) noexcept {
  const auto& values = phase == Phase::Middlegame ? PIECE_VALUES : PIECE_VALUES_ENDGAME;

  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    total += values[static_cast<std::size_t>(piece)] * count_of(board, piece);
  }

  if (has_bishop_pair(board, colour)) {
    total += phase == Phase::Middlegame ? BISHOP_PAIR_MIDDLEGAME : BISHOP_PAIR_ENDGAME;
  }

  return total;
}

// Sum PSQT bonuses for all pieces of one side.
int eval_psqt(Colour colour, const Board& board, Phase phase) noexcept {
  const auto& tables = PIECE_SQUARE_TABLES[phase_index(phase)];

  int total = 0;
  for (const auto piece : pieces_for(colour)) {
    Bitboard piece_squares = board.pieces(piece);
    while (piece_squares != 0) {
      const Square square = Square::pop_first_occupied(piece_squares);
      total += tables[static_cast<std::size_t>(piece)][square.index()];
    }
  }
  return total;
}

namespace {

// White's lead in one phase's currency: positive means White stands better.
int white_advantage(const Board& board, Phase phase) noexcept {
  const int material =
      eval_material(Colour::White, board, phase) - eval_material(Colour::Black, board, phase);
  const int placement =
      eval_psqt(Colour::White, board, phase) - eval_psqt(Colour::Black, board, phase);
  return material + placement;
}

} // namespace

// =============================================================================
// MAIN EVALUATION FUNCTION
// =============================================================================
// Returns the position score from the perspective of the side to move:
//   Positive = good for side to move
//   Negative = bad for side to move
//   Zero = equal position
//
// This "side-to-move perspective" convention simplifies the search algorithm:
// the current player always wants to maximize the score, regardless of colour.
// Note that the taper and the clamp both happen on the White-relative score and
// only the final sign flip depends on whose turn it is. That keeps evaluation
// exactly antisymmetric: the score of a position is minus the score of the same
// position with the other side to move.
// =============================================================================

int eval(const Position& pos) noexcept {
  if (has_insufficient_material(pos.board)) {
    return CENTIPAWN_DRAW;
  }

  const int phase = game_phase(pos.board);
  const int middlegame = white_advantage(pos.board, Phase::Middlegame);
  const int endgame = white_advantage(pos.board, Phase::Endgame);

  // The taper: full armies (phase 24) return the middlegame score, bare kings
  // (phase 0) return the endgame score, and everything in between is a weighted
  // average that shifts a little further towards the endgame with every trade.
  const int score = (middlegame * phase + endgame * (PHASE_MAX - phase)) / PHASE_MAX;

  // A static score must never be mistaken for a forced mate by the search.
  const int bounded = std::clamp(score, -CENTIPAWN_EVAL_MAX, CENTIPAWN_EVAL_MAX);

  // Flip sign if Black is to move (so positive always means "good for me")
  return pos.colour_to_move == Colour::White ? bounded : -bounded;
}

} // namespace c3
