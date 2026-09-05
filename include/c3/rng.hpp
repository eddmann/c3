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
// The two uses want different things from it, though, and next_scrambled()
// below explains why the Zobrist tables take an extra step that the magic
// search does not.
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

  // ===========================================================================
  // SCRAMBLED OUTPUT: what Zobrist keys need and plain xorshift cannot give
  // ===========================================================================
  // next() above is a purely LINEAR function over GF(2)—the field with two
  // elements, where XOR is addition. Every step is a shift and an XOR, and both
  // are linear, so the whole generator is "multiply the state by a fixed 64x64
  // bit matrix". Two consequences follow, and both are bad for hashing:
  //
  //   1. The outputs are linear in the SEED. Run the generator from seed A,
  //      from seed B, and from seed A^B, and the third stream is the XOR of the
  //      first two, output for output.
  //
  //   2. The stream obeys a FIXED linear recurrence of order 64. Some fixed
  //      subset of any 64 consecutive outputs always XORs to the 65th—the same
  //      subset every time, whatever the seed.
  //
  // Point 2 is the one that bites. A Zobrist key set is used by XORing keys
  // together, so a subset of keys that XORs to zero is a pair of DIFFERENT
  // positions with the SAME hash. With a linear generator such subsets are not
  // bad luck, they are built in: pick any 65 consecutive keys and one exists,
  // by construction, in every build of the engine. The transposition table
  // would then confidently hand one position's score to another.
  //
  // The fix is the "*" in xorshift64*: multiply the output by a large odd
  // constant. Multiplication carries bits upward between positions, which XOR
  // and shifts never do, so the output stops being a linear function of the
  // state. The internal state still advances exactly as before—the full period
  // and the equidistribution of xorshift64 are untouched—but the relations
  // above no longer survive the trip to the caller.
  //
  // 0x2545F4914F6CDD1D is Vigna's published xorshift64* multiplier.
  //
  // Only the Zobrist tables use this. The magic bitboard generator deliberately
  // stays on the raw next()/next_sparse() stream: it does not care about
  // linearity (it just needs candidate multipliers to try), and changing the
  // numbers it draws would silently produce a DIFFERENT set of magics, which
  // means a different generated magic.hpp for no gain.
  // ===========================================================================
  constexpr std::uint64_t next_scrambled() { return next() * 0x2545F4914F6CDD1Dull; }

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
