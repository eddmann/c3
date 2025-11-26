#include <gtest/gtest.h>

#include <cstddef>
#include <span>

#include "c3/magic.hpp"
#include "c3/square.hpp"
#include "fixtures.hpp"

using namespace c3;

namespace {

const Magic& select_magic(const fixtures::MagicSample& sample, Square square) {
  if (sample.piece == "rook") {
    return ROOK_MAGICS[square.index()];
  }

  return BISHOP_MAGICS[square.index()];
}

std::span<const std::uint64_t> select_attacks(const fixtures::MagicSample& sample) {
  if (sample.piece == "rook") {
    return ROOK_ATTACKS;
  }

  return BISHOP_ATTACKS;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Magic, GeneratedTablesMatchFixtures) {
  const auto samples = fixtures::load_magic_samples(fixtures::magic_path());

  for (const auto& sample : samples) {
    const auto square = Square::parse(sample.square);
    ASSERT_TRUE(square.has_value()) << "Invalid square in fixtures: " << sample.square;

    const Magic& magic = select_magic(sample, *square);
    EXPECT_EQ(magic.mask, sample.mask) << sample.piece << " " << sample.square;
    EXPECT_EQ(magic.num, sample.num) << sample.piece << " " << sample.square;
    EXPECT_EQ(magic.shift, sample.shift) << sample.piece << " " << sample.square;
    EXPECT_EQ(magic.offset, sample.offset) << sample.piece << " " << sample.square;

    const auto& attacks = select_attacks(sample);
    const std::uint64_t index = (sample.occupancy * magic.num) >> magic.shift;
    ASSERT_LT(magic.offset + index, attacks.size()) << sample.piece << " " << sample.square;

    const std::uint64_t attack = attacks[magic.offset + index];
    EXPECT_EQ(attack, sample.attack) << sample.piece << " " << sample.square;
  }
}
