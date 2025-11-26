#include <gtest/gtest.h>

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
