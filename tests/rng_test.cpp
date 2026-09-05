#include <gtest/gtest.h>

#include <cstdint>

#include "c3/rng.hpp"

using namespace c3;

TEST(HashRng, ProducesExpectedSequence) {
  HashRng rng(0xC3C3C3C3C3C3C3C3ULL);

  EXPECT_EQ(rng.next(), 0x2355555565c4d484ULL);
  EXPECT_EQ(rng.next(), 0x84d04f4bdcf0fc2dULL);
  EXPECT_EQ(rng.next(), 0x015b975b05d9b695ULL);
  EXPECT_EQ(rng.next(), 0x5f4870d5f21d00b8ULL);
  EXPECT_EQ(rng.next(), 0x87e6965757dc14b9ULL);
}

TEST(HashRng, NextSparseUsesThreeSteps) {
  HashRng rng(0xC3C3C3C3C3C3C3C3ULL);

  EXPECT_EQ(rng.next_sparse(), 0x0050054104c09404ULL);
  EXPECT_EQ(rng.next_sparse(), 0x0500101440000090ULL);
}

TEST(HashRng, RawOutputIsLinearInTheSeed) {
  // This is the property that makes plain xorshift unsuitable for Zobrist keys,
  // stated as an equation: the generator is a linear map over GF(2), so running
  // it from seed A, from seed B and from seed A^B gives three streams where the
  // third is the XOR of the first two, output for output.
  //
  // Nothing here is broken—next() is documented as raw xorshift64 and the magic
  // generator depends on exactly these numbers. The test exists so that the
  // defect the scrambler fixes is visible rather than merely asserted in a
  // comment.
  constexpr std::uint64_t first_seed = 0xC3C3C3C3C3C3C3C3ULL;
  constexpr std::uint64_t second_seed = 0x0123456789ABCDEFULL;

  HashRng first(first_seed);
  HashRng second(second_seed);
  HashRng combined(first_seed ^ second_seed);

  for (int draw = 0; draw < 8; ++draw) {
    EXPECT_EQ(first.next() ^ second.next(), combined.next()) << "draw " << draw;
  }
}

TEST(HashRng, ScramblingBreaksSeedLinearity) {
  // The same experiment through next_scrambled(). The multiply carries bits
  // upward between positions, which XOR and shifts never do, so the equation
  // above stops holding—which is the whole point of the extra step.
  constexpr std::uint64_t first_seed = 0xC3C3C3C3C3C3C3C3ULL;
  constexpr std::uint64_t second_seed = 0x0123456789ABCDEFULL;

  HashRng first(first_seed);
  HashRng second(second_seed);
  HashRng combined(first_seed ^ second_seed);

  int linear_draws = 0;
  for (int draw = 0; draw < 8; ++draw) {
    if ((first.next_scrambled() ^ second.next_scrambled()) == combined.next_scrambled()) {
      ++linear_draws;
    }
  }

  EXPECT_EQ(linear_draws, 0);
}

TEST(HashRng, ScrambledSequenceIsPinned) {
  // The Zobrist tables are built from this stream, so pinning it pins every
  // key in the engine. Each value is the matching next() output above
  // multiplied by 0x2545F4914F6CDD1D.
  HashRng rng(0xC3C3C3C3C3C3C3C3ULL);

  EXPECT_EQ(rng.next_scrambled(), 0xa0623ee9d67206f4ULL);
  EXPECT_EQ(rng.next_scrambled(), 0x0b37c64d56fb6a19ULL);
  EXPECT_EQ(rng.next_scrambled(), 0xbbbdcef29e244fe1ULL);
  EXPECT_EQ(rng.next_scrambled(), 0x854b59ec8c87ecd8ULL);
  EXPECT_EQ(rng.next_scrambled(), 0xb0237ac4c6de0df5ULL);
}

TEST(HashRng, ScramblingDoesNotDisturbTheRawStream) {
  // The magic bitboard generator draws from next()/next_sparse(). If the
  // scrambler had been bolted onto next() itself, every magic in the checked-in
  // magic.hpp would silently become wrong on the next regeneration, so the two
  // streams are pinned side by side: same state advance, different output.
  HashRng raw(0xC3C3C3C3C3C3C3C3ULL);
  HashRng scrambled(0xC3C3C3C3C3C3C3C3ULL);

  for (int draw = 0; draw < 5; ++draw) {
    const auto raw_value = raw.next();
    const auto scrambled_value = scrambled.next_scrambled();
    EXPECT_EQ(scrambled_value, raw_value * 0x2545F4914F6CDD1DULL) << "draw " << draw;
  }
}
