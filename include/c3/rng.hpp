#pragma once

// =============================================================================
// DETERMINISTIC RANDOM NUMBER GENERATOR
// =============================================================================
//
// Chess engines need random-looking numbers for two purposes:
//
//   1. Zobrist hashing: Random values XORed together create position fingerprints
//   2. Magic bitboards: "Magic" multipliers found via random search
//
// Why deterministic (seeded) randomness instead of true randomness?
//
//   - Reproducibility: Same seed → same hash tables → same engine behavior
//   - Debugging: A bug that only appears with certain hash values can be reproduced
//   - Testing: Engine behavior is predictable across runs and machines
//   - Compile-time: Using constexpr, tables can be computed at compile time
//
// We use xorshift64, a simple PRNG that's fast and produces well-distributed
// values. It's not cryptographically secure, but that's irrelevant for hashing.
//
// =============================================================================

#include <cstdint>

namespace c3 {

// Xorshift64: A fast, simple pseudo-random number generator.
// The three XOR-shift operations ensure good bit mixing with minimal operations.
class HashRng {
public:
  explicit constexpr HashRng(std::uint64_t seed) : state_{seed} {}

  constexpr std::uint64_t next() {
    std::uint64_t x = state_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state_ = x;
    return x;
  }

  // Generate a "sparse" random number (few bits set).
  // ANDing three random numbers together yields ~8 bits set on average (64 * 0.5^3).
  // Sparse numbers are useful for finding magic multipliers, where fewer set bits
  // often produce valid magics faster during the search process.
  constexpr std::uint64_t next_sparse() { return next() & next() & next(); }

private:
  std::uint64_t state_;
};

// The seed value for all hash table generation in this engine.
// The specific value doesn't matter as long as it's non-zero and consistent.
// Using the same seed everywhere ensures all engine instances generate
// identical Zobrist tables, so positions hash the same way universally.
inline constexpr std::uint64_t HASH_SEED = 0xC3C3C3C3C3C3C3C3ull;

} // namespace c3
