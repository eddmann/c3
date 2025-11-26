#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "c3/uci.hpp"

using namespace c3;
namespace uci = c3::uci;
using namespace std::chrono_literals;

TEST(UciParse, GoCommandAllAttributes) {
  const auto cmd =
      uci::parse_command("go depth 1 movetime 2 wtime 3 btime 4 winc 5 binc 6 nodes 7");

  ASSERT_EQ(cmd.type, uci::CommandType::Go);
  ASSERT_TRUE(cmd.go_params.has_value());
  const auto& params = *cmd.go_params;

  EXPECT_EQ(params.depth, 1);
  EXPECT_EQ(params.movetime, 2ms);
  EXPECT_EQ(params.wtime, 3ms);
  EXPECT_EQ(params.btime, 4ms);
  EXPECT_EQ(params.winc, 5ms);
  EXPECT_EQ(params.binc, 6ms);
  EXPECT_EQ(params.nodes, 7U);
}

TEST(UciParse, GoInfinite) {
  const auto cmd = uci::parse_command("go infinite");

  ASSERT_EQ(cmd.type, uci::CommandType::Go);
  ASSERT_TRUE(cmd.go_params.has_value());

  const auto& params = *cmd.go_params;
  EXPECT_FALSE(params.depth.has_value());
  EXPECT_FALSE(params.movetime.has_value());
  EXPECT_FALSE(params.wtime.has_value());
  EXPECT_FALSE(params.btime.has_value());
  EXPECT_FALSE(params.winc.has_value());
  EXPECT_FALSE(params.binc.has_value());
  EXPECT_FALSE(params.nodes.has_value());
}

TEST(UciParse, PositionWithMoves) {
  const auto cmd = uci::parse_command("position startpos moves e2e4 e7e5");

  ASSERT_EQ(cmd.type, uci::CommandType::Position);
  ASSERT_TRUE(cmd.position.has_value());

  const auto& pos_cmd = *cmd.position;
  EXPECT_EQ(pos_cmd.fen, std::string(Position::START_POS_FEN));
  ASSERT_EQ(pos_cmd.moves.size(), 2U);

  EXPECT_EQ(pos_cmd.moves[0].from, Square::E2);
  EXPECT_EQ(pos_cmd.moves[0].to, Square::E4);
  EXPECT_FALSE(pos_cmd.moves[0].promotion_piece.has_value());

  EXPECT_EQ(pos_cmd.moves[1].from, Square::E7);
  EXPECT_EQ(pos_cmd.moves[1].to, Square::E5);
  EXPECT_FALSE(pos_cmd.moves[1].promotion_piece.has_value());
}

TEST(UciParse, PositionWithPromotionMoves) {
  const auto cmd = uci::parse_command("position startpos moves e7e8q e2e1r");

  ASSERT_EQ(cmd.type, uci::CommandType::Position);
  ASSERT_TRUE(cmd.position.has_value());

  const auto& pos_cmd = *cmd.position;
  ASSERT_EQ(pos_cmd.moves.size(), 2U);

  EXPECT_EQ(pos_cmd.moves[0].promotion_piece, Piece::WQ);
  EXPECT_EQ(pos_cmd.moves[1].promotion_piece, Piece::BR);
}

TEST(UciApplyPosition, HandlesEnPassantForWhite) {
  const auto cmd = uci::parse_command("position fen 4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1 moves e5d6");

  Position pos = Position::from_fen(Position::START_POS_FEN);
  uci::apply_position_command(*cmd.position, pos);

  EXPECT_EQ(pos.board.piece_at(Square::D6), Piece::WP);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E5));
  EXPECT_FALSE(pos.board.has_piece_at(Square::D5));
}

TEST(UciTime, MatchesExpectedAllocations) {
  const auto rapid = uci::calculate_allocated_time(600000ms, std::make_optional(10000ms));
  const auto blitz = uci::calculate_allocated_time(180000ms, std::make_optional(2000ms));
  const auto bullet = uci::calculate_allocated_time(60000ms, std::nullopt);
  const auto scramble = uci::calculate_allocated_time(5000ms, std::make_optional(500ms));
  const auto long_inc = uci::calculate_allocated_time(90000ms, std::make_optional(200000ms));

  EXPECT_EQ(rapid, std::make_optional(25000ms));
  EXPECT_EQ(blitz, std::make_optional(7000ms));
  EXPECT_EQ(bullet, std::make_optional(2000ms));
  EXPECT_EQ(scramble, std::make_optional(416ms));
  EXPECT_EQ(long_inc, std::make_optional(85500ms));
}

TEST(UciReporter, PrintsInfoAndTracksBestMove) {
  std::ostringstream out;
  uci::UciReporter reporter(out);

  search::Report report;
  report.depth = 3;
  report.nodes = 200;
  report.tt_stats = {1, 2};
  report.started_at = std::chrono::steady_clock::now() - 1s;

  MoveList pv;
  pv.push_back(Move{
      .piece = Piece::WP,
      .from = Square::E2,
      .to = Square::E4,
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  });
  pv.push_back(Move{
      .piece = Piece::BP,
      .from = Square::E7,
      .to = Square::E5,
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  });

  report.pv = std::make_pair(pv, 42);

  reporter.send(report);

  const auto output = out.str();
  EXPECT_NE(output.find("info depth 3"), std::string::npos);
  EXPECT_NE(output.find("score cp 42"), std::string::npos);
  EXPECT_NE(output.find("pv e2e4 e7e5"), std::string::npos);

  const auto best = reporter.best_move();
  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->from, Square::E2);
  EXPECT_EQ(best->to, Square::E4);
}

TEST(UciSession, StartposBestmovesDepth1To3) {
  const std::vector<std::string> script = {
      "uci", "isready", "position startpos", "go depth 1", "go depth 2", "go depth 3",
  };

  const auto output = uci::run_script_for_test(script);

  EXPECT_NE(output.find("bestmove e2e4"), std::string::npos);
  EXPECT_NE(output.find("info depth 1"), std::string::npos);
  EXPECT_NE(output.find("info depth 2"), std::string::npos);
  EXPECT_NE(output.find("info depth 3"), std::string::npos);
}

TEST(UciSession, KiwipeteBestmoveDepth3) {
  const std::vector<std::string> script = {
      "uci",
      "isready",
      "position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "go depth 3",
  };

  const auto output = uci::run_script_for_test(script);

  EXPECT_NE(output.find("bestmove e2a6"), std::string::npos);
  EXPECT_NE(output.find("score cp 50"), std::string::npos);
}

// -----------------------------------------------------------------------------
// Command Parsing Edge Cases
// -----------------------------------------------------------------------------

TEST(UciParse, PositionFenWithAllFields) {
  // FEN with half-move clock = 25, full move counter = 50
  const auto cmd =
      uci::parse_command("position fen r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 25 50");

  ASSERT_EQ(cmd.type, uci::CommandType::Position);
  ASSERT_TRUE(cmd.position.has_value());

  Position pos = Position::startpos();
  uci::apply_position_command(*cmd.position, pos);

  EXPECT_EQ(pos.half_move_clock, 25);
  EXPECT_EQ(pos.full_move_counter, 50);
}

TEST(UciParse, EmptyCommandThrows) {
  EXPECT_THROW(uci::parse_command(""), std::runtime_error);
  EXPECT_THROW(uci::parse_command("   "), std::runtime_error);
}

TEST(UciParse, UnknownCommandThrows) {
  EXPECT_THROW(uci::parse_command("foobar"), std::runtime_error);
  EXPECT_THROW(uci::parse_command("badcommand 123"), std::runtime_error);
}

TEST(UciParse, SetOptionHashValidRange) {
  // Min boundary
  const auto min_cmd = uci::parse_command("setoption name Hash value 1");
  ASSERT_EQ(min_cmd.type, uci::CommandType::SetOption);
  ASSERT_TRUE(min_cmd.option.has_value());
  EXPECT_EQ(min_cmd.option->name, "hash");
  EXPECT_EQ(min_cmd.option->value, "1");

  // Max boundary
  const auto max_cmd = uci::parse_command("setoption name Hash value 4096");
  ASSERT_EQ(max_cmd.type, uci::CommandType::SetOption);
  EXPECT_EQ(max_cmd.option->value, "4096");
}

TEST(UciParse, SetOptionUnknownThrows) {
  EXPECT_THROW(uci::parse_command("setoption name UnknownOption value 123"), std::runtime_error);
}

TEST(UciParse, SetOptionMissingNameThrows) {
  EXPECT_THROW(uci::parse_command("setoption"), std::runtime_error);
  EXPECT_THROW(uci::parse_command("setoption name"), std::runtime_error);
}

TEST(UciParse, GoDepthOutOfRangeThrows) {
  // Depth must fit in uint8_t (0-255), values > 255 throw
  EXPECT_THROW(uci::parse_command("go depth 256"), std::runtime_error);
}

TEST(UciParse, InvalidMoveThrows) {
  EXPECT_THROW(uci::parse_command("position startpos moves invalid"), std::runtime_error);
  EXPECT_THROW(uci::parse_command("position startpos moves e2"), std::runtime_error);
}

// -----------------------------------------------------------------------------
// Position Application Edge Cases
// -----------------------------------------------------------------------------

TEST(UciApplyPosition, HandlesEnPassantForBlack) {
  // Black to move, white just played d2-d4, black can capture e.p.
  const auto cmd = uci::parse_command("position fen 4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1 moves e4d3");

  Position pos = Position::startpos();
  uci::apply_position_command(*cmd.position, pos);

  EXPECT_EQ(pos.board.piece_at(Square::D3), Piece::BP);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E4));
  EXPECT_FALSE(pos.board.has_piece_at(Square::D4)); // Captured pawn removed
}

TEST(UciApplyPosition, CastlingThroughMoveSequence) {
  // Italian game with kingside castling
  const auto cmd = uci::parse_command("position startpos moves e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 e1g1");

  Position pos = Position::startpos();
  uci::apply_position_command(*cmd.position, pos);

  // White king should be on g1, rook on f1
  EXPECT_EQ(pos.board.piece_at(Square::G1), Piece::WK);
  EXPECT_EQ(pos.board.piece_at(Square::F1), Piece::WR);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E1));
  EXPECT_FALSE(pos.board.has_piece_at(Square::H1));

  // White should have lost kingside castling right
  EXPECT_FALSE(pos.castling_rights.has(CastlingRight::WhiteKing));
}

TEST(UciApplyPosition, PromotionCapture) {
  // Pawn promotes while capturing
  const auto cmd = uci::parse_command("position fen 1n2k3/2P5/8/8/8/8/8/4K3 w - - 0 1 moves c7b8q");

  Position pos = Position::startpos();
  uci::apply_position_command(*cmd.position, pos);

  EXPECT_EQ(pos.board.piece_at(Square::B8), Piece::WQ);
  EXPECT_FALSE(pos.board.has_piece_at(Square::C7));
}

TEST(UciApplyPosition, Underpromotion) {
  // Promote to knight
  const auto cmd = uci::parse_command("position fen 8/4P3/8/8/8/7k/8/4K3 w - - 0 1 moves e7e8n");

  Position pos = Position::startpos();
  uci::apply_position_command(*cmd.position, pos);

  EXPECT_EQ(pos.board.piece_at(Square::E8), Piece::WN);
  EXPECT_FALSE(pos.board.has_piece_at(Square::E7));
}

// -----------------------------------------------------------------------------
// Time Management Edge Cases
// -----------------------------------------------------------------------------

TEST(UciTime, ZeroTimeReturnsZero) {
  const auto result = uci::calculate_allocated_time(0ms, std::nullopt);
  EXPECT_EQ(result, std::make_optional(0ms));
}

TEST(UciTime, VeryLowTimeWithIncrement) {
  // 100ms left with 1s increment - should allocate something small but positive
  const auto result = uci::calculate_allocated_time(100ms, std::make_optional(1000ms));
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->count(), 0);
  EXPECT_LE(result->count(), 200); // Should not allocate more than time left + some margin
}

TEST(UciTime, LongGameTimeManagement) {
  // 1 hour classical game, no increment
  const auto result = uci::calculate_allocated_time(3600000ms, std::nullopt);
  ASSERT_TRUE(result.has_value());
  // Should allocate a reasonable fraction (not all the time)
  EXPECT_GT(result->count(), 30000);  // At least 30 seconds
  EXPECT_LT(result->count(), 300000); // Less than 5 minutes
}

// -----------------------------------------------------------------------------
// Full UCI Session Tests
// -----------------------------------------------------------------------------

TEST(UciSession, MateInOne) {
  // Back rank mate position: Re8#
  const std::vector<std::string> script = {
      "uci",
      "isready",
      "position fen 6k1/5ppp/8/8/8/8/8/4R1K1 w - - 0 1",
      "go depth 3",
  };

  const auto output = uci::run_script_for_test(script);

  EXPECT_NE(output.find("bestmove e1e8"), std::string::npos);
}

TEST(UciSession, PerftIntegration) {
  const std::vector<std::string> script = {
      "position startpos",
      "perft 3",
  };

  const auto output = uci::run_script_for_test(script);

  // Perft(3) from startpos = 8902
  EXPECT_NE(output.find("8902"), std::string::npos);
}

TEST(UciSession, EvalCommand) {
  const std::vector<std::string> script = {
      "position startpos",
      "eval",
  };

  const auto output = uci::run_script_for_test(script);

  // Symmetric position should evaluate to 0
  EXPECT_NE(output.find("eval:"), std::string::npos);
}

TEST(UciSession, ZobristCommand) {
  const std::vector<std::string> script = {
      "position startpos",
      "zobrist",
  };

  const auto output = uci::run_script_for_test(script);

  // Should print zobrist key
  EXPECT_NE(output.find("zobrist:"), std::string::npos);
}
