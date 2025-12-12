#include <gtest/gtest.h>

#include "c3/eval.hpp"
#include "c3/position.hpp"
#include "c3/tablebase.hpp"

using namespace c3;
namespace tb = c3::tablebase;

namespace {

Position parse(std::string_view fen) { return Position::from_fen(fen); }

} // namespace

TEST(Tablebase, ConfigDefaultValues) {
  tb::Config config = tb::get_config();
  EXPECT_TRUE(config.path.empty());
  EXPECT_EQ(config.probe_depth, 1);
  EXPECT_EQ(config.probe_limit, 7);
  EXPECT_TRUE(config.use_50_move_rule);
}

TEST(Tablebase, ConfigSetAndGet) {
  tb::Config config;
  config.path = "/some/path";
  config.probe_depth = 5;
  config.probe_limit = 6;
  config.use_50_move_rule = false;

  tb::set_config(config);

  const auto retrieved = tb::get_config();
  EXPECT_EQ(retrieved.path, "/some/path");
  EXPECT_EQ(retrieved.probe_depth, 5);
  EXPECT_EQ(retrieved.probe_limit, 6);
  EXPECT_FALSE(retrieved.use_50_move_rule);

  tb::set_config(tb::Config{});
}

TEST(Tablebase, NotAvailableByDefault) {
  EXPECT_FALSE(tb::is_available());
  EXPECT_EQ(tb::max_pieces(), 0);
}

TEST(Tablebase, ProbeFailsWhenNotAvailable) {
  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 w - - 0 1");
  EXPECT_EQ(tb::probe_wdl(pos), tb::WdlResult::Failed);
  EXPECT_FALSE(tb::probe_root_move(pos).has_value());
}

TEST(Tablebase, WdlToScoreWin) {
  const int score = tb::wdl_to_score(tb::WdlResult::Win, 0);
  EXPECT_EQ(score, tb::CENTIPAWN_TB_WIN);

  const int score_ply5 = tb::wdl_to_score(tb::WdlResult::Win, 5);
  EXPECT_EQ(score_ply5, tb::CENTIPAWN_TB_WIN - 5);
  EXPECT_LT(score_ply5, score);
}

TEST(Tablebase, WdlToScoreLoss) {
  const int score = tb::wdl_to_score(tb::WdlResult::Loss, 0);
  EXPECT_EQ(score, -tb::CENTIPAWN_TB_WIN);

  const int score_ply5 = tb::wdl_to_score(tb::WdlResult::Loss, 5);
  EXPECT_EQ(score_ply5, -tb::CENTIPAWN_TB_WIN + 5);
  EXPECT_GT(score_ply5, score);
}

TEST(Tablebase, WdlToScoreDraw) {
  EXPECT_EQ(tb::wdl_to_score(tb::WdlResult::Draw, 0), CENTIPAWN_DRAW);
  EXPECT_EQ(tb::wdl_to_score(tb::WdlResult::Draw, 10), CENTIPAWN_DRAW);
}

TEST(Tablebase, WdlToScoreCursedWin) {
  const int score = tb::wdl_to_score(tb::WdlResult::CursedWin, 0);
  EXPECT_GT(score, CENTIPAWN_DRAW);
  EXPECT_LT(score, tb::CENTIPAWN_TB_WIN);
}

TEST(Tablebase, WdlToScoreBlessedLoss) {
  const int score = tb::wdl_to_score(tb::WdlResult::BlessedLoss, 0);
  EXPECT_LT(score, CENTIPAWN_DRAW);
  EXPECT_GT(score, -tb::CENTIPAWN_TB_WIN);
}

TEST(Tablebase, ProbeRejectsCastlingRights) {
  Position pos = parse("8/8/8/8/8/8/8/R3K2R w KQ - 0 1");
  EXPECT_EQ(tb::probe_wdl(pos), tb::WdlResult::Failed);
}

TEST(Tablebase, ProbeRespectsProbeLimit) {
  tb::Config config;
  config.probe_limit = 3;
  tb::set_config(config);

  Position pos = parse("8/8/8/8/8/3k4/4P3/R3K3 w - - 0 1");

  EXPECT_EQ(tb::probe_wdl(pos), tb::WdlResult::Failed);

  tb::set_config(tb::Config{});
}

TEST(Tablebase, TbWinScoreBelowMate) {
  EXPECT_LT(tb::CENTIPAWN_TB_WIN, CENTIPAWN_MATE_THRESHOLD);
}

TEST(Tablebase, TbWinScoreAboveNormalEval) {
  EXPECT_GT(tb::CENTIPAWN_TB_WIN, 8500);
}

class TablebaseWithFiles : public ::testing::Test {
protected:
  void SetUp() override {
    const std::vector<std::string> paths = {
        "/syzygy",
        "/opt/syzygy",
        "./syzygy",
        "../syzygy",
        "../../syzygy",
        "../../../syzygy",
    };

    for (const auto& path : paths) {
      tb::Config config;
      config.path = path;
      tb::set_config(config);
      if (tb::init() > 0) {
        tb_available_ = true;
        return;
      }
    }

    tb_available_ = false;
  }

  void TearDown() override {
    tb::free();
    tb::set_config(tb::Config{});
  }

  bool tb_available_{false};
};

TEST_F(TablebaseWithFiles, KRvK_WhiteWins) {
  if (!tb_available_) {
    GTEST_SKIP() << "Syzygy tablebases not found";
  }

  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 w - - 0 1");

  const auto wdl = tb::probe_wdl(pos);
  EXPECT_EQ(wdl, tb::WdlResult::Win);
}

TEST_F(TablebaseWithFiles, KRvK_BlackLoses) {
  if (!tb_available_) {
    GTEST_SKIP() << "Syzygy tablebases not found";
  }

  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 b - - 0 1");

  const auto wdl = tb::probe_wdl(pos);
  EXPECT_EQ(wdl, tb::WdlResult::Loss);
}

TEST_F(TablebaseWithFiles, KvK_Draw) {
  if (!tb_available_) {
    GTEST_SKIP() << "Syzygy tablebases not found";
  }

  Position pos = parse("8/8/8/4k3/8/8/8/4K3 w - - 0 1");

  const auto wdl = tb::probe_wdl(pos);
  EXPECT_EQ(wdl, tb::WdlResult::Draw);
}

TEST_F(TablebaseWithFiles, RootProbeReturnsMove) {
  if (!tb_available_) {
    GTEST_SKIP() << "Syzygy tablebases not found";
  }

  Position pos = parse("8/8/8/8/8/3k4/8/R3K3 w - - 0 1");

  const auto move = tb::probe_root_move(pos);
  ASSERT_TRUE(move.has_value());

  EXPECT_TRUE(move->piece == Piece::WR || move->piece == Piece::WK);
}
