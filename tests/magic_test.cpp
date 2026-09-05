#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "c3/attacks.hpp"
#include "c3/bitboard.hpp"
#include "c3/board.hpp"
#include "c3/magic.hpp"
#include "c3/piece.hpp"
#include "c3/rng.hpp"
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

// =============================================================================
// A RAY-WALKING ORACLE FOR THE MAGIC TABLES
// =============================================================================
// The fixture samples above pin a dozen hand-checked entries. That catches a
// table that was regenerated with different magics, but not a table that is
// wrong in the same way it was wrong when the fixtures were written.
//
// So the tests below compare the magic lookups against an independent
// implementation: step square by square along each ray and stop after the first
// occupied square, which is itself attacked because it can be captured. This is
// the slow method magic bitboards exist to replace, and its obviousness is the
// point. Anything the two disagree about is a bug in the fast path.
// =============================================================================

struct Direction {
  int file_step;
  int rank_step;
};

constexpr std::array<Direction, 4> ROOK_DIRECTIONS = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
constexpr std::array<Direction, 4> BISHOP_DIRECTIONS = {{{1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};

constexpr bool is_on_board(int file, int rank) {
  return file >= 0 && file <= 7 && rank >= 0 && rank <= 7;
}

Bitboard ray_attacks(Square from, Bitboard occupancy, std::span<const Direction> directions) {
  Bitboard attacks = 0;

  for (const auto direction : directions) {
    int file = from.file() + direction.file_step;
    int rank = from.rank() + direction.rank_step;

    while (is_on_board(file, rank)) {
      const Square square = Square::from_file_and_rank(static_cast<std::uint8_t>(file),
                                                       static_cast<std::uint8_t>(rank));
      attacks |= square;

      if ((occupancy & square) != 0) {
        break;
      }

      file += direction.file_step;
      rank += direction.rank_step;
    }
  }

  return attacks;
}

Board board_with_occupancy(Bitboard occupancy) {
  Board board = Board::empty();

  while (occupancy != 0) {
    board.put_piece(Piece::BN, Square::pop_first_occupied(occupancy));
  }

  return board;
}

// A few hundred blocker patterns per square, sampled the same way on every run
// so a failure can be reproduced. ANDing two random words gives roughly a
// quarter of the board occupied, which is a realistic crowd of blockers.
void assert_lookups_match_oracle(Piece piece, std::span<const Direction> directions) {
  constexpr std::size_t OCCUPANCY_SAMPLES = 256;

  HashRng rng{HASH_SEED};

  for (std::uint8_t index = 0; index < 64; ++index) {
    const Square square = Square::from_index(index);

    for (std::size_t sample = 0; sample < OCCUPANCY_SAMPLES; ++sample) {
      // The first two samples are the extremes: an empty board and a board
      // where every other square blocks.
      const Bitboard random_occupancy = rng.next() & rng.next();
      const Bitboard occupancy = (sample == 0   ? 0
                                  : sample == 1 ? ~Bitboard{0}
                                                : random_occupancy) &
                                 ~Bitboard(square);

      const Board board = board_with_occupancy(occupancy);

      EXPECT_EQ(attacks_for(piece, square, board), ray_attacks(square, occupancy, directions))
          << piece << " on " << square << " with occupancy " << occupancy;
    }
  }
}

} // namespace

TEST(Magic, RookLookupsMatchRayWalkingOracle) {
  assert_lookups_match_oracle(Piece::WR, ROOK_DIRECTIONS);
}

TEST(Magic, BishopLookupsMatchRayWalkingOracle) {
  assert_lookups_match_oracle(Piece::WB, BISHOP_DIRECTIONS);
}

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
