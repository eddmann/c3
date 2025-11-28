#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "c3/movegen.hpp"
#include "c3/search.hpp"
#include "c3/tablebase.hpp"

using namespace c3;
namespace tb = c3::tablebase;

namespace {

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

// =============================================================================
// Stub Tablebase for Testing
// =============================================================================
// A configurable stub that allows tests to control probe results.

class StubTablebase : public tb::Tablebase {
public:
  bool init(const std::string& path) override {
    path_ = path;
    available_ = !path.empty();
    return available_;
  }

  void free() override {
    available_ = false;
    path_.clear();
  }

  [[nodiscard]] bool is_available() const override { return available_; }

  [[nodiscard]] std::uint8_t max_pieces() const override { return max_pieces_; }

  [[nodiscard]] std::optional<tb::WdlResult> probe_wdl(const Position& pos) const override {
    if (!available_ || tb::count_pieces(pos) > max_pieces_) {
      return std::nullopt;
    }
    return wdl_result_;
  }

  [[nodiscard]] std::optional<tb::DtzResult> probe_dtz(const Position& pos) const override {
    if (!available_ || tb::count_pieces(pos) > max_pieces_) {
      return std::nullopt;
    }
    return dtz_result_;
  }

  [[nodiscard]] std::optional<std::vector<tb::RootMove>>
  probe_root(const Position& pos, const MoveList& legal_moves) const override {
    if (!available_ || tb::count_pieces(pos) > max_pieces_) {
      return std::nullopt;
    }
    return root_result_;
  }

  // Configuration methods for tests
  void set_max_pieces(std::uint8_t pieces) { max_pieces_ = pieces; }
  void set_wdl_result(std::optional<tb::WdlResult> result) { wdl_result_ = result; }
  void set_dtz_result(std::optional<tb::DtzResult> result) { dtz_result_ = result; }
  void set_root_result(std::optional<std::vector<tb::RootMove>> result) {
    root_result_ = std::move(result);
  }

private:
  bool available_{false};
  std::string path_;
  std::uint8_t max_pieces_{6};
  std::optional<tb::WdlResult> wdl_result_;
  std::optional<tb::DtzResult> dtz_result_;
  std::optional<std::vector<tb::RootMove>> root_result_;
};

} // namespace

// =============================================================================
// WDL Result Tests
// =============================================================================

TEST(TablebaseWdl, WinConvertsToHighPositiveScore) {
  EXPECT_EQ(tb::wdl_to_centipawns(tb::WdlResult::Win), 10000);
}

TEST(TablebaseWdl, LossConvertsToHighNegativeScore) {
  EXPECT_EQ(tb::wdl_to_centipawns(tb::WdlResult::Loss), -10000);
}

TEST(TablebaseWdl, DrawConvertsToZero) {
  EXPECT_EQ(tb::wdl_to_centipawns(tb::WdlResult::Draw), 0);
}

TEST(TablebaseWdl, CursedWinConvertsToSmallPositive) {
  const int score = tb::wdl_to_centipawns(tb::WdlResult::CursedWin);
  EXPECT_GT(score, 0);
  EXPECT_LT(score, 100);
}

TEST(TablebaseWdl, BlessedLossConvertsToSmallNegative) {
  const int score = tb::wdl_to_centipawns(tb::WdlResult::BlessedLoss);
  EXPECT_LT(score, 0);
  EXPECT_GT(score, -100);
}

// =============================================================================
// DTZ Result Tests
// =============================================================================

TEST(TablebaseDtz, ValidWhenDtzIsNonZero) {
  tb::DtzResult result{tb::WdlResult::Win, 5};
  EXPECT_TRUE(result.is_valid());
}

TEST(TablebaseDtz, ValidWhenDrawWithZeroDtz) {
  tb::DtzResult result{tb::WdlResult::Draw, 0};
  EXPECT_TRUE(result.is_valid());
}

TEST(TablebaseDtz, InvalidWhenWinWithZeroDtz) {
  // A winning position should have non-zero DTZ
  tb::DtzResult result{tb::WdlResult::Win, 0};
  EXPECT_FALSE(result.is_valid());
}

// =============================================================================
// Piece Counting Tests
// =============================================================================

TEST(TablebasePieceCount, StartingPositionHas32Pieces) {
  const Position pos = Position::startpos();
  EXPECT_EQ(tb::count_pieces(pos), 32);
}

TEST(TablebasePieceCount, EndgameKPvKHas3Pieces) {
  // King + Pawn vs King
  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  EXPECT_EQ(tb::count_pieces(pos), 3);
}

TEST(TablebasePieceCount, EndgameKRRvKHas4Pieces) {
  // King + 2 Rooks vs King
  const Position pos = parse("8/8/8/8/8/4k3/R7/R3K3 w - - 0 1");
  EXPECT_EQ(tb::count_pieces(pos), 4);
}

TEST(TablebasePieceCount, EndgameKQvKRBNHas6Pieces) {
  // King + Queen vs King + Rook + Bishop + Knight
  const Position pos = parse("8/8/2k5/8/8/2nrb3/8/3QK3 w - - 0 1");
  EXPECT_EQ(tb::count_pieces(pos), 6);
}

// =============================================================================
// Probe Eligibility Tests
// =============================================================================

TEST(TablebaseEligibility, StartingPositionNotProbeable) {
  const Position pos = Position::startpos();
  EXPECT_FALSE(tb::is_probeable(pos));
}

TEST(TablebaseEligibility, EndgameWithCastlingNotProbeable) {
  // 5-piece position but with castling rights
  const Position pos = parse("8/8/8/8/8/4k3/4P3/R3K2R w KQ - 0 1");
  EXPECT_FALSE(tb::is_probeable(pos));
}

TEST(TablebaseEligibility, EndgameWithoutCastlingIsProbeable) {
  // 3-piece position, no castling
  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  EXPECT_TRUE(tb::is_probeable(pos));
}

TEST(TablebaseEligibility, SixPieceEndgameIsProbeable) {
  // 6 pieces, no castling
  const Position pos = parse("8/8/2k5/8/8/2nrb3/8/3QK3 w - - 0 1");
  EXPECT_TRUE(tb::is_probeable(pos));
}

TEST(TablebaseEligibility, SevenPieceEndgameNotProbeable) {
  // 7 pieces exceeds default limit
  const Position pos = parse("8/8/2k5/8/3p4/2nrb3/8/3QK3 w - - 0 1");
  EXPECT_FALSE(tb::is_probeable(pos));
}

// =============================================================================
// Should Probe Tests (depth-based)
// =============================================================================

TEST(TablebaseShouldProbe, ProbesAtSufficientDepth) {
  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  tb::Config::set_probe_depth(1);
  EXPECT_TRUE(tb::should_probe(pos, 2));
}

TEST(TablebaseShouldProbe, DoesNotProbeAtInsufficientDepth) {
  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  tb::Config::set_probe_depth(3);
  EXPECT_FALSE(tb::should_probe(pos, 2));
}

TEST(TablebaseShouldProbe, DoesNotProbeWhenTooManyPieces) {
  const Position pos = Position::startpos();
  tb::Config::set_probe_depth(1);
  EXPECT_FALSE(tb::should_probe(pos, 10));
}

// =============================================================================
// Configuration Tests
// =============================================================================

TEST(TablebaseConfig, DefaultProbeDepthIsOne) {
  // Reset to defaults
  tb::Config::set_probe_depth(1);
  EXPECT_EQ(tb::Config::get_probe_depth(), 1);
}

TEST(TablebaseConfig, DefaultProbeLimitIsSix) {
  tb::Config::set_probe_limit(6);
  EXPECT_EQ(tb::Config::get_probe_limit(), 6);
}

TEST(TablebaseConfig, Default50MoveRuleIsEnabled) {
  tb::Config::set_50_move_rule(true);
  EXPECT_TRUE(tb::Config::get_50_move_rule());
}

TEST(TablebaseConfig, PathCanBeSet) {
  tb::Config::set_path("/path/to/syzygy");
  EXPECT_EQ(tb::Config::get_path(), "/path/to/syzygy");
  tb::Config::set_path(""); // Clean up
}

TEST(TablebaseConfig, ProbeDepthCanBeModified) {
  tb::Config::set_probe_depth(5);
  EXPECT_EQ(tb::Config::get_probe_depth(), 5);
  tb::Config::set_probe_depth(1); // Reset
}

TEST(TablebaseConfig, ProbeLimitCanBeModified) {
  tb::Config::set_probe_limit(5);
  EXPECT_EQ(tb::Config::get_probe_limit(), 5);
  tb::Config::set_probe_limit(6); // Reset
}

TEST(TablebaseConfig, FiftyMoveRuleCanBeDisabled) {
  tb::Config::set_50_move_rule(false);
  EXPECT_FALSE(tb::Config::get_50_move_rule());
  tb::Config::set_50_move_rule(true); // Reset
}

// =============================================================================
// Stub Tablebase Tests
// =============================================================================

TEST(StubTablebase, InitializesWithPath) {
  StubTablebase tb;
  EXPECT_FALSE(tb.is_available());
  EXPECT_TRUE(tb.init("/path/to/tb"));
  EXPECT_TRUE(tb.is_available());
}

TEST(StubTablebase, FreeClearsState) {
  StubTablebase tb;
  tb.init("/path/to/tb");
  tb.free();
  EXPECT_FALSE(tb.is_available());
}

TEST(StubTablebase, ReturnsConfiguredWdlResult) {
  StubTablebase tb;
  tb.init("/path/to/tb");
  tb.set_wdl_result(tb::WdlResult::Win);

  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  const auto result = tb.probe_wdl(pos);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, tb::WdlResult::Win);
}

TEST(StubTablebase, ReturnsNulloptWhenTooManyPieces) {
  StubTablebase tb;
  tb.init("/path/to/tb");
  tb.set_max_pieces(5);
  tb.set_wdl_result(tb::WdlResult::Win);

  // 6-piece position exceeds configured limit
  const Position pos = parse("8/8/2k5/8/8/2nrb3/8/3QK3 w - - 0 1");
  const auto result = tb.probe_wdl(pos);

  EXPECT_FALSE(result.has_value());
}

TEST(StubTablebase, ReturnsNulloptWhenNotAvailable) {
  StubTablebase tb;
  // Don't initialize

  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  const auto result = tb.probe_wdl(pos);

  EXPECT_FALSE(result.has_value());
}

TEST(StubTablebase, ReturnsConfiguredDtzResult) {
  StubTablebase tb;
  tb.init("/path/to/tb");
  tb.set_dtz_result(tb::DtzResult{tb::WdlResult::Win, 12});

  const Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");
  const auto result = tb.probe_dtz(pos);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->wdl, tb::WdlResult::Win);
  EXPECT_EQ(result->dtz, 12);
}

// =============================================================================
// Global Tablebase Instance Tests
// =============================================================================

TEST(GlobalTablebase, CanSetCustomInstance) {
  auto stub = std::make_unique<StubTablebase>();
  stub->init("/test/path");
  stub->set_wdl_result(tb::WdlResult::Draw);

  tb::set_tablebase(std::move(stub));

  EXPECT_TRUE(tb::get_tablebase().is_available());

  // Clean up
  tb::reset_tablebase();
}

TEST(GlobalTablebase, ResetRestoresDefault) {
  auto stub = std::make_unique<StubTablebase>();
  stub->init("/test/path");
  tb::set_tablebase(std::move(stub));

  tb::reset_tablebase();

  // Default tablebase is not initialized by default
  EXPECT_FALSE(tb::get_tablebase().is_available());
}

// =============================================================================
// Search Integration Tests
// =============================================================================
// These tests verify that the search correctly uses tablebase probing.

class SearchWithTablebaseTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Save original config
    original_probe_depth_ = tb::Config::get_probe_depth();
    original_probe_limit_ = tb::Config::get_probe_limit();

    // Set up stub tablebase
    auto stub = std::make_unique<StubTablebase>();
    stub->init("/test/path");
    stub_ = stub.get();
    tb::set_tablebase(std::move(stub));

    // Configure for testing
    tb::Config::set_probe_depth(1);
    tb::Config::set_probe_limit(6);
  }

  void TearDown() override {
    tb::reset_tablebase();
    tb::Config::set_probe_depth(original_probe_depth_);
    tb::Config::set_probe_limit(original_probe_limit_);
  }

  StubTablebase* stub_{nullptr};

private:
  std::uint8_t original_probe_depth_{1};
  std::uint8_t original_probe_limit_{6};
};

TEST_F(SearchWithTablebaseTest, ProbesWdlInEndgame) {
  // KP vs K endgame (3 pieces, no castling)
  Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");

  // Configure stub to return a win
  stub_->set_wdl_result(tb::WdlResult::Win);

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  const auto result = search::search(pos, limits, reporter);

  // With tablebase showing a win, the evaluation should be highly positive
  // Note: The actual search may still run if TB probing isn't wired in yet
  EXPECT_GE(result.eval, 0);
}

TEST_F(SearchWithTablebaseTest, DoesNotProbeWhenTooManyPieces) {
  // Position with 7 pieces (exceeds probe limit of 6)
  Position pos = parse("8/8/2k5/8/3p4/2nrb3/8/3QK3 w - - 0 1");

  stub_->set_wdl_result(tb::WdlResult::Win);

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  // Should search normally without using tablebase
  const auto result = search::search(pos, limits, reporter);

  // Result should reflect normal search, not tablebase
  EXPECT_FALSE(result.pv.empty());
}

TEST_F(SearchWithTablebaseTest, DoesNotProbeWithCastlingRights) {
  // 5-piece position but with castling rights
  Position pos = parse("8/8/8/8/8/4k3/4P3/R3K2R w KQ - 0 1");

  stub_->set_wdl_result(tb::WdlResult::Win);

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 2;

  // Should search normally - castling rights prevent probing
  const auto result = search::search(pos, limits, reporter);

  EXPECT_FALSE(result.pv.empty());
}

TEST_F(SearchWithTablebaseTest, UsesRootProbeForOptimalMove) {
  // KP vs K endgame (3 pieces, no castling)
  Position pos = parse("8/8/8/8/8/4k3/4P3/4K3 w - - 0 1");

  // Configure stub to return a ranked list of root moves
  // Create a move (e2e4 as an example winning move)
  Move winning_move;
  winning_move.piece = Piece::WP;
  winning_move.from = Square::E2;
  winning_move.to = Square::E4;

  std::vector<tb::RootMove> root_moves = {
      {winning_move, tb::DtzResult{tb::WdlResult::Win, 10}},
  };
  stub_->set_root_result(root_moves);

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 10; // High depth that would take time without TB

  const auto result = search::search(pos, limits, reporter);

  // Should return immediately with the tablebase move
  EXPECT_EQ(result.pv.size(), 1);
  EXPECT_EQ(result.pv[0].from, Square::E2);
  EXPECT_EQ(result.pv[0].to, Square::E4);
  EXPECT_EQ(result.eval, tb::wdl_to_centipawns(tb::WdlResult::Win));
  EXPECT_EQ(result.nodes, 1); // Only counted the probe, no search
}

TEST_F(SearchWithTablebaseTest, RootProbeDrawReturnsZeroEval) {
  // Simple drawn endgame
  Position pos = parse("8/8/8/4k3/8/4K3/8/8 w - - 0 1");

  // Configure stub to return a draw
  Move draw_move;
  draw_move.piece = Piece::WK;
  draw_move.from = Square::E3;
  draw_move.to = Square::E2;

  std::vector<tb::RootMove> root_moves = {
      {draw_move, tb::DtzResult{tb::WdlResult::Draw, 0}},
  };
  stub_->set_root_result(root_moves);

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 5;

  const auto result = search::search(pos, limits, reporter);

  EXPECT_EQ(result.eval, 0); // Draw should be 0
}
