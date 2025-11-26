#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "fixtures.hpp"

namespace {

template <class T, class Pred> const T* find_by(const std::vector<T>& records, Pred pred) {
  const auto it = std::find_if(records.begin(), records.end(), pred);
  return it == records.end() ? nullptr : &*it;
}

} // namespace

TEST(Fixtures, LoadPerftStartPositionDepth4) {
  const auto records = c3::fixtures::load_perft(c3::fixtures::perft_path());
  ASSERT_FALSE(records.empty());

  const auto* record = find_by(records, [](const auto& r) { return r.name == "startpos-d4"; });
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->depth, 4);
  EXPECT_EQ(record->nodes, 197281U);
}

TEST(Fixtures, LoadEvalKeepsSideToMoveSign) {
  const auto records = c3::fixtures::load_eval(c3::fixtures::eval_path());

  const auto* white = find_by(records, [](const auto& r) { return r.name == "white-rook-king"; });
  const auto* black =
      find_by(records, [](const auto& r) { return r.name == "black-to-move-same"; });

  ASSERT_NE(white, nullptr);
  ASSERT_NE(black, nullptr);

  EXPECT_GT(white->score, 0);
  EXPECT_EQ(black->score, -white->score);
}

TEST(Fixtures, LoadZobristStartPosition) {
  const auto records = c3::fixtures::load_zobrist(c3::fixtures::zobrist_path());

  const auto* record = find_by(records, [](const auto& r) { return r.name == "startpos"; });
  ASSERT_NE(record, nullptr);

  EXPECT_EQ(record->key, 0xd9189e710b0d5138ULL);
}
