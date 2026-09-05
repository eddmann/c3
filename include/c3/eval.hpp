#pragma once

// =============================================================================
// POSITION EVALUATION: Estimating Who's Winning
// =============================================================================
//
// The evaluation function assigns a numerical score to a position: positive
// means white is better, negative means black is better, zero is equal.
//
// This is the "brain" of the chess engine—without search, the engine would
// just pick the move with the best immediate evaluation. Search allows it to
// look ahead and find moves that lead to good evaluations later.
//
// Our evaluation is built from three layers.
//
// LAYER 1 — WHAT YOU OWN AND WHERE IT STANDS. Material (plus a bishop-pair
// bonus) and a piece-square bonus for every piece. These decompose into "what
// is this piece on this square worth", so the Board keeps a running total of
// them and eval() reads it instead of walking the pieces. See eval_terms.hpp.
//
// LAYER 2 — POSITIONAL TERMS. Everything that depends on how the pieces relate
// to EACH OTHER, and therefore cannot be attributed to a single piece on a
// single square:
//
//   PAWN STRUCTURE  passed, doubled and isolated pawns (eval_pawn_structure)
//   KING SAFETY     pawn shield, open files beside the king, and how many enemy
//                   pieces are aiming at it (eval_king_safety)
//   ROOKS           open and semi-open files, and the seventh rank (eval_rooks)
//   MOBILITY        how many squares each piece can actually go to
//                   (eval_piece_activity)
//
// Each of those is a function returning a PhaseScore—a middlegame reading and
// an endgame reading—for ONE colour, and eval() takes White's minus Black's.
// Writing them per colour rather than as a signed difference is what keeps them
// readable: each one describes what a side has, and the subtraction happens once
// in one place.
//
// LAYER 3 — WHOSE TURN IT IS. A tempo bonus for the side to move, plus the
// draw and clamp rules. See eval() in src/eval.cpp.
//
// Layers 1 and 2 are each computed TWICE—once with middlegame numbers, once
// with endgame numbers—and blended according to the game phase. That blending
// is called a "tapered evaluation" and it is what lets one function give
// sensible advice in both a queen-filled middlegame and a bare-kings endgame.
//
// THE NEXT OPTIMISATION, when this becomes the bottleneck, is a PAWN HASH
// TABLE. Look at what layer 2 actually reads: the pawn-structure term needs
// only the two pawn bitboards, and the shield and open-file halves of king
// safety need only those plus the two king squares. Pawns move rarely—most
// nodes of a search share the same pawn structure as their parent—so those
// scores can be computed once, stored under a Zobrist key built from the pawns
// alone, and looked up for millions of nodes afterwards. Mobility cannot join
// them, because it changes whenever any piece moves.
//
// Still missing, and deliberately so: piece coordination, outposts, trapped
// pieces, king-pawn race knowledge, and any kind of automatic tuning of the
// numbers below (scripts/texel_tune.py sketches how that would work). Even a
// simple evaluation plus deep search plays a decent game; search depth
// compensates for a lot of evaluation simplicity.
//
// =============================================================================

#include <array>
#include <cstddef>

#include "c3/board.hpp"
#include "c3/colour.hpp"
#include "c3/eval_terms.hpp"
#include "c3/position.hpp"

namespace c3 {

// =============================================================================
// EVALUATION SCALE: Centipawns
// =============================================================================
// All evaluations use "centipawns" (1/100th of a pawn). A pawn = 100 centipawns.
// This gives fine-grained precision without floating point math.
//
// Examples:
//   +100  = white is up one pawn
//   -330  = black has a bishop advantage
//   +9745 = white has mate in N moves (see mate score encoding below)
// =============================================================================

inline constexpr int CENTIPAWN_MAX = 10'000;
inline constexpr int CENTIPAWN_MIN = -CENTIPAWN_MAX;
inline constexpr int CENTIPAWN_DRAW = 0;

// =============================================================================
// MATE SCORE ENCODING
// =============================================================================
// Checkmate is represented as CENTIPAWN_MATE (10000) minus the number of plies
// until mate. This ensures "mate in 3" beats "mate in 5"—the engine prefers
// faster mates.
//
//   Mate in 1 ply  = 10000 - 1 = 9999
//   Mate in 3 plies = 10000 - 3 = 9997
//   Getting mated in 2 = -(10000 - 2) = -9998
//
// CENTIPAWN_MATE_THRESHOLD (9745) distinguishes mate scores from huge material
// advantages. Any score above this threshold is a forced mate.
//
// Because the search treats "above the threshold" as "mate found", the static
// evaluation must never reach it on its own. A position with a dozen promoted
// queens really can add up to more than 9745 centipawns, so eval() clamps its
// result to ±CENTIPAWN_EVAL_MAX: a colossal material lead is still only a
// material lead, never a claim of forced mate.
// =============================================================================

inline constexpr int CENTIPAWN_MATE = CENTIPAWN_MAX;
inline constexpr int CENTIPAWN_MATE_THRESHOLD = CENTIPAWN_MATE - 255;
inline constexpr int CENTIPAWN_EVAL_MAX = CENTIPAWN_MATE_THRESHOLD - 1;

// The game phase scale (PHASE_MAX, PHASE_WEIGHTS), the Phase enum and the two
// tables of piece values (PIECE_VALUES, PIECE_VALUES_ENDGAME) live in
// eval_terms.hpp, included above, alongside the piece-square tables built from
// them. They moved there so that Board can keep a running total of them; see
// that header for the numbers themselves and for why the total is maintained
// rather than recomputed.
//
// Everything from here down is a term the Board does NOT accumulate, because
// none of it is a property of one piece on one square.

// =============================================================================
// PHASE SCORE: one term, read twice
// =============================================================================
// Every positional term has two opinions—what it is worth with the queens on,
// and what it is worth in an endgame—and eval() blends them. Carrying the pair
// around together means a term is written once and tapered once, instead of
// every term being computed twice by two nearly identical functions.
// =============================================================================

struct PhaseScore {
  int middlegame{};
  int endgame{};

  constexpr PhaseScore& operator+=(const PhaseScore& other) noexcept {
    middlegame += other.middlegame;
    endgame += other.endgame;
    return *this;
  }

  constexpr PhaseScore& operator-=(const PhaseScore& other) noexcept {
    middlegame -= other.middlegame;
    endgame -= other.endgame;
    return *this;
  }

  [[nodiscard]] friend constexpr bool operator==(const PhaseScore&,
                                                 const PhaseScore&) noexcept = default;
};

// =============================================================================
// BISHOP PAIR
// =============================================================================
// Two bishops cover both square colours, so nothing on the board is permanently
// safe from them. The pair is worth noticeably more than the sum of its parts,
// and more still in an open endgame where the bishops' range matters most.
// The bonus is awarded once, to a side holding at least two bishops.
// =============================================================================

inline constexpr int BISHOP_PAIR_MIDDLEGAME = 30;
inline constexpr int BISHOP_PAIR_ENDGAME = 50;

// =============================================================================
// PAWN STRUCTURE
// =============================================================================
// Pawns are the only pieces that cannot retreat, so a pawn move is permanent and
// a pawn weakness is permanent with it. Three of those weaknesses (and one
// strength) are cheap enough to be worth measuring at every node.
//
// PASSED PAWNS. A pawn with no enemy pawn ahead of it on its own or either
// neighbouring file cannot be stopped by pawns at all—only pieces can blockade
// it, and pieces have better things to do. The bonus is indexed by how far the
// pawn has travelled (see relative_rank in pawns.hpp) because a passer on the
// second rank is a distant promise while one on the seventh is nearly a queen.
// Only the FRONT pawn of a file collects it: a pawn stuck behind a friendly
// pawn is not a second promotion threat, it is the same threat twice.
//
// The endgame numbers are roughly double the middlegame ones, and that gap is
// the single most important thing these tables say. In a middlegame the enemy
// has a dozen pieces to spare for blockading duty and a running pawn may simply
// be rounded up; once the pieces are traded there is nobody left to stop it, and
// whole endgames are won by shepherding one passer home. This is why strong
// players trade pieces when they own a passed pawn and avoid trades when they
// face one—an idea the engine learns for free from the taper.
//
// DOUBLED PAWNS. Two pawns on one file cannot defend each other, and the rear
// one covers only squares the front one already covered, so the pair does less
// work than two pawns on neighbouring files. The penalty is charged once per
// extra pawn on the file, and it is worse in the endgame where a doubled pair
// can fail to create a passer that a healthy pair would have.
//
// ISOLATED PAWNS. A pawn with no friendly pawn on either neighbouring file can
// never be defended by another pawn: defending it costs a PIECE, permanently.
// The square in front of it is also a permanent hole for an enemy knight.
// =============================================================================

inline constexpr std::array<int, 8> PASSED_PAWN_MIDDLEGAME = {0, 5, 10, 20, 35, 60, 100, 0};
inline constexpr std::array<int, 8> PASSED_PAWN_ENDGAME = {0, 10, 20, 40, 70, 120, 200, 0};

inline constexpr PhaseScore DOUBLED_PAWN_PENALTY{.middlegame = -10, .endgame = -20};
inline constexpr PhaseScore ISOLATED_PAWN_PENALTY{.middlegame = -10, .endgame = -15};

// =============================================================================
// KING SAFETY
// =============================================================================
// Every number here is a middlegame number, and every endgame number is zero.
// That is not laziness: it is the term's most important claim. King safety is
// about surviving an attack, and once the queens and rooks are gone there is no
// attack left to survive—the same king that wanted a wall of pawns in front of
// it now wants to march into the centre and help its own pawns queen. The taper
// fades this whole term out exactly as that happens.
//
// PAWN SHIELD. The pawns on the king's file and its two neighbours, one or two
// ranks in front of it. Three unmoved pawns in front of a castled king are the
// classic shelter; every pawn pushed out of that zone is a hole an enemy piece
// can occupy with check. KING_SHIELD_MAX_PAWNS caps the TOTAL count, so a wall
// of doubled pawns cannot earn more than the three a castled king wants.
//
// OPEN FILES BESIDE THE KING. A file with no friendly pawn on it is a highway
// for an enemy rook or queen aimed at the king. If it has no enemy pawn either
// it is fully open, and worse still, because nothing at all obstructs it.
//
// ATTACKING PIECES. Any enemy knight, bishop, rook or queen whose attacks touch
// the eight squares around the king, whether or not those squares are occupied:
// a piece the defender has to keep guarded is still a piece tied down. Attacks
// on a king are not additive—they multiply. One
// piece pointing at the king zone is a nuisance; three are a mating attack. We
// only model the count, and only linearly (real engines use a lookup table that
// grows faster than linearly), and we cap it, because this evaluation has no
// tuning data behind it and a runaway king-safety term makes an engine hallucinate
// attacks and throw away material chasing them.
// =============================================================================

inline constexpr int KING_SHIELD_PAWN_MIDDLEGAME = 10;
inline constexpr int KING_SHIELD_MAX_PAWNS = 3;
inline constexpr int KING_SEMI_OPEN_FILE_MIDDLEGAME = -10;
inline constexpr int KING_FULLY_OPEN_FILE_EXTRA_MIDDLEGAME = -5;
inline constexpr int KING_ZONE_ATTACKER_MIDDLEGAME = -10;
inline constexpr int KING_ZONE_MAX_ATTACKERS = 4;

// =============================================================================
// ROOKS
// =============================================================================
// A rook is a long-range piece stuck behind a wall of its own pawns for the
// first twenty moves of the game, so "which file is it on" is most of what
// there is to say about it.
//
// OPEN FILE: no pawn of either colour. The rook sees the whole board and, more
// to the point, sees the enemy back rank.
// SEMI-OPEN FILE: no friendly pawn, but an enemy one. Half the value—the rook
// still has a target to attack, it just cannot go all the way.
//
// SEVENTH RANK: the rank where the enemy's unmoved pawns live and where the
// enemy king is often trapped behind them. A rook there attacks several pawns at
// once and cuts the king off from the rest of the board, which is why this is
// the one rook bonus that is BIGGER in the endgame: in an ending "rook on the
// seventh" is frequently the whole win. We only pay it when there is actually
// something to attack—enemy pawns still on that rank, or an enemy king stuck on
// the eighth—so a rook on an empty seventh rank earns nothing.
// =============================================================================

inline constexpr PhaseScore ROOK_OPEN_FILE{.middlegame = 20, .endgame = 10};
inline constexpr PhaseScore ROOK_SEMI_OPEN_FILE{.middlegame = 10, .endgame = 5};
inline constexpr PhaseScore ROOK_ON_SEVENTH{.middlegame = 15, .endgame = 25};

// =============================================================================
// MOBILITY
// =============================================================================
// The number of squares a piece can actually move to, weighted per piece type.
// This is the term that notices a bishop biting on its own pawn chain or a rook
// walled in on h1—positions where the material count says "equal" and every
// human says "White has an extra piece".
//
// The weights differ by piece and by phase for good reasons. A knight's squares
// are worth the most because a knight only has eight to begin with, so each one
// lost is a large fraction of the piece. A queen's are worth the least because
// she already has more squares than she can use and counting them again would
// just re-price the queen. Rook mobility is worth more in the endgame, when
// files open up and a rook's job becomes the whole game.
//
// For knights and bishops we do not count squares that an enemy PAWN attacks.
// A minor piece cannot use such a square: standing there loses material to the
// cheapest piece on the board. Counting them would tell the engine a knight
// surrounded by enemy pawns is a good knight.
//
// COST. This is by far the most expensive term in the evaluation—it is the only
// one that asks the magic-bitboard tables a question, once per sliding piece per
// side, at every leaf of the search. That buys real positional understanding,
// but it is paid for in nodes per second, and the trade only makes sense while
// the understanding is worth more than the depth it costs. See the comment above
// eval_piece_activity in src/eval.cpp for how the work is shared with king
// safety so that it is done once rather than twice.
// =============================================================================

inline constexpr std::array<PhaseScore, 6> MOBILITY_WEIGHTS = {{
    {.middlegame = 0, .endgame = 0}, // Pawn: covered by the pawn-structure terms
    {.middlegame = 4, .endgame = 4}, // Knight
    {.middlegame = 3, .endgame = 3}, // Bishop
    {.middlegame = 2, .endgame = 4}, // Rook: files open as the board empties
    {.middlegame = 1, .endgame = 2}, // Queen
    {.middlegame = 0, .endgame = 0}, // King: its own tables already place it
}};

// =============================================================================
// TEMPO
// =============================================================================
// Having the move is worth something in almost every position: it is one free
// half-move of development, or the chance to strike first. The bonus also does
// real work inside the search, where it discourages the engine from evaluating
// a quiet position as equal when it is actually the one under pressure.
//
// Unlike every other term, tempo is NOT a property of the board—it belongs to
// whoever is to move—so it is added after the side-to-move flip rather than
// before it. See eval() for what that does to the evaluation's symmetry.
// =============================================================================

inline constexpr int TEMPO_BONUS = 10;

// Non-pawn material left on the board, clamped to [0, PHASE_MAX].
// PHASE_MAX = full middlegame armies, 0 = bare kings.
[[nodiscard]] int game_phase(const Board& board) noexcept;

// True when neither side owns enough material to force checkmate, so the game
// is drawn no matter how well either player plays. See src/eval.cpp for the
// exact list of material combinations we recognise.
[[nodiscard]] bool has_insufficient_material(const Board& board) noexcept;

// Component scores for one colour, always positive-is-good-for-that-colour.
// Both take the phase explicitly so tests (and curious readers) can inspect
// either end of the taper.
//
// eval() no longer calls either of these: it reads the running totals the Board
// keeps instead. They remain as the from-scratch REFERENCE implementation—the
// slow, obviously-correct version that the tests and the Debug assertions in
// position.cpp measure the running totals against. Keeping a readable reference
// beside an optimised implementation is what makes the optimisation safe to
// trust; delete it and the only check left on the accumulator is that it agrees
// with itself.
[[nodiscard]] int eval_material(Colour colour, const Board& board, Phase phase) noexcept;
[[nodiscard]] int eval_psqt(Colour colour, const Board& board, Phase phase) noexcept;

// The positional terms, each scored for ONE colour and each positive-is-good
// for that colour. eval() calls every one of them twice, once per side, and
// takes the difference.
[[nodiscard]] PhaseScore eval_pawn_structure(Colour side, const Board& board) noexcept;
[[nodiscard]] PhaseScore eval_rooks(Colour side, const Board& board) noexcept;

// What one side's knights, bishops, rooks and queens are doing: how much room
// they have, and how many of them are looking at the squares around the ENEMY
// king. The two answers come back together because they are read off the same
// attack bitboards, and asking for those bitboards is the expensive part.
struct PieceActivity {
  PhaseScore mobility;
  int king_zone_attackers{};
};

[[nodiscard]] PieceActivity eval_piece_activity(Colour side, const Board& board) noexcept;

// How safe `side`'s king is. `enemy_attackers` is the king_zone_attackers count
// produced by eval_piece_activity(!side, board): this function cannot work it
// out for itself without redoing that whole pass over the enemy pieces, so the
// caller hands it over instead.
[[nodiscard]] PhaseScore eval_king_safety(Colour side, const Board& board,
                                          int enemy_attackers) noexcept;

// Layer 1 on its own: material, piece-square bonuses and the bishop pair,
// tapered, clamped and flipped for the side to move—but with none of the
// positional terms and no tempo bonus.
//
// This is what eval() used to be, and it is exposed so the accumulator tests
// have something exact to compare their from-scratch rebuild against. Mixing
// the positional terms into that comparison would test the terms rather than
// the accumulator, which is the one thing those tests exist to check.
[[nodiscard]] int eval_material_and_psqt(const Position& pos) noexcept;

// The whole evaluation, from the perspective of the side to move.
[[nodiscard]] int eval(const Position& pos) noexcept;

} // namespace c3
