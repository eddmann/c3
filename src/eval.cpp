// =============================================================================
// STATIC EVALUATION IMPLEMENTATION
// =============================================================================
//
// How a raw position becomes a single centipawn number:
//
//   1. DEAD DRAW?      If neither side can possibly deliver mate (K vs K, K+B
//                      vs K, ...), stop immediately and return 0.
//   2. MATERIAL        Add up piece values for each side.
//   3. PIECE-SQUARE    Add a bonus/penalty for every piece depending on the
//                      square it stands on (PSQT).
//   4. BISHOP PAIR     Add a bonus to a side owning two bishops of opposite
//                      square colours.
//   5. POSITIONAL      Pawn structure, king safety, rook placement and
//                      mobility, each scored for one side and then subtracted
//                      the other way round.
//   6. TWICE OVER      Steps 2 to 5 are all run with two different sets of
//                      numbers: one tuned for the middlegame, one for the
//                      endgame.
//   7. TAPER           Blend the two totals according to the game phase.
//   8. CLAMP + FLIP    Keep the result below mate scores, flip the sign if
//                      black is to move, then add the tempo bonus.
//
// Steps 2, 3 and 6 are not actually performed here for material and piece
// squares. They decompose into "what is this piece on this square worth", so
// the Board keeps a running total of them (see eval_terms.hpp) and eval() reads
// it. eval_material() and eval_psqt() below still compute the same quantities
// the slow way: they are the reference implementation the tests—and the Debug
// assertions in position.cpp—measure the running total against.
//
// The step-5 terms cannot be accumulated that way. A pawn is passed because of
// where the ENEMY pawns are; a rook's file is open because of where BOTH sides'
// pawns are; a bishop's mobility changes every time any piece anywhere moves
// onto or off its diagonal. None of that is a property of one piece on one
// square, so all of it is computed from scratch at every call.
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
// The taper is also how the positional terms know when to speak up: king safety
// carries an endgame value of zero and passed pawns carry double their
// middlegame value, so each one fades in or out on its own schedule without a
// single "if endgame" anywhere in the code.
//
// =============================================================================

#include "c3/eval.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "c3/attacks.hpp"
#include "c3/bitboard.hpp"
#include "c3/pawns.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

// ---------------------------------------------------------------------------
// COMPILE-TIME EXPERIMENT HOOK
// ---------------------------------------------------------------------------
// The eval half of the hook documented above SearchContext in search.hpp,
// normalised the same way and for the same reason: a name the command line has
// not defined is given the value 0 here, so the tests below are `#if` on a
// VALUE rather than `#ifdef` on a name. The difference is not pedantry.
// `-DC3_DISABLE_MOBILITY=0` is what somebody writes when they mean "leave
// mobility on", and under `#ifdef` that would have turned mobility OFF—the
// macro is defined, whatever its value. With the normalisation `=0` is on, and
// `=1` or a bare `-DC3_DISABLE_MOBILITY` is off.
// ---------------------------------------------------------------------------
#ifndef C3_DISABLE_MOBILITY
#define C3_DISABLE_MOBILITY 0
#endif
#ifndef C3_DISABLE_KING_ATTACKERS
#define C3_DISABLE_KING_ATTACKERS 0
#endif

namespace c3 {
namespace {

// The piece-square tables, the piece values and the phase weights all live in
// eval_terms.hpp now, because Board keeps a running total of them and therefore
// has to see them too. What is left in this file is how those numbers are
// combined into a single score.

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

// =============================================================================
// THE KING ZONE
// =============================================================================
// The eight squares immediately around a king. This is the patch of board an
// attack has to break into: a piece that merely eyes a distant square is no
// threat, while a piece covering g2 next to a king on g1 is the beginning of a
// mating net. Using the king's own move pattern as the zone is the simplest
// definition that captures that, and it costs one table lookup.
//
// The king's square itself is deliberately NOT in the zone. A piece attacking
// it is giving check, which the search handles far better than a static bonus
// ever could.
// =============================================================================

constexpr std::array<Bitboard, 64> build_king_zones() {
  std::array<Bitboard, 64> zones{};

  for (std::size_t index = 0; index < zones.size(); ++index) {
    const Bitboard king_square = Square::from_index(static_cast<std::uint8_t>(index));

    // The file masks stop a king on the h-file from "covering" the a-file: on a
    // board stored as one 64-bit word, a left shift walks off the edge of a rank
    // straight onto the start of the next one.
    zones[index] = ((king_square & ~FILE_H) << 1) | ((king_square & ~FILE_A) >> 1) |
                   (king_square << 8) | (king_square >> 8) | ((king_square & ~FILE_A) << 7) |
                   ((king_square & ~FILE_H) << 9) | ((king_square & ~FILE_H) >> 7) |
                   ((king_square & ~FILE_A) >> 9);
  }

  return zones;
}

// The king's own file together with its neighbours—the three files a castled
// king lives behind, or two files when the king is on the a- or h-file.
constexpr std::array<Bitboard, 8> build_king_file_windows() {
  std::array<Bitboard, 8> windows{};
  for (std::size_t file = 0; file < windows.size(); ++file) {
    windows[file] = FILE_MASKS[file] | ADJACENT_FILE_MASKS[file];
  }
  return windows;
}

// The squares a pawn shield may occupy: the king's three files, one and two
// ranks in front of it. For a king on its home rank that is exactly ranks 2 and
// 3, the pawns a castled king hides behind; the general form keeps working for
// a king that has stepped up a rank, and quietly runs out of board for one that
// has marched to the far side. Built once, at compile time, because working it
// out with a loop at every node made king safety measurably the second most
// expensive term in the evaluation.
constexpr std::array<std::array<Bitboard, 64>, 2> build_shield_zones() {
  const auto windows = build_king_file_windows();
  std::array<std::array<Bitboard, 64>, 2> zones{};

  for (const auto side : {Colour::White, Colour::Black}) {
    const int forward = side == Colour::White ? 1 : -1;

    for (std::size_t index = 0; index < 64; ++index) {
      const auto king_square = Square::from_index(static_cast<std::uint8_t>(index));

      Bitboard ranks = 0;
      for (int step = 1; step <= 2; ++step) {
        const int rank = king_square.rank() + (forward * step);
        if (rank >= 0 && rank < 8) {
          ranks |= RANK_MASKS[static_cast<std::size_t>(rank)];
        }
      }

      zones[static_cast<std::size_t>(side)][index] = ranks & windows[king_square.file()];
    }
  }

  return zones;
}

constexpr auto KING_ZONES = build_king_zones();
constexpr auto SHIELD_ZONES = build_shield_zones();

} // namespace

// Non-pawn material left on the board, on the 24-point scale described in eval_terms.hpp.
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
//
// That rejection is the fast path, and it is worth writing as one test rather
// than six. Every node of the search asks this question and almost every node
// answers "no, there is plenty of material": ORing the six bitboards together
// and comparing against zero settles those in a handful of instructions, and
// only the genuinely bare boards go on to count anything.
// =============================================================================

bool has_insufficient_material(const Board& board) noexcept {
  const Bitboard winning_material = board.pieces(Piece::WP) | board.pieces(Piece::BP) |
                                    board.pieces(Piece::WR) | board.pieces(Piece::BR) |
                                    board.pieces(Piece::WQ) | board.pieces(Piece::BQ);
  if (winning_material != 0) {
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

// =============================================================================
// PAWN STRUCTURE
// =============================================================================
// One pass over this side's pawns answering three questions per pawn, each of
// them a single AND against a precomputed mask (see pawns.hpp):
//
//   PASSED    is the "no enemy pawn may stand here" zone ahead of it empty,
//             AND is this the front pawn of its file?
//   DOUBLED   is there a friendly pawn further up the same file?
//   ISOLATED  is there no friendly pawn on either neighbouring file?
//
// A pawn can be several of these at once, and the scores simply add up: an
// isolated passer really is both a long-term liability and a running threat.
//
// The extra clause on PASSED matters. Two white pawns on c4 and c5 with no
// black pawn in the way are ONE passer, not two: the rear pawn can never
// overtake the one in front of it, so only the front pawn is a promotion
// threat. Without the clause a doubled pair would collect the bonus twice and
// the engine would happily double its pawns to earn it.
//
// Charging the doubled penalty to the pawn that has another one AHEAD of it,
// rather than counting pawns per file, is what makes three pawns on a file cost
// two penalties without a second loop—and it is the same test, so the rear pawn
// pays the penalty and misses the bonus in one go.
// =============================================================================

PhaseScore eval_pawn_structure(Colour side, const Board& board) noexcept {
  const auto side_index = static_cast<std::size_t>(side);
  const Bitboard friendly_pawns = board.pieces(pawn(side));
  const Bitboard enemy_pawns = board.pieces(pawn(!side));

  PhaseScore score;

  Bitboard remaining = friendly_pawns;
  while (remaining != 0) {
    const Square square = Square::pop_first_occupied(remaining);
    const auto index = square.index();

    const bool blocked_by_own_pawn = (FORWARD_FILE_MASKS[side_index][index] & friendly_pawns) != 0;

    if (!blocked_by_own_pawn && (PASSED_PAWN_MASKS[side_index][index] & enemy_pawns) == 0) {
      const auto rank = relative_rank(square, side);
      score.middlegame += PASSED_PAWN_MIDDLEGAME[rank];
      score.endgame += PASSED_PAWN_ENDGAME[rank];
    }

    if (blocked_by_own_pawn) {
      score += DOUBLED_PAWN_PENALTY;
    }

    if ((ADJACENT_FILE_MASKS[square.file()] & friendly_pawns) == 0) {
      score += ISOLATED_PAWN_PENALTY;
    }
  }

  return score;
}

// =============================================================================
// ROOKS
// =============================================================================
// Rooks care about two things: the file they stand on, and whether they have
// reached the enemy's second rank. Both are answered with file and rank masks.
//
// The seventh-rank test asks whether there is anything there worth attacking
// BEFORE paying the bonus. A rook on an empty seventh rank in a position where
// the enemy king has long since walked to the centre is just a rook on a good
// file, and paying it a bonus anyway would teach the engine to shuffle rooks
// onto the seventh for no reason.
// =============================================================================

PhaseScore eval_rooks(Colour side, const Board& board) noexcept {
  const Bitboard friendly_pawns = board.pieces(pawn(side));
  const Bitboard enemy_pawns = board.pieces(pawn(!side));

  // "Seventh" and "eighth" from this side's point of view: ranks 7 and 8 for
  // White, ranks 2 and 1 for Black.
  const std::size_t seventh_rank = side == Colour::White ? 6 : 1;
  const std::size_t eighth_rank = side == Colour::White ? 7 : 0;

  const bool seventh_has_targets = (board.pieces(king(!side)) & RANK_MASKS[eighth_rank]) != 0 ||
                                   (enemy_pawns & RANK_MASKS[seventh_rank]) != 0;

  PhaseScore score;

  Bitboard rooks = board.pieces(rook(side));
  while (rooks != 0) {
    const Square square = Square::pop_first_occupied(rooks);
    const Bitboard file = FILE_MASKS[square.file()];

    if ((file & friendly_pawns) == 0) {
      score += (file & enemy_pawns) == 0 ? ROOK_OPEN_FILE : ROOK_SEMI_OPEN_FILE;
    }

    if (seventh_has_targets && square.rank() == seventh_rank) {
      score += ROOK_ON_SEVENTH;
    }
  }

  return score;
}

// =============================================================================
// PIECE ACTIVITY: MOBILITY AND PRESSURE ON THE ENEMY KING
// =============================================================================
// Two terms, one loop, for a reason worth spelling out.
//
// Mobility needs the set of squares each knight, bishop, rook and queen attacks.
// The king-safety attacker count needs exactly the same sets, asked a different
// question: does this piece touch the squares around the enemy king? Those
// attack sets are the expensive part of the whole evaluation—two magic-bitboard
// probes for every queen—so computing them once and answering both questions is
// worth more than the tidiness of two separate functions that each walk the
// pieces. The alternative was measured, and it is roughly twice the work for the
// same answers.
//
// Note which squares count as mobility. Squares holding a FRIENDLY piece are
// excluded (the piece cannot go there); squares holding an ENEMY piece are
// included (it can go there, by capturing). For knights and bishops we also
// exclude squares covered by an enemy pawn, because a minor piece standing on
// one is simply lost—counting them would praise a knight for the squares it
// dare not use.
//
// The attacker count uses the RAW attack set, before any of that filtering. A
// rook bearing down on g2 is pressure on a king on g1 whether or not g2 is
// occupied by one of the rook's own pieces: the defender still has to keep
// something there. That is deliberate, and it is why the two questions read
// different bitboards from the same lookup.
// =============================================================================

namespace {

// The colour is a TEMPLATE parameter rather than an ordinary argument, and that
// is a performance decision rather than a stylistic one. It makes knight(Side),
// bishop(Side) and the rest compile-time constants, so that UNDER LTO—where
// attacks_for, which lives in movegen.cpp, can be inlined here—its switch over
// the twelve piece types folds down to the single branch each call can actually
// reach. Measured on the Release build that is worth about a fifth of this
// function's running time; in a Debug build, with nothing inlined across
// translation units, it buys nothing and costs nothing but one wrapper below.
template <Colour Side> PieceActivity piece_activity(const Board& board) noexcept {
  const Bitboard friendly = board.pieces_by_colour(Side);
  const Bitboard enemy_pawn_attacks = pawn_attack_span(board.pieces(pawn(!Side)), !Side);

  // A board with no enemy king (unit tests and analysis positions often have
  // none) simply has no king zone to attack.
  const Bitboard enemy_king = board.pieces(king(!Side));
  const Bitboard king_zone =
      enemy_king == 0 ? Bitboard{0} : KING_ZONES[Square::first_occupied(enemy_king).index()];

  PieceActivity activity;

  const auto tally = [&](Bitboard attacks, Bitboard reachable, const PhaseScore& weight) {
    const int destinations = std::popcount(attacks & reachable);
    activity.mobility.middlegame += destinations * weight.middlegame;
    activity.mobility.endgame += destinations * weight.endgame;

    if ((attacks & king_zone) != 0) {
      ++activity.king_zone_attackers;
    }
  };

  const Bitboard minor_squares = ~(friendly | enemy_pawn_attacks);
  const Bitboard major_squares = ~friendly;

  Bitboard knights = board.pieces(knight(Side));
  while (knights != 0) {
    const Square from = Square::pop_first_occupied(knights);
    tally(attacks_for(knight(Side), from, board), minor_squares, MOBILITY_WEIGHTS[1]);
  }

  Bitboard bishops = board.pieces(bishop(Side));
  while (bishops != 0) {
    const Square from = Square::pop_first_occupied(bishops);
    tally(attacks_for(bishop(Side), from, board), minor_squares, MOBILITY_WEIGHTS[2]);
  }

  Bitboard rooks = board.pieces(rook(Side));
  while (rooks != 0) {
    const Square from = Square::pop_first_occupied(rooks);
    tally(attacks_for(rook(Side), from, board), major_squares, MOBILITY_WEIGHTS[3]);
  }

  Bitboard queens = board.pieces(queen(Side));
  while (queens != 0) {
    const Square from = Square::pop_first_occupied(queens);
    tally(attacks_for(queen(Side), from, board), major_squares, MOBILITY_WEIGHTS[4]);
  }

  return activity;
}

} // namespace

PieceActivity eval_piece_activity(Colour side, const Board& board) noexcept {
  return side == Colour::White ? piece_activity<Colour::White>(board)
                               : piece_activity<Colour::Black>(board);
}

// =============================================================================
// KING SAFETY
// =============================================================================
// Three middlegame-only readings of how comfortable a king is: how many of its
// own pawns still stand in front of it, how many of the files beside it are
// clear for enemy heavy pieces, and how many enemy pieces are already looking
// at the squares around it.
//
// The endgame half of the result is left at zero on purpose; see the discussion
// above KING_SHIELD_PAWN_MIDDLEGAME in eval.hpp. The taper does the rest.
// =============================================================================

PhaseScore eval_king_safety(Colour side, const Board& board, int enemy_attackers) noexcept {
  const Bitboard king_board = board.pieces(king(side));
  if (king_board == 0) {
    return {};
  }

  const Square king_square = Square::first_occupied(king_board);
  const Bitboard friendly_pawns = board.pieces(pawn(side));
  const Bitboard enemy_pawns = board.pieces(pawn(!side));

  int middlegame = 0;

  // The count is a single cap on the total, not a cap per file: six pawns
  // crammed into the shield zone score the same as the three a castled king
  // actually wants, because the fourth pawn is not more shelter—it is a pawn
  // standing behind another pawn.
  const Bitboard shield = SHIELD_ZONES[static_cast<std::size_t>(side)][king_square.index()];
  const int shield_pawns = std::min(std::popcount(friendly_pawns & shield), KING_SHIELD_MAX_PAWNS);
  middlegame += shield_pawns * KING_SHIELD_PAWN_MIDDLEGAME;

  const int king_file = king_square.file();
  for (int file = std::max(0, king_file - 1); file <= std::min(7, king_file + 1); ++file) {
    const Bitboard file_mask = FILE_MASKS[static_cast<std::size_t>(file)];
    if ((file_mask & friendly_pawns) != 0) {
      continue; // Our own pawn still blocks the file.
    }

    middlegame += KING_SEMI_OPEN_FILE_MIDDLEGAME;
    if ((file_mask & enemy_pawns) == 0) {
      middlegame += KING_FULLY_OPEN_FILE_EXTRA_MIDDLEGAME;
    }
  }

  middlegame += std::min(enemy_attackers, KING_ZONE_MAX_ATTACKERS) * KING_ZONE_ATTACKER_MIDDLEGAME;

  return PhaseScore{.middlegame = middlegame, .endgame = 0};
}

namespace {

// The bishop-pair bonus, as a signed White-minus-Black pair. This is the one
// material term the accumulator cannot carry: it belongs to a side owning two
// bishops on opposite square colours, not to either bishop individually, so
// there is no per-piece value to add and subtract.
//
// Who owns a pair is a fact about the board, not about the phase, so it is
// established once and then priced twice. Asking the question per phase would
// run the same four bitboard tests over again for an answer that cannot have
// changed in between.
PhaseScore bishop_pair_advantage(const Board& board) noexcept {
  const int white = has_bishop_pair(board, Colour::White) ? 1 : 0;
  const int black = has_bishop_pair(board, Colour::Black) ? 1 : 0;
  const int pairs = white - black;

  return PhaseScore{
      .middlegame = pairs * BISHOP_PAIR_MIDDLEGAME,
      .endgame = pairs * BISHOP_PAIR_ENDGAME,
  };
}

// Everything the Board already knows, as a White-minus-Black pair: the running
// material and piece-square totals, plus the bishop pair the accumulator cannot
// carry. Nothing here walks the pieces.
PhaseScore accumulated_advantage(const Board& board) noexcept {
  const auto& accumulator = board.accumulator();

  PhaseScore advantage{
      .middlegame = accumulator.middlegame(Colour::White) - accumulator.middlegame(Colour::Black),
      .endgame = accumulator.endgame(Colour::White) - accumulator.endgame(Colour::Black),
  };
  advantage += bishop_pair_advantage(board);
  return advantage;
}

// The taper: full armies (phase 24) return the middlegame score, bare kings
// (phase 0) return the endgame score, and everything in between is a weighted
// average that shifts a little further towards the endgame with every trade.
int taper(const PhaseScore& score, int phase) noexcept {
  return ((score.middlegame * phase) + (score.endgame * (PHASE_MAX - phase))) / PHASE_MAX;
}

// The tempo bonus is added AFTER the clamp, so the clamp has to leave room for
// it: without this the largest possible static score plus the tempo bonus would
// tip over CENTIPAWN_MATE_THRESHOLD and be read as a forced mate.
constexpr int TAPERED_SCORE_LIMIT = CENTIPAWN_EVAL_MAX - TEMPO_BONUS;

} // namespace

// Layer 1 alone, as eval() used to compute it: material, piece squares and the
// bishop pair, tapered, clamped and flipped. See the declaration in eval.hpp for
// why this is worth keeping around.
int eval_material_and_psqt(const Position& pos) noexcept {
  if (has_insufficient_material(pos.board)) {
    return CENTIPAWN_DRAW;
  }

  const int phase = std::min(pos.board.accumulator().phase(), PHASE_MAX);
  const int score = taper(accumulated_advantage(pos.board), phase);
  const int bounded = std::clamp(score, -CENTIPAWN_EVAL_MAX, CENTIPAWN_EVAL_MAX);

  return pos.colour_to_move == Colour::White ? bounded : -bounded;
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
// the current player always wants to maximize the score, regardless of colour.
//
// Every term is scored for White and for Black and the two are subtracted, so
// the whole White-relative score is antisymmetric by construction: mirror the
// colours and it negates. The taper and the clamp are applied to that
// White-relative figure, and only then does the sign flip for the side to move.
//
// The tempo bonus is the one deliberate exception. It belongs to whoever is on
// move, not to the position, so it is added after the flip—which means eval() is
// no longer exactly antisymmetric. What holds instead is
//
//   eval(position) + eval(same position, other side to move) == 2 * TEMPO_BONUS
//
// and that is what the tests check. Dead draws return before any of this: a
// position nobody can win is worth exactly zero, tempo included.
// =============================================================================

int eval(const Position& pos) noexcept {
  const Board& board = pos.board;

  if (has_insufficient_material(board)) {
    return CENTIPAWN_DRAW;
  }

  const int phase = std::min(board.accumulator().phase(), PHASE_MAX);

  PhaseScore score = accumulated_advantage(board);

  // Mobility and the king-zone attacker counts come out of the same pass, and
  // each side's attackers are what threaten the OTHER side's king.
  //
  // COMPILE-TIME EXPERIMENT HOOK, the counterpart of the C3_DISABLE_* switches
  // in search.hpp: `-DC3_DISABLE_MOBILITY` drops the mobility term from the
  // score and `-DC3_DISABLE_KING_ATTACKERS` drops the attacker count king
  // safety reads, so an engine-versus-engine match can price either one on its
  // own. Both are read as VALUES—see the normalisation at the top of this
  // file—so `=0` means "leave it on". The default build defines neither macro
  // and is unaffected; nothing on the UCI path can reach these, because only
  // the compiler invocation sets them.
  //
  // The two terms are separable in the score but NOT in the work: they are read
  // off the same attack bitboards, and those bitboards are the expensive part
  // of this function—roughly half of an evaluation. So disabling one of them
  // buys the term's strength contribution and none of the time back, and only
  // disabling BOTH skips the pass and buys the time. That is why the pass below
  // is skipped only when neither answer is wanted.
  const auto activity_of = [&]([[maybe_unused]] Colour side) {
#if C3_DISABLE_MOBILITY && C3_DISABLE_KING_ATTACKERS
    return PieceActivity{};
#else
    PieceActivity activity = eval_piece_activity(side, board);
#if C3_DISABLE_MOBILITY
    activity.mobility = {};
#endif
#if C3_DISABLE_KING_ATTACKERS
    activity.king_zone_attackers = 0;
#endif
    return activity;
#endif
  };

  const auto white_activity = activity_of(Colour::White);
  const auto black_activity = activity_of(Colour::Black);

  score += white_activity.mobility;
  score -= black_activity.mobility;

  score += eval_pawn_structure(Colour::White, board);
  score -= eval_pawn_structure(Colour::Black, board);

  score += eval_rooks(Colour::White, board);
  score -= eval_rooks(Colour::Black, board);

  score += eval_king_safety(Colour::White, board, black_activity.king_zone_attackers);
  score -= eval_king_safety(Colour::Black, board, white_activity.king_zone_attackers);

  // A static score must never be mistaken for a forced mate by the search.
  const int bounded = std::clamp(taper(score, phase), -TAPERED_SCORE_LIMIT, TAPERED_SCORE_LIMIT);

  // Flip sign if Black is to move (so positive always means "good for me"),
  // then pay whoever that is for holding the move.
  const int for_side_to_move = pos.colour_to_move == Colour::White ? bounded : -bounded;
  return for_side_to_move + TEMPO_BONUS;
}

} // namespace c3
