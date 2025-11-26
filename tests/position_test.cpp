#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "c3/move.hpp"
#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/square.hpp"

using namespace c3;

namespace {
// NOLINTBEGIN(modernize-use-designated-initializers)

Position parse_fen(std::string_view fen) {
  return Position::from_fen(fen);
}

Move make_move(Piece piece, Square from, Square to, std::optional<Piece> captured = std::nullopt,
               std::optional<Piece> promotion = std::nullopt, bool is_en_passant = false) {
  return Move{piece, from, to, captured, promotion, is_en_passant};
}

} // namespace

TEST(Move, DetectsCastlingByFileDelta) {
  const Move king_side{Piece::WK, Square::E1, Square::G1, std::nullopt, std::nullopt, false};
  const Move queen_side{Piece::WK, Square::E1, Square::C1, std::nullopt, std::nullopt, false};
  const Move normal{Piece::WK, Square::E1, Square::E2, std::nullopt, std::nullopt, false};

  EXPECT_TRUE(king_side.is_castling());
  EXPECT_TRUE(queen_side.is_castling());
  EXPECT_FALSE(normal.is_castling());
}

TEST(Move, CaptureSquareHandlesEnPassant) {
  const Move ep_capture{Piece::WP, Square::D5, Square::E6, Piece::BP, std::nullopt, true};
  ASSERT_TRUE(ep_capture.capture_square().has_value());
  EXPECT_EQ(*ep_capture.capture_square(), Square::E5);

  const Move normal{Piece::WQ, Square::A1, Square::A8, Piece::BR, std::nullopt, false};
  EXPECT_EQ(normal.capture_square(), normal.to);
}

TEST(Move, EqualityIgnoresCapturedPiece) {
  const Move a{Piece::WQ, Square::A1, Square::A8, Piece::BR, std::nullopt, false};
  const Move b{Piece::WQ, Square::A1, Square::A8, std::nullopt, std::nullopt, false};
  EXPECT_EQ(a, b);
}

TEST(Position, MoveAPiece) {
  Position pos = parse_fen("8/8/8/8/8/8/8/5R2 w - - 0 1");
  const Move mv{Piece::WR, Square::F1, Square::F4, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.to), mv.piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.from));
  EXPECT_EQ(pos.colour_to_move, Colour::Black);
}

TEST(Position, UndoMovingAPiece) {
  Position pos = parse_fen("4k3/8/8/8/8/8/8/4KR2 w - - 0 1");
  const Move mv{Piece::WR, Square::F1, Square::F4, std::nullopt, std::nullopt, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.to));
  EXPECT_EQ(pos.colour_to_move, Colour::White);
}

TEST(Position, CaptureAPiece) {
  Position pos = parse_fen("8/8/8/5p2/3N4/8/8/8 w - - 0 1");
  const Move mv{Piece::WN, Square::D4, Square::F5, Piece::BP, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.to), mv.piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.from));
}

TEST(Position, UndoCapturingAPiece) {
  Position pos = parse_fen("4k3/8/8/5p2/3N4/8/8/4K3 w - - 0 1");
  const Move mv{Piece::WN, Square::D4, Square::F5, Piece::BP, std::nullopt, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_EQ(pos.board.piece_at(mv.to), mv.captured_piece);
}

TEST(Position, CastleKingSide) {
  Position pos = parse_fen("8/8/8/8/8/8/8/4K2R w K - 0 1");
  const Move mv{Piece::WK, Square::E1, Square::G1, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.castling_rights, CastlingRights::none());
  EXPECT_EQ(pos.board.piece_at(mv.to), mv.piece);
  EXPECT_EQ(pos.board.piece_at(Square::F1), Piece::WR);
  EXPECT_FALSE(pos.board.has_piece_at(mv.from));
  EXPECT_FALSE(pos.board.has_piece_at(Square::H1));
}

TEST(Position, UndoCastleKingSide) {
  Position pos = parse_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
  const Move mv{Piece::WK, Square::E1, Square::G1, std::nullopt, std::nullopt, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.castling_rights, CastlingRights::from({CastlingRight::WhiteKing}));
  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_EQ(pos.board.piece_at(Square::H1), Piece::WR);
  EXPECT_FALSE(pos.board.has_piece_at(mv.to));
  EXPECT_FALSE(pos.board.has_piece_at(Square::F1));
}

TEST(Position, MovingRookRemovesCastlingRights) {
  Position pos = parse_fen("8/8/8/8/8/8/8/R3K2R w KQ - 0 1");
  const Move mv{Piece::WR, Square::H1, Square::G1, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.castling_rights, CastlingRights::from({CastlingRight::WhiteQueen}));
}

TEST(Position, CapturingRookRemovesCastlingRights) {
  Position pos = parse_fen("8/8/8/8/3b4/8/8/R3K2R b KQ - 0 1");
  const Move mv{Piece::BB, Square::D4, Square::A1, Piece::WR, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.castling_rights, CastlingRights::from({CastlingRight::WhiteKing}));
}

TEST(Position, PromoteAPawn) {
  Position pos = parse_fen("8/4P3/8/8/8/8/8/8 w - - 0 1");
  const Move mv{Piece::WP, Square::E7, Square::E8, std::nullopt, Piece::WN, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.to), mv.promotion_piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.from));
}

TEST(Position, UndoPromotingAPawn) {
  Position pos = parse_fen("4k3/2P5/8/8/8/8/8/4K3 w - - 0 1");
  const Move mv{Piece::WP, Square::C7, Square::C8, std::nullopt, Piece::WN, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.to));
}

TEST(Position, UndoPromotingAPawnWithCapture) {
  Position pos = parse_fen("1n2k3/2P5/8/8/8/8/8/4K3 w - - 0 1");
  const Move mv{Piece::WP, Square::C7, Square::B8, Piece::BN, Piece::WN, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_EQ(pos.board.piece_at(mv.to), mv.captured_piece);
}

TEST(Position, CapturePawnEnPassant) {
  Position pos = parse_fen("8/8/8/3Pp3/8/8/8/8 w - e6 0 1");
  const Move mv{Piece::WP, Square::D5, Square::E6, Piece::BP, std::nullopt, true};

  pos.make_move(mv);

  EXPECT_EQ(pos.board.piece_at(mv.to), mv.piece);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E5));
  EXPECT_FALSE(pos.board.has_piece_at(mv.from));
}

TEST(Position, UndoCapturePawnEnPassant) {
  Position pos = parse_fen("4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 1");
  const Move mv{Piece::WP, Square::D5, Square::E6, Piece::BP, std::nullopt, true};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.en_passant_square, mv.to);
  EXPECT_EQ(pos.board.piece_at(mv.from), mv.piece);
  EXPECT_EQ(pos.board.piece_at(Square::E5), mv.captured_piece);
  EXPECT_FALSE(pos.board.has_piece_at(mv.to));
}

TEST(Position, SetEnPassantSquareForWhiteDoubleAdvance) {
  Position pos = parse_fen("8/8/8/8/8/8/4P3/8 w - - 0 1");
  const Move mv{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.en_passant_square, Square::E3);
}

TEST(Position, SetEnPassantSquareForBlackDoubleAdvance) {
  Position pos = parse_fen("8/4p3/8/8/8/8/8/8 b - - 0 1");
  const Move mv{Piece::BP, Square::E7, Square::E5, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.en_passant_square, Square::E6);
}

TEST(Position, ResetEnPassantWhenUndoingDoubleAdvance) {
  Position pos = Position::startpos();
  const Move mv{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false};

  pos.make_move(mv);
  pos.unmake_move(mv);

  EXPECT_EQ(pos.en_passant_square, std::nullopt);
}

TEST(Position, RestorePreviousEnPassantWhenUndoingMove) {
  Position pos = Position::startpos();

  const Move pawn_push{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false};
  pos.make_move(pawn_push);

  const Move knight{Piece::BN, Square::G8, Square::F6, std::nullopt, std::nullopt, false};
  pos.make_move(knight);

  pos.unmake_move(knight);

  EXPECT_EQ(pos.en_passant_square, Square::E3);
}

TEST(Position, IncrementHalfMoveClockForQuietMove) {
  Position pos = parse_fen("8/4p3/8/8/8/8/4P3/4K3 w - - 0 1");
  const Move mv{Piece::WK, Square::E1, Square::F2, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.half_move_clock, 1);
}

TEST(Position, ResetHalfMoveClockWhenPawnMoves) {
  Position pos = parse_fen("8/4p3/8/8/8/8/4P3/8 w - - 1 1");
  const Move mv{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.half_move_clock, 0);
}

TEST(Position, ResetHalfMoveClockWhenCaptureOccurs) {
  Position pos = parse_fen("8/4p3/8/8/8/8/4Q3/8 w - - 1 1");
  const Move mv{Piece::WQ, Square::E2, Square::E7, Piece::BP, std::nullopt, false};

  pos.make_move(mv);

  EXPECT_EQ(pos.half_move_clock, 0);
}

TEST(Position, IncrementFullMoveCounterWhenBlackMoves) {
  Position pos = Position::startpos();

  const Move white{Piece::WP, Square::E2, Square::E4, std::nullopt, std::nullopt, false};
  pos.make_move(white);
  EXPECT_EQ(pos.full_move_counter, 1);

  const Move black{Piece::BP, Square::E7, Square::E5, std::nullopt, std::nullopt, false};
  pos.make_move(black);
  EXPECT_EQ(pos.full_move_counter, 2);

  pos.unmake_move(black);
  EXPECT_EQ(pos.full_move_counter, 1);

  pos.unmake_move(white);
  EXPECT_EQ(pos.full_move_counter, 1);
}

TEST(Position, DetectRepetitionDrawFromStartPosition) {
  Position pos = Position::startpos();

  std::array<Move, 8> moves = {
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BN, Square::G8, Square::F6),
      make_move(Piece::WN, Square::F3, Square::G1), make_move(Piece::BN, Square::F6, Square::G8),
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BN, Square::G8, Square::F6),
      make_move(Piece::WN, Square::F3, Square::G1), make_move(Piece::BN, Square::F6, Square::G8),
  };

  for (std::size_t idx = 0; idx < moves.size(); ++idx) {
    pos.make_move(moves[idx]);
    const bool expect_draw = idx == moves.size() - 1;
    EXPECT_EQ(pos.is_repetition_draw(0), expect_draw) << "ply " << (idx + 1);
  }
}

TEST(Position, DetectRepetitionDrawFromMiddleGamePosition) {
  Position pos = parse_fen("1r1q1rk1/2p2pp1/2Q4p/pB2P3/P2P4/b6P/2R2PP1/3R2K1 b - - 10 33");

  std::array<Move, 8> moves = {
      make_move(Piece::BR, Square::B8, Square::C8), make_move(Piece::WB, Square::B5, Square::A6),
      make_move(Piece::BR, Square::C8, Square::B8), make_move(Piece::WB, Square::A6, Square::B5),
      make_move(Piece::BR, Square::B8, Square::C8), make_move(Piece::WB, Square::B5, Square::A6),
      make_move(Piece::BR, Square::C8, Square::B8), make_move(Piece::WB, Square::A6, Square::B5),
  };

  for (std::size_t idx = 0; idx < moves.size(); ++idx) {
    pos.make_move(moves[idx]);
    const bool expect_draw = idx == moves.size() - 1;
    EXPECT_EQ(pos.is_repetition_draw(0), expect_draw) << "ply " << (idx + 1);
  }
}

TEST(Position, RepetitionNotCountedWhenCastlingRightsDiffer) {
  Position pos = Position::startpos();

  std::array<Move, 9> moves = {
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BN, Square::G8, Square::F6),
      make_move(Piece::WR, Square::H1, Square::G1), make_move(Piece::BN, Square::F6, Square::G8),
      make_move(Piece::WR, Square::G1, Square::H1), make_move(Piece::BN, Square::G8, Square::F6),
      make_move(Piece::WN, Square::F3, Square::G1), make_move(Piece::BN, Square::F6, Square::G8),
      make_move(Piece::WN, Square::G1, Square::F3),
  };

  for (std::size_t idx = 0; idx < moves.size(); ++idx) {
    pos.make_move(moves[idx]);
    EXPECT_FALSE(pos.is_repetition_draw(0)) << "ply " << (idx + 1);
  }
}

TEST(Position, RepetitionNotCountedWhenEnPassantAvailabilityDiffers) {
  Position pos = parse_fen("4k3/4p3/8/3P4/8/8/8/4K1Nn w - - 0 1");

  std::array<Move, 10> moves = {
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BP, Square::E7, Square::E5),
      make_move(Piece::WN, Square::F3, Square::G1), make_move(Piece::BN, Square::H1, Square::G3),
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BN, Square::G3, Square::H1),
      make_move(Piece::WN, Square::F3, Square::G1), make_move(Piece::BN, Square::H1, Square::G3),
      make_move(Piece::WN, Square::G1, Square::F3), make_move(Piece::BN, Square::G3, Square::H1),
  };

  for (std::size_t idx = 0; idx < moves.size(); ++idx) {
    pos.make_move(moves[idx]);
    EXPECT_FALSE(pos.is_repetition_draw(0)) << "ply " << (idx + 1);
  }
}

TEST(Position, NullMoveClearsEnPassantAndTogglesSide) {
  Position pos = parse_fen("8/8/8/3Pp3/8/8/8/4K3 w - e6 10 20");
  const auto key_before = pos.key;

  pos.make_null_move();

  EXPECT_EQ(pos.colour_to_move, Colour::Black);
  EXPECT_EQ(pos.en_passant_square, std::nullopt);
  EXPECT_EQ(pos.half_move_clock, 11);
  EXPECT_EQ(pos.full_move_counter, 20);
  EXPECT_NE(pos.key, key_before);
  EXPECT_EQ(pos.key, pos.compute_key());

  pos.unmake_null_move();
  EXPECT_EQ(pos.colour_to_move, Colour::White);
  EXPECT_EQ(pos.en_passant_square, Square::E6);
  EXPECT_EQ(pos.half_move_clock, 10);
  EXPECT_EQ(pos.full_move_counter, 20);
  EXPECT_EQ(pos.key, key_before);
}

TEST(Position, ZobristKeyStaysInSyncDuringMoveUndo) {
  Position pos = Position::startpos();
  const auto initial = pos.key;

  std::vector<Move> moves = {
      make_move(Piece::WP, Square::E2, Square::E4),
      make_move(Piece::BP, Square::C7, Square::C5),
      make_move(Piece::WN, Square::G1, Square::F3),
      make_move(Piece::BN, Square::B8, Square::C6),
  };

  for (const auto& mv : moves) {
    pos.make_move(mv);
    EXPECT_EQ(pos.key, pos.compute_key());
  }

  for (const auto& mv : std::ranges::reverse_view(moves)) {
    pos.unmake_move(mv);
    EXPECT_EQ(pos.key, pos.compute_key());
  }

  EXPECT_EQ(pos.key, initial);
}
// NOLINTEND(modernize-use-designated-initializers)
