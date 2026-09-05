#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <istream>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
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

TEST(UciTime, BudgetGivesTheSoftLimitRoomToOverrun) {
  // The soft limit is still the old allocation; the hard limit is three times
  // it, so an iteration that turns out expensive can finish instead of being
  // thrown away half-done.
  const auto blitz = uci::calculate_time_budget(180000ms, std::make_optional(2000ms));

  EXPECT_EQ(blitz.soft, 7000ms);
  EXPECT_EQ(blitz.hard, 21000ms);
  EXPECT_EQ(blitz.soft, uci::calculate_allocated_time(180000ms, std::make_optional(2000ms)));
}

TEST(UciTime, BudgetNeverPromisesMoreThanTheClockHolds) {
  // Three times a healthy budget can exceed the whole remaining clock. What is
  // left after the reserve is the real ceiling.
  const auto scramble = uci::calculate_time_budget(5000ms, std::make_optional(500ms));

  EXPECT_EQ(scramble.soft, 416ms);
  EXPECT_EQ(scramble.hard, 1248ms); // 3 x 416, comfortably inside 5000 - 250
  EXPECT_LE(scramble.hard, 5000ms - 250ms);

  // With almost nothing left, the floor has already claimed everything the
  // clock can afford, so there is no headroom to give and the two coincide.
  const auto desperate = uci::calculate_time_budget(20ms, std::nullopt);
  EXPECT_EQ(desperate.hard, desperate.soft);
}

TEST(UciTime, BudgetHardLimitIsNeverBelowTheSoftOne) {
  for (const auto clock : {10ms, 60ms, 500ms, 5000ms, 60000ms, 600000ms}) {
    const auto budget = uci::calculate_time_budget(clock, std::nullopt);
    EXPECT_GE(budget.hard, budget.soft) << "clock " << clock.count() << "ms";
  }
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

  EXPECT_NE(output.find("bestmove g1f3"), std::string::npos);
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
  EXPECT_NE(output.find("score cp "), std::string::npos);
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

// -----------------------------------------------------------------------------
// run_loop coverage
//
// `run_script_for_test` is a legacy in-process harness; the tests below drive
// the real `uci::run_loop` (parser + dispatcher + background search thread),
// which is what a GUI actually talks to.
// -----------------------------------------------------------------------------

namespace {

std::string run_uci(const std::string& input) {
  std::istringstream in(input);
  std::ostringstream out;
  uci::run_loop(in, out);
  return out.str();
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> lines_starting_with(const std::string& text, const std::string& prefix) {
  std::vector<std::string> matches;
  for (const auto& line : split_lines(text)) {
    if (line.rfind(prefix, 0) == 0) {
      matches.push_back(line);
    }
  }
  return matches;
}

} // namespace

TEST(UciParse, GoAcceptsMovesToGo) {
  EXPECT_NO_THROW((void)uci::parse_command("go wtime 10000 btime 10000 movestogo 40"));
}

TEST(UciParse, GoIgnoresUnknownTokens) {
  EXPECT_NO_THROW((void)uci::parse_command("go depth 2 nonsense 5"));
}

TEST(UciParse, GoAcceptsSearchMovesPonderAndMate) {
  EXPECT_NO_THROW((void)uci::parse_command("go searchmoves e2e4 d2d4 depth 3"));
  EXPECT_NO_THROW((void)uci::parse_command("go ponder wtime 1000 btime 1000"));
  EXPECT_NO_THROW((void)uci::parse_command("go mate 2"));
}

TEST(UciParse, GoInfiniteOverridesTimeAndKeepsLaterTokens) {
  const auto with_time = uci::parse_command("go wtime 1000 btime 1000 infinite");
  ASSERT_TRUE(with_time.go_params.has_value());
  EXPECT_FALSE(with_time.go_params->wtime.has_value());
  EXPECT_FALSE(with_time.go_params->btime.has_value());

  const auto after_infinite = uci::parse_command("go infinite depth 3");
  ASSERT_TRUE(after_infinite.go_params.has_value());
  EXPECT_EQ(after_infinite.go_params->depth, 3);
}

TEST(UciParse, NoOpProtocolCommandsAreAccepted) {
  EXPECT_NO_THROW((void)uci::parse_command("debug on"));
  EXPECT_NO_THROW((void)uci::parse_command("register later"));
  EXPECT_NO_THROW((void)uci::parse_command("ponderhit"));
}

TEST(UciParse, SetOptionHashOutOfRangeReportsRange) {
  try {
    (void)uci::parse_command("setoption name Hash value 999999");
    FAIL() << "expected an out-of-range error";
  } catch (const std::exception& ex) {
    const std::string message = ex.what();
    EXPECT_EQ(message.find("could not parse"), std::string::npos) << message;
    EXPECT_NE(message.find("between"), std::string::npos) << message;
  }
}

TEST(UciApplyPosition, RejectsIllegalMoves) {
  Position pos = Position::startpos();

  // Promotion suffix on a non-promotion move.
  const auto bogus_promotion = uci::parse_command("position startpos moves e2e4q");
  EXPECT_THROW(uci::apply_position_command(*bogus_promotion.position, pos), std::runtime_error);

  // A black move while it is White to move.
  const auto wrong_side = uci::parse_command("position startpos moves e7e5");
  EXPECT_THROW(uci::apply_position_command(*wrong_side.position, pos), std::runtime_error);

  // A move that is simply not legal for the piece.
  const auto illegal = uci::parse_command("position startpos moves e2e5");
  EXPECT_THROW(uci::apply_position_command(*illegal.position, pos), std::runtime_error);
}

TEST(UciApplyPosition, LeavesPositionUntouchedWhenAMoveIsRejected) {
  Position pos = Position::startpos();
  const auto original_fen = pos.to_fen();

  const auto cmd = uci::parse_command("position startpos moves e2e4 e7e5 d2d5");
  EXPECT_THROW(uci::apply_position_command(*cmd.position, pos), std::runtime_error);
  EXPECT_EQ(pos.to_fen(), original_fen);
}

TEST(UciLoop, FullSessionAnswersEveryCommand) {
  const auto output = run_uci("uci\nisready\nposition startpos moves e2e4\ngo depth 2\nquit\n");

  EXPECT_NE(output.find("id name"), std::string::npos) << output;
  EXPECT_NE(output.find("uciok"), std::string::npos) << output;
  EXPECT_NE(output.find("readyok"), std::string::npos) << output;
  EXPECT_NE(output.find("info depth"), std::string::npos) << output;

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  const auto move_text = bestmoves[0].substr(std::string("bestmove ").size());
  EXPECT_GE(move_text.size(), 4U) << output;
  EXPECT_LE(move_text.size(), 5U) << output;
}

TEST(UciLoop, NewGameThenSearchStillReportsABestMove) {
  const auto output = run_uci("ucinewgame\nposition startpos\ngo depth 1\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;
}

TEST(UciLoop, StopEndsAnInfiniteSearch) {
  const auto output = run_uci("position startpos\ngo infinite\nstop\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;

  // The reported move must be the one we actually searched, not the fallback:
  // at least one iteration reported, and its PV starts with that move.
  const auto lines = split_lines(output);
  std::string last_pv_move;
  std::size_t info_depth_lines = 0;
  for (const auto& line : lines) {
    if (line.rfind("info depth ", 0) != 0) {
      continue;
    }
    ++info_depth_lines;
    const auto pv = line.find(" pv ");
    if (pv != std::string::npos) {
      std::istringstream moves(line.substr(pv + 4));
      moves >> last_pv_move;
    }
  }

  EXPECT_GE(info_depth_lines, 1U) << output;
  ASSERT_FALSE(last_pv_move.empty()) << output;
  EXPECT_EQ(bestmoves[0], "bestmove " + last_pv_move) << output;
}

TEST(UciLoop, EofStopsTheSearchAndStillReportsOneBestMove) {
  // No `quit`: the input stream simply ends while the search thread is running.
  const auto output = run_uci("position startpos\ngo depth 2\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;
}

TEST(UciLoop, ZeroClockStillProducesALegalBestMove) {
  const auto output = run_uci("position startpos\ngo wtime 0 btime 0\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;
}

TEST(UciLoop, ReportsNoneOnlyWhenThereIsNoLegalMove) {
  // Black has just delivered mate, so White has no legal reply at all.
  const auto output = run_uci("position fen 8/8/8/8/8/5k2/6q1/7K w - - 0 1\ngo depth 3\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0], "bestmove (none)") << output;
}

TEST(UciLoop, DiagnosticsAreLegalUciLines) {
  const auto output = run_uci("foobar\nposition startpos moves zz99\nquit\n");

  EXPECT_TRUE(lines_starting_with(output, "error:").empty()) << output;
  EXPECT_GE(lines_starting_with(output, "info string ").size(), 2U) << output;
}

TEST(UciLoop, IgnoresDebugRegisterAndPonderHit) {
  const auto output = run_uci("debug on\nregister later\nponderhit\nisready\nquit\n");

  EXPECT_TRUE(lines_starting_with(output, "info string ").empty()) << output;
  EXPECT_NE(output.find("readyok"), std::string::npos) << output;
}

TEST(UciParse, GoMovesToGoIsCaptured) {
  const auto cmd = uci::parse_command("go wtime 10000 btime 10000 movestogo 40");

  ASSERT_TRUE(cmd.go_params.has_value());
  EXPECT_EQ(cmd.go_params->movestogo, 40U);
  EXPECT_EQ(cmd.go_params->wtime, 10000ms);
}

TEST(UciParse, GoSearchMovesAreConsumedNotTreatedAsAttributes) {
  const auto cmd = uci::parse_command("go searchmoves e2e4 d2d4 depth 3");

  ASSERT_TRUE(cmd.go_params.has_value());
  ASSERT_EQ(cmd.go_params->searchmoves.size(), 2U);
  EXPECT_EQ(cmd.go_params->searchmoves[0].to, Square::E4);
  EXPECT_EQ(cmd.go_params->searchmoves[1].to, Square::D4);
  EXPECT_EQ(cmd.go_params->depth, 3);
}

TEST(UciParse, GoMateMapsToAPlyDepth) {
  // A mate in N needs N of our moves and N-1 replies: 2N-1 plies.
  const auto cmd = uci::parse_command("go mate 2");

  ASSERT_TRUE(cmd.go_params.has_value());
  EXPECT_EQ(cmd.go_params->depth, 3);
}

TEST(UciParse, GoPonderAndInfiniteAreFlags) {
  const auto ponder = uci::parse_command("go ponder depth 2");
  ASSERT_TRUE(ponder.go_params.has_value());
  EXPECT_TRUE(ponder.go_params->ponder);
  EXPECT_FALSE(ponder.go_params->infinite);
  EXPECT_EQ(ponder.go_params->depth, 2);

  const auto infinite = uci::parse_command("go wtime 1000 infinite");
  ASSERT_TRUE(infinite.go_params.has_value());
  EXPECT_TRUE(infinite.go_params->infinite);
  EXPECT_FALSE(infinite.go_params->wtime.has_value());
}

TEST(UciTime, MovesToGoSplitsTheRemainingClock) {
  // 10s with 40 moves to reach the control: roughly a quarter of a second.
  const auto shared = uci::calculate_allocated_time(10000ms, std::nullopt, 40U);
  EXPECT_EQ(shared, std::make_optional(250ms));

  // "One move to go" must not spend the whole clock.
  const auto last_move = uci::calculate_allocated_time(10000ms, std::nullopt, 1U);
  EXPECT_EQ(last_move, std::make_optional(5000ms));
}

TEST(UciTime, NeverAllocatesZeroWhileTimeRemains) {
  // Reserves and integer division would otherwise round these down to 0ms,
  // which aborts the search before it evaluates anything.
  const auto sliver = uci::calculate_allocated_time(1ms, std::nullopt);
  ASSERT_TRUE(sliver.has_value());
  EXPECT_GT(sliver->count(), 0);
  EXPECT_LE(sliver->count(), 1);

  const auto scramble = uci::calculate_allocated_time(60ms, std::nullopt);
  EXPECT_EQ(scramble, std::make_optional(10ms));
}

// -----------------------------------------------------------------------------
// Malformed input must never cost us a `bestmove`
// -----------------------------------------------------------------------------

TEST(UciParse, GoWithUnparsableValuesDoesNotThrow) {
  // A value we cannot read is dropped like an unknown token: `go` still has to
  // produce a search, and therefore a bestmove.
  EXPECT_NO_THROW((void)uci::parse_command("go depth abc"));
  EXPECT_NO_THROW((void)uci::parse_command("go wtime nine btime 1000"));
  EXPECT_NO_THROW((void)uci::parse_command("go mate 300"));
  EXPECT_NO_THROW((void)uci::parse_command("go movestogo 5000000000"));

  // The readable half of the line still applies.
  const auto mixed = uci::parse_command("go wtime nine btime 1000");
  ASSERT_TRUE(mixed.go_params.has_value());
  EXPECT_FALSE(mixed.go_params->wtime.has_value());
  EXPECT_EQ(mixed.go_params->btime, 1000ms);
  EXPECT_FALSE(mixed.diagnostics.empty());
}

TEST(UciParse, GoDepthOutOfRangeIsClampedNotDropped) {
  // Clamping keeps a bounded request bounded; dropping it would silently turn
  // `go depth 256` into a search that only `stop` can end.
  const auto cmd = uci::parse_command("go depth 256");
  ASSERT_TRUE(cmd.go_params.has_value());
  EXPECT_EQ(cmd.go_params->depth, 255);

  const auto zero = uci::parse_command("go depth 0");
  ASSERT_TRUE(zero.go_params.has_value());
  EXPECT_EQ(zero.go_params->depth, 1);
}

TEST(UciParse, GoMateAndDepthTakeTheTighterBound) {
  const auto mate_first = uci::parse_command("go mate 2 depth 8");
  const auto depth_first = uci::parse_command("go depth 8 mate 2");

  ASSERT_TRUE(mate_first.go_params.has_value());
  ASSERT_TRUE(depth_first.go_params.has_value());
  EXPECT_EQ(mate_first.go_params->depth, 3);
  EXPECT_EQ(depth_first.go_params->depth, 3);
}

TEST(UciParse, GoNodesAcceptsTheFullUnsignedRange) {
  const auto cmd = uci::parse_command("go nodes 18446744073709551615");

  ASSERT_TRUE(cmd.go_params.has_value());
  EXPECT_EQ(cmd.go_params->nodes, std::numeric_limits<std::uint64_t>::max());
}

TEST(UciParse, GoNodesRejectsNegativeValues) {
  // std::stoull would happily wrap "-1" into 2^64-1.
  const auto cmd = uci::parse_command("go nodes -1");

  ASSERT_TRUE(cmd.go_params.has_value());
  EXPECT_FALSE(cmd.go_params->nodes.has_value());
  EXPECT_FALSE(cmd.diagnostics.empty());
}

TEST(UciLoop, UnparsableGoValueStillYieldsOneBestMove) {
  const auto output = run_uci("position startpos\ngo depth abc\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;
  EXPECT_FALSE(lines_starting_with(output, "info string ").empty()) << output;
}

TEST(UciLoop, EverySearchGetsExactlyOneBestMove) {
  const auto output = run_uci("position startpos\ngo infinite\ngo depth 1\nquit\n");

  EXPECT_EQ(lines_starting_with(output, "bestmove ").size(), 2U) << output;
}

TEST(UciLoop, IsReadyIsAnsweredWhileSearching) {
  const auto output = run_uci("position startpos\ngo infinite\nisready\nstop\nquit\n");

  const auto readyok = output.find("readyok");
  const auto bestmove = output.find("bestmove ");
  ASSERT_NE(readyok, std::string::npos) << output;
  ASSERT_NE(bestmove, std::string::npos) << output;
  EXPECT_LT(readyok, bestmove) << output;
}

TEST(UciLoop, StopWithoutASearchSaysNothing) {
  EXPECT_EQ(run_uci("stop\nquit\n"), "");
}

TEST(UciLoop, EveryLineIsAValidUciCommand) {
  // Debug helpers must not leak bare text: a GUI parses every line we write.
  const auto output = run_uci("position startpos\nprintfen\neval\nzobrist\nperft 2\nquit\n");

  for (const auto& line : split_lines(output)) {
    if (line.empty()) {
      continue;
    }
    const bool valid = line.rfind("info ", 0) == 0 || line.rfind("bestmove ", 0) == 0 ||
                       line.rfind("id ", 0) == 0 || line.rfind("option ", 0) == 0 ||
                       line == "uciok" || line == "readyok";
    EXPECT_TRUE(valid) << "not a UCI line: " << line << '\n' << output;
  }
}

// -----------------------------------------------------------------------------
// The Engine's transposition table, seen through the loop
// -----------------------------------------------------------------------------
//
// These tests need a search to run to its full depth, which `run_uci` cannot
// give them: it hands the loop the whole script at once, so the next command
// arrives microseconds after `go` and the `stop()` it performs cuts the search
// off just after its depth-1 report. A real GUI does not do that—it waits for
// `bestmove` before sending anything else—so the harness below paces the
// session the same way.
// -----------------------------------------------------------------------------

namespace {

// An output sink both the loop's search thread and the test read, so every
// access goes through one mutex.
class RecordingSink : public std::streambuf {
public:
  [[nodiscard]] std::string text() const {
    const std::scoped_lock lock(mutex_);
    return text_;
  }

  [[nodiscard]] std::size_t bestmove_count() const {
    const std::scoped_lock lock(mutex_);
    std::size_t count = 0;
    for (std::size_t at = text_.find("bestmove "); at != std::string::npos;
         at = text_.find("bestmove ", at + 1)) {
      count += 1;
    }
    return count;
  }

protected:
  int_type overflow(int_type ch) override {
    if (ch != traits_type::eof()) {
      const std::scoped_lock lock(mutex_);
      text_.push_back(static_cast<char>(ch));
    }
    return ch;
  }

  std::streamsize xsputn(const char* data, std::streamsize size) override {
    const std::scoped_lock lock(mutex_);
    text_.append(data, static_cast<std::size_t>(size));
    return size;
  }

private:
  mutable std::mutex mutex_;
  std::string text_;
};

// Releases one command at a time, and never the command after a `go` until
// that `go` has been answered with a `bestmove`.
class PacedInput : public std::streambuf {
public:
  PacedInput(std::vector<std::string> commands, const RecordingSink& sink)
      : commands_(std::move(commands)), sink_(&sink) {}

protected:
  int_type underflow() override {
    if (gptr() < egptr()) {
      return traits_type::to_int_type(*gptr());
    }

    if (next_ == commands_.size()) {
      return traits_type::eof();
    }

    // Bounded so a search that never answers fails the test instead of
    // hanging the whole suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (sink_->bestmove_count() < searches_started_ &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    line_ = commands_[next_] + "\n";
    if (commands_[next_].rfind("go", 0) == 0) {
      ++searches_started_;
    }
    ++next_;

    setg(line_.data(), line_.data(), line_.data() + line_.size());
    return traits_type::to_int_type(line_[0]);
  }

private:
  std::vector<std::string> commands_;
  const RecordingSink* sink_;
  std::string line_;
  std::size_t next_{0};
  std::size_t searches_started_{0};
};

std::string run_uci_paced(const std::vector<std::string>& commands) {
  RecordingSink sink;
  std::ostream out(&sink);
  PacedInput input(commands, sink);
  std::istream in(&input);

  uci::run_loop(in, out);

  return sink.text();
}

// Node count from the last `info depth` line of each search, in order: one
// entry per `bestmove`, which is one entry per `go`.
std::vector<std::uint64_t> node_counts_per_search(const std::string& output) {
  std::vector<std::uint64_t> counts;
  std::uint64_t latest = 0;

  for (const auto& line : split_lines(output)) {
    if (line.rfind("info depth ", 0) == 0) {
      const auto nodes = line.find(" nodes ");
      if (nodes != std::string::npos) {
        std::istringstream value(line.substr(nodes + std::string(" nodes ").size()));
        value >> latest;
      }
      continue;
    }

    if (line.rfind("bestmove ", 0) == 0) {
      counts.push_back(latest);
      latest = 0;
    }
  }

  return counts;
}

// Deep enough that a warm table saves an obvious amount of work, shallow
// enough to stay quick in a Debug build with sanitisers on.
constexpr auto WARM_TABLE_GO = "go depth 6";

} // namespace

TEST(UciLoop, SecondGoReusesTheTableTheFirstOneFilled) {
  // The loop must search through the Engine's table rather than build a
  // throwaway one per `go`. When it does, the second search of the same
  // position starts from everything the first one learned and costs a
  // fraction of the nodes.
  const auto output = run_uci_paced({"position startpos", WARM_TABLE_GO, WARM_TABLE_GO, "quit"});

  const auto counts = node_counts_per_search(output);
  ASSERT_EQ(counts.size(), 2U) << output;
  ASSERT_GT(counts[0], 0U) << output;
  EXPECT_LT(counts[1], counts[0] / 2) << output;
}

// -----------------------------------------------------------------------------
// bench
// -----------------------------------------------------------------------------

namespace {

// The `info string bench nodes <N> nps <M>` line's node total, or nullopt when
// the run did not produce one.
std::optional<std::uint64_t> bench_total(const std::string& output) {
  for (const auto& line : lines_starting_with(output, "info string bench nodes ")) {
    std::istringstream fields(line.substr(std::string("info string bench nodes ").size()));
    std::uint64_t nodes = 0;
    if (fields >> nodes) {
      return nodes;
    }
  }
  return std::nullopt;
}

} // namespace

TEST(UciBench, RunsEveryPositionAndTotalsTheNodes) {
  const auto output = run_uci("bench 1\nquit\n");

  // One line per position, then the timing line and the total.
  const auto positions = lines_starting_with(output, "info string bench position ");
  EXPECT_EQ(positions.size(), 12U) << output;

  const auto total = bench_total(output);
  ASSERT_TRUE(total.has_value()) << output;
  EXPECT_GT(*total, 0U) << output;
  EXPECT_NE(output.find(" nps "), std::string::npos) << output;
}

TEST(UciBench, TwoRunsAgreeExactly) {
  // The whole point of bench: the same build searching the same positions must
  // reach the same total, or the number cannot be compared between builds.
  // Clearing the table before each position is what makes that true.
  const auto first = bench_total(run_uci("bench 1\nquit\n"));
  const auto second = bench_total(run_uci("bench 1\nquit\n"));

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
}

TEST(UciBench, IsUnaffectedByWhatCameBeforeIt) {
  // A search run first leaves the table full. If bench inherited it, its total
  // would depend on the session's history rather than on the engine.
  const auto cold = bench_total(run_uci("bench 1\nquit\n"));
  const auto warm = bench_total(run_uci("position startpos\ngo depth 4\nbench 1\nquit\n"));

  ASSERT_TRUE(cold.has_value());
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(*cold, *warm);
}

TEST(UciBench, DeeperRunsSearchMore) {
  const auto shallow = bench_total(run_uci("bench 1\nquit\n"));
  const auto deeper = bench_total(run_uci("bench 3\nquit\n"));

  ASSERT_TRUE(shallow.has_value());
  ASSERT_TRUE(deeper.has_value());
  EXPECT_GT(*deeper, *shallow);
}

TEST(UciBench, EveryLineIsAValidUciLine) {
  // bench is a developer tool, but it still writes to the same stream a GUI is
  // reading, so it may not emit bare text.
  const auto output = run_uci("bench 1\nquit\n");

  for (const auto& line : split_lines(output)) {
    if (line.empty()) {
      continue;
    }
    EXPECT_EQ(line.rfind("info ", 0), 0U) << "not a UCI line: " << line;
  }
}

TEST(UciLoop, HashOptionStopsTheSearchBeforeResizingTheTable) {
  // Resizing frees the memory the search is reading, so the search must be
  // joined before the table is touched. The visible consequence is that the
  // interrupted `go` still gets its one `bestmove`, and the session survives
  // to answer afterwards.
  const auto output = run_uci("position startpos\ngo infinite\nsetoption name Hash value 1\n"
                              "isready\nquit\n");

  const auto bestmoves = lines_starting_with(output, "bestmove ");
  ASSERT_EQ(bestmoves.size(), 1U) << output;
  EXPECT_EQ(bestmoves[0].find("(none)"), std::string::npos) << output;
  EXPECT_NE(output.find("readyok"), std::string::npos) << output;
}

TEST(UciLoop, NewGameForgetsWhatTheTableLearned) {
  // `ucinewgame` says the next position belongs to a different game, so the
  // stored scores describe a tree we will never visit again. After it the
  // search has to pay full price once more.
  const auto output = run_uci_paced({"position startpos", WARM_TABLE_GO, "ucinewgame",
                                     "position startpos", WARM_TABLE_GO, "quit"});

  const auto counts = node_counts_per_search(output);
  ASSERT_EQ(counts.size(), 2U) << output;
  ASSERT_GT(counts[0], 0U) << output;
  EXPECT_GT(counts[1], counts[0] / 2) << output;
}
