// =============================================================================
// MOVE GENERATION: Finding All Pseudo-Legal Moves
// =============================================================================
//
// Move generation is one of the most performance-critical parts of a chess engine.
// During search, we generate moves millions of times per second, so every
// microsecond matters. This file uses several key techniques:
//
// 1. PRECOMPUTED ATTACK TABLES
//    For pieces with fixed attack patterns (pawns, knights, kings), we precompute
//    all possible attacks at compile time. Looking up attacks is O(1)—just an
//    array index. Without precomputation, we'd calculate attacks on every query.
//
// 2. MAGIC BITBOARDS (for sliding pieces)
//    Rooks, bishops, and queens are "sliding" pieces—their attacks depend on
//    blockers. Magic bitboards use a clever hashing trick to look up attacks
//    in O(1) time. See magic.hpp for details.
//
// 3. PSEUDO-LEGAL THEN FILTER
//    We generate "pseudo-legal" moves (moves that follow piece movement rules)
//    then filter out moves that leave our king in check. This is often faster
//    than generating only legal moves directly, especially with bitboards.
//
// =============================================================================

#include "c3/movegen.hpp"

#include <array>
#include <cassert>

#include "c3/bitboard.hpp"
#include "c3/magic.hpp"

namespace c3 {

namespace {

constexpr std::size_t colour_index(Colour colour) {
  return static_cast<std::size_t>(colour);
}

constexpr std::array<Square, 64> make_squares() {
  std::array<Square, 64> squares{};
  for (std::size_t i = 0; i < squares.size(); ++i) {
    squares[i] = Square::from_index(static_cast<std::uint8_t>(i));
  }
  return squares;
}

// =============================================================================
// PAWN ATTACK TABLE
// =============================================================================
// Pawns attack diagonally forward. For each square, we precompute the two
// squares a pawn could capture (if enemies were there).
//
// Bit shifting moves pieces on the board:
//   - << 8 moves up one rank (white's forward)
//   - >> 8 moves down one rank (black's forward)
//   - << 1 moves right one file (towards H)
//   - >> 1 moves left one file (towards A)
//
// Diagonal moves combine rank + file shifts:
//   - White captures: << 7 (up-left) and << 9 (up-right)
//   - Black captures: >> 7 (down-right) and >> 9 (down-left)
//
// The FILE_A and FILE_H masks prevent wrap-around: a pawn on the A-file
// can't capture left (it would wrap to the H-file), and vice versa.
// =============================================================================
constexpr std::array<std::array<Bitboard, 64>, 2> make_pawn_attacks() {
  std::array<std::array<Bitboard, 64>, 2> attacks{};
  const auto squares = make_squares();

  for (const auto square : squares) {
    const Bitboard bb = square;

    attacks[colour_index(Colour::White)][square.index()] =
        ((bb & ~FILE_A) << 7) | ((bb & ~FILE_H) << 9);

    attacks[colour_index(Colour::Black)][square.index()] =
        ((bb & ~FILE_H) >> 7) | ((bb & ~FILE_A) >> 9);
  }

  return attacks;
}

// =============================================================================
// KNIGHT ATTACK TABLE
// =============================================================================
// Knights move in an "L" shape: 2 squares in one direction, 1 square perpendicular.
// This gives 8 possible moves from any square (fewer on edges).
//
// Each shift combination represents one L-shaped move:
//   << 6  = up 1, left 2    (15 for up 2, left 1)
//   << 10 = up 1, right 2   (17 for up 2, right 1)
//   >> 6  = down 1, right 2 (etc.)
//   >> 10 = down 1, left 2
//
// File masks prevent wrap-around. A knight on the B-file moving "left 2" would
// wrap around to the G/H files without masking. We exclude files A and B for
// left-2 moves, and files G and H for right-2 moves.
// =============================================================================
constexpr std::array<Bitboard, 64> make_knight_attacks() {
  std::array<Bitboard, 64> attacks{};
  const auto squares = make_squares();

  for (const auto square : squares) {
    const Bitboard bb = square;

    attacks[square.index()] = ((bb & ~FILE_A & ~FILE_B) << 6) | ((bb & ~FILE_G & ~FILE_H) << 10) |
                              ((bb & ~FILE_A) << 15) | ((bb & ~FILE_H) << 17) |
                              ((bb & ~FILE_G & ~FILE_H) >> 6) | ((bb & ~FILE_A & ~FILE_B) >> 10) |
                              ((bb & ~FILE_H) >> 15) | ((bb & ~FILE_A) >> 17);
  }

  return attacks;
}

// =============================================================================
// KING ATTACK TABLE
// =============================================================================
// Kings can move one square in any direction (8 possible moves).
// Like knights, we precompute all attacks and use file masks to prevent wrap.
// =============================================================================
constexpr std::array<Bitboard, 64> make_king_attacks() {
  std::array<Bitboard, 64> attacks{};
  const auto squares = make_squares();

  for (const auto square : squares) {
    const Bitboard bb = square;

    // All 8 directions: N, S, E, W, NE, NW, SE, SW
    attacks[square.index()] = ((bb & ~FILE_H) << 1) | ((bb & ~FILE_A) >> 1) | (bb << 8) |
                              ((bb & ~FILE_A) << 7) | ((bb & ~FILE_H) << 9) | (bb >> 8) |
                              ((bb & ~FILE_H) >> 7) | ((bb & ~FILE_A) >> 9);
  }

  return attacks;
}

inline constexpr auto PAWN_ATTACKS = make_pawn_attacks();
inline constexpr auto KNIGHT_ATTACKS = make_knight_attacks();
inline constexpr auto KING_ATTACKS = make_king_attacks();

constexpr std::array<std::uint8_t, 2> PAWN_START_RANKS = {1, 6};

inline constexpr Bitboard WHITE_KING_CASTLING_PATH = Square::F1 | Square::G1;
inline constexpr Bitboard BLACK_KING_CASTLING_PATH = Square::F8 | Square::G8;
inline constexpr Bitboard WHITE_QUEEN_CASTLING_PATH = Square::B1 | Square::C1 | Square::D1;
inline constexpr Bitboard BLACK_QUEEN_CASTLING_PATH = Square::B8 | Square::C8 | Square::D8;

Bitboard pawn_attacks(Square square, Colour colour, const Board& board) {
  return PAWN_ATTACKS[colour_index(colour)][square.index()] & board.pieces_by_colour(!colour);
}

Bitboard knight_attacks(Square square) {
  return KNIGHT_ATTACKS[square.index()];
}

Bitboard king_attacks(Square square) {
  return KING_ATTACKS[square.index()];
}

// =============================================================================
// MAGIC BITBOARD LOOKUPS (Sliding Pieces)
// =============================================================================
// Unlike knights and kings, sliding pieces (rook, bishop, queen) have attacks
// that depend on which squares are blocked. A rook on A1 attacks differently
// when there's a piece on A4 vs when the file is clear.
//
// The naive approach: trace rays in each direction until hitting a piece.
// This is O(n) per query, which adds up during search.
//
// Magic bitboards achieve O(1) lookup using this formula:
//   index = ((occupancy & mask) * magic_number) >> shift
//
// The "magic number" is carefully chosen so that multiplying by the relevant
// occupancy bits and shifting produces a unique index for each blocker pattern.
// This index looks up the precomputed attack bitboard.
//
// The trade-off: we need ~2MB of lookup tables, but attack generation becomes
// a single multiply, shift, and array lookup—extremely fast.
//
// See magic.hpp for the precomputed tables and magic numbers.
// =============================================================================

Bitboard bishop_attacks(Square square, const Board& board) {
  const Magic& magic = BISHOP_MAGICS[square.index()];
  const Bitboard occupancy = board.occupancy() & magic.mask;
  const std::uint64_t index = (occupancy * magic.num) >> magic.shift;
  return BISHOP_ATTACKS[magic.offset + index];
}

Bitboard rook_attacks(Square square, const Board& board) {
  const Magic& magic = ROOK_MAGICS[square.index()];
  const Bitboard occupancy = board.occupancy() & magic.mask;
  const std::uint64_t index = (occupancy * magic.num) >> magic.shift;
  return ROOK_ATTACKS[magic.offset + index];
}

Bitboard pawn_advances(Square square, Colour colour, const Board& board) {
  const Square one_ahead = square.advance(colour);

  if (board.has_piece_at(one_ahead)) {
    return 0;
  }

  if (square.rank() != PAWN_START_RANKS[colour_index(colour)]) {
    return one_ahead;
  }

  const Square two_ahead = one_ahead.advance(colour);
  if (board.has_piece_at(two_ahead)) {
    return one_ahead;
  }

  return one_ahead | two_ahead;
}

// =============================================================================
// CASTLING MOVE GENERATION
// =============================================================================
// Castling has strict requirements:
//   1. King and rook haven't moved (tracked by castling rights)
//   2. Squares between king and rook are empty
//   3. King doesn't pass through or land on an attacked square
//   4. King is not currently in check (checked in castling_moves below)
//
// Note: The rook CAN pass through an attacked square (relevant for queenside).
// We only check the squares the KING passes through (F1/D1 for white).
// =============================================================================

Bitboard white_castling(CastlingRights rights, const Board& board) {
  Bitboard moves = 0;

  if ((rights & CastlingRight::WhiteKing) && !board.has_occupancy_at(WHITE_KING_CASTLING_PATH) &&
      !is_attacked(Square::F1, Colour::Black, board)) {
    moves |= Square::G1;
  }

  if ((rights & CastlingRight::WhiteQueen) && !board.has_occupancy_at(WHITE_QUEEN_CASTLING_PATH) &&
      !is_attacked(Square::D1, Colour::Black, board)) {
    moves |= Square::C1;
  }

  return moves;
}

Bitboard black_castling(CastlingRights rights, const Board& board) {
  Bitboard moves = 0;

  if ((rights & CastlingRight::BlackKing) && !board.has_occupancy_at(BLACK_KING_CASTLING_PATH) &&
      !is_attacked(Square::F8, Colour::White, board)) {
    moves |= Square::G8;
  }

  if ((rights & CastlingRight::BlackQueen) && !board.has_occupancy_at(BLACK_QUEEN_CASTLING_PATH) &&
      !is_attacked(Square::D8, Colour::White, board)) {
    moves |= Square::C8;
  }

  return moves;
}

Bitboard castling_moves(CastlingRights rights, Colour colour, const Board& board) {
  const Bitboard moves =
      colour == Colour::White ? white_castling(rights, board) : black_castling(rights, board);

  if (moves != 0 && !is_in_check(colour, board)) {
    return moves;
  }

  return 0;
}

} // namespace

// Find pawns that can capture en passant to a given square.
// Uses reverse attack lookup: which squares attack the en passant square?
// Those are the potential source squares for capturing pawns.
Bitboard en_passant_sources(Square en_passant_square, Colour colour, const Board& board) {
  return PAWN_ATTACKS[colour_index(!colour)][en_passant_square.index()] &
         board.pieces(pawn(colour));
}

Bitboard attacks_for(Piece piece, Square square, const Board& board) {
  switch (piece) {
  case Piece::WP:
  case Piece::BP:
    return pawn_attacks(square, colour(piece), board);
  case Piece::WN:
  case Piece::BN:
    return knight_attacks(square);
  case Piece::WB:
  case Piece::BB:
    return bishop_attacks(square, board);
  case Piece::WR:
  case Piece::BR:
    return rook_attacks(square, board);
  case Piece::WQ:
  case Piece::BQ:
    return bishop_attacks(square, board) | rook_attacks(square, board);
  case Piece::WK:
  case Piece::BK:
    return king_attacks(square);
  }
  return 0;
}

// =============================================================================
// ATTACK DETECTION
// =============================================================================
// To check if a square is attacked, we use a clever reverse lookup:
// "If a knight were on this square, which squares could it attack?"
// Any enemy knight on those squares can attack us. Same logic for all pieces.
//
// This is faster than checking each enemy piece individually, because we
// reuse the same attack patterns we already computed for move generation.
// =============================================================================

Bitboard get_attackers(Square square, Colour colour, const Board& board) {
  const Bitboard pawn_mask = pawn_attacks(square, !colour, board);
  const Bitboard knight_mask = knight_attacks(square);
  const Bitboard bishop_mask = bishop_attacks(square, board);
  const Bitboard rook_mask = rook_attacks(square, board);
  const Bitboard queen_mask = bishop_mask | rook_mask; // Queen = bishop + rook
  const Bitboard king_mask = king_attacks(square);

  return (board.pieces(pawn(colour)) & pawn_mask) | (board.pieces(knight(colour)) & knight_mask) |
         (board.pieces(bishop(colour)) & bishop_mask) | (board.pieces(rook(colour)) & rook_mask) |
         (board.pieces(queen(colour)) & queen_mask) | (board.pieces(king(colour)) & king_mask);
}

bool is_attacked(Square square, Colour colour, const Board& board) {
  return get_attackers(square, colour, board) != 0;
}

// Check if a side's king is in check. Used for move legality filtering.
bool is_in_check(Colour colour, const Board& board) {
  const Bitboard king_bb = board.pieces(king(colour));
  assert(king_bb != 0);
  const Square king_square = Square::first_occupied(king_bb);
  return is_attacked(king_square, !colour, board);
}

MoveList pseudo_legal_moves(const Position& pos) {
  MoveList moves;
  moves.reserve(MAX_LEGAL_MOVES);

  const Colour colour_to_move = pos.colour_to_move;

  for (const Piece piece : pieces_for(colour_to_move)) {
    Bitboard piece_bb = pos.board.pieces(piece);

    while (piece_bb != 0) {
      const Square from_square = Square::pop_first_occupied(piece_bb);

      Bitboard to_squares =
          ~pos.board.pieces_by_colour(colour_to_move) & attacks_for(piece, from_square, pos.board);

      if (is_pawn(piece)) {
        to_squares |= pawn_advances(from_square, colour_to_move, pos.board);
      } else if (is_king(piece)) {
        to_squares |= castling_moves(pos.castling_rights, colour_to_move, pos.board);
      }

      while (to_squares != 0) {
        const Square to_square = Square::pop_first_occupied(to_squares);
        const auto captured_piece = pos.board.piece_at(to_square);

        if (is_pawn(piece) && to_square.is_back_rank()) {
          for (const Piece promo : promotions_for(colour_to_move)) {
            moves.push_back(Move{
                .piece = piece,
                .from = from_square,
                .to = to_square,
                .captured_piece = captured_piece,
                .promotion_piece = promo,
                .is_en_passant = false,
            });
          }
          continue;
        }

        moves.push_back(Move{
            .piece = piece,
            .from = from_square,
            .to = to_square,
            .captured_piece = captured_piece,
            .promotion_piece = std::nullopt,
            .is_en_passant = false,
        });
      }
    }
  }

  if (pos.en_passant_square.has_value()) {
    Bitboard from_squares = en_passant_sources(*pos.en_passant_square, colour_to_move, pos.board);

    while (from_squares != 0) {
      moves.push_back(Move{
          .piece = pawn(colour_to_move),
          .from = Square::pop_first_occupied(from_squares),
          .to = *pos.en_passant_square,
          .captured_piece = pawn(!colour_to_move),
          .promotion_piece = std::nullopt,
          .is_en_passant = true,
      });
    }
  }

  return moves;
}

MoveList pseudo_legal_noisy_moves(const Position& pos) {
  MoveList moves;
  moves.reserve(MAX_LEGAL_MOVES);

  const Colour colour_to_move = pos.colour_to_move;
  const Bitboard captures_mask = pos.board.pieces_by_colour(!colour_to_move);

  for (const Piece piece : pieces_for(colour_to_move)) {
    Bitboard piece_bb = pos.board.pieces(piece);

    while (piece_bb != 0) {
      const Square from_square = Square::pop_first_occupied(piece_bb);

      Bitboard to_squares = captures_mask & attacks_for(piece, from_square, pos.board);

      if (is_pawn(piece)) {
        to_squares |= pawn_advances(from_square, colour_to_move, pos.board) & BACK_RANKS;
      }

      while (to_squares != 0) {
        const Square to_square = Square::pop_first_occupied(to_squares);
        const auto captured_piece = pos.board.piece_at(to_square);

        if (is_pawn(piece) && to_square.is_back_rank()) {
          for (const Piece promo : promotions_for(colour_to_move)) {
            moves.push_back(Move{
                .piece = piece,
                .from = from_square,
                .to = to_square,
                .captured_piece = captured_piece,
                .promotion_piece = promo,
                .is_en_passant = false,
            });
          }
          continue;
        }

        moves.push_back(Move{
            .piece = piece,
            .from = from_square,
            .to = to_square,
            .captured_piece = captured_piece,
            .promotion_piece = std::nullopt,
            .is_en_passant = false,
        });
      }
    }
  }

  if (pos.en_passant_square.has_value()) {
    Bitboard from_squares = en_passant_sources(*pos.en_passant_square, colour_to_move, pos.board);

    while (from_squares != 0) {
      moves.push_back(Move{
          .piece = pawn(colour_to_move),
          .from = Square::pop_first_occupied(from_squares),
          .to = *pos.en_passant_square,
          .captured_piece = pawn(!colour_to_move),
          .promotion_piece = std::nullopt,
          .is_en_passant = true,
      });
    }
  }

  return moves;
}

// =============================================================================
// PERFT: Performance Test / Move Generation Validation
// =============================================================================
// Perft counts all leaf nodes at a given depth. It's the gold standard for
// testing move generation correctness: if perft(depth=6) from the starting
// position equals 119,060,324 (a known value), move generation is correct.
//
// Perft doesn't evaluate positions or prune—it exhaustively explores every
// legal move sequence. This makes it perfect for debugging: a wrong count
// means a bug in move generation.
// =============================================================================

std::uint64_t perft(Position& pos, std::uint8_t depth) {
  if (depth == 0) {
    return 1;
  }

  std::uint64_t nodes = 0;

  for (const auto& mv : pseudo_legal_moves(pos)) {
    pos.make_move(mv);

    // Filter out moves that leave own king in check (pseudo-legal → legal)
    if (!is_in_check(pos.opponent_colour(), pos.board)) {
      nodes += perft(pos, static_cast<std::uint8_t>(depth - 1));
    }

    pos.unmake_move(mv);
  }

  return nodes;
}

} // namespace c3
