#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "c3/piece.hpp"
#include "c3/position.hpp"
#include "c3/square.hpp"
#include "c3/zobrist.hpp"
#include "fixtures.hpp"

using namespace c3;

TEST(Zobrist, MatchesFixtureKeys) {
  const auto records = fixtures::load_zobrist(fixtures::zobrist_path());

  for (const auto& record : records) {
    const auto pos = Position::from_fen(record.fen);
    EXPECT_EQ(pos.key, record.key) << "fixture mismatch for " << record.name;
  }
}

TEST(Zobrist, IncrementalToggleMatchesRecompute) {
  Position pos = Position::startpos();
  const auto original_key = pos.key;

  const Square from = Square::E2;
  const Square to = Square::E4;
  const Piece pawn_piece = Piece::WP;

  pos.board.remove_piece(from);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][from.index()];

  pos.board.put_piece(pawn_piece, to);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][to.index()];

  pos.en_passant_square = Square::from_file_and_rank(4, 2); // e3
  pos.colour_to_move = Colour::Black;
  pos.key ^= ZOBRIST.colour_to_move;

  EXPECT_EQ(pos.key, pos.compute_key());

  pos.key ^= ZOBRIST.colour_to_move;
  pos.colour_to_move = Colour::White;
  pos.en_passant_square = std::nullopt;

  pos.board.remove_piece(to);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][to.index()];

  pos.board.put_piece(pawn_piece, from);
  pos.key ^= ZOBRIST.piece_square[static_cast<std::size_t>(pawn_piece)][from.index()];

  EXPECT_EQ(pos.key, pos.compute_key());
  EXPECT_EQ(pos.key, original_key);
}

// -----------------------------------------------------------------------------
// Key quality: what a Zobrist table is allowed to look like
// -----------------------------------------------------------------------------

namespace {

// The table in the order make_zobrist_table() drew it. The draw order matters
// here in a way it never does at run time: a generator's structural weaknesses
// are relationships between CONSECUTIVE outputs, so they are only visible if we
// line the keys back up the way the generator produced them.
std::vector<std::uint64_t> keys_in_draw_order() {
  std::vector<std::uint64_t> keys;
  keys.reserve(793);

  for (const auto piece : all_pieces()) {
    for (std::uint8_t file = 0; file < 8; ++file) {
      for (std::uint8_t rank = 0; rank < 8; ++rank) {
        const auto square = Square::from_file_and_rank(file, rank);
        keys.push_back(ZOBRIST.piece_square[static_cast<std::size_t>(piece)][square.index()]);
      }
    }
  }

  keys.push_back(ZOBRIST.colour_to_move);
  keys.insert(keys.end(), ZOBRIST.castling_rights.begin(), ZOBRIST.castling_rights.end());
  keys.insert(keys.end(), ZOBRIST.en_passant_files.begin(), ZOBRIST.en_passant_files.end());

  return keys;
}

// Treating 64-bit keys as vectors over GF(2) (XOR is addition, there is no
// other operation), find a subset of keys[start .. start+63] whose XOR equals
// keys[start+64]. Textbook Gaussian elimination: reduce each key against the
// basis built so far, and remember which originals went into each basis vector.
//
// Such a subset ALWAYS exists—65 vectors cannot be independent in a 64-
// dimensional space—so finding one proves nothing on its own. What the tests
// below do with it is the interesting part.
std::uint64_t solve_for_next_key(const std::vector<std::uint64_t>& keys, std::size_t start) {
  struct BasisVector {
    std::uint64_t value;
    std::uint64_t sources; // bit i set = keys[start + i] contributed
  };

  std::vector<BasisVector> basis;

  const auto reduce = [&basis](std::uint64_t value, std::uint64_t sources) {
    for (const auto& vector : basis) {
      const auto pivot = static_cast<unsigned>(std::bit_width(vector.value)) - 1U;
      if (((value >> pivot) & 1U) != 0U) {
        value ^= vector.value;
        sources ^= vector.sources;
      }
    }
    return BasisVector{value, sources};
  };

  for (std::size_t i = 0; i < 64; ++i) {
    const auto reduced = reduce(keys[start + i], std::uint64_t{1} << i);
    if (reduced.value != 0) {
      basis.push_back(reduced);
      std::ranges::sort(basis, std::ranges::greater{}, &BasisVector::value);
    }
  }

  const auto solution = reduce(keys[start + 64], 0);
  return solution.value == 0 ? solution.sources : 0;
}

// Does "XOR of the selected keys equals the 65th" hold at this offset?
bool relation_holds(const std::vector<std::uint64_t>& keys, std::size_t start,
                    std::uint64_t sources) {
  std::uint64_t total = keys[start + 64];
  for (std::size_t i = 0; i < 64; ++i) {
    if (((sources >> i) & 1U) != 0U) {
      total ^= keys[start + i];
    }
  }
  return total == 0;
}

} // namespace

TEST(Zobrist, KeysDoNotObeyAFixedLinearRecurrence) {
  // THE DEFECT THIS GUARDS AGAINST. A plain xorshift generator is a linear map
  // over GF(2): every output is a fixed matrix times the previous state. That
  // makes its output stream obey a linear recurrence of order 64—a fixed subset
  // of any 64 consecutive outputs always XORs to the 65th, with the same subset
  // every time, for every seed. Translated into Zobrist terms, that is a set of
  // (piece, square) placements that XOR to exactly zero: two genuinely
  // different positions with the same hash, built into the table by
  // construction rather than by bad luck.
  //
  // The test works by finding such a subset in the first 65 keys and then
  // asking whether the SAME subset also works elsewhere in the table. For a
  // linear generator it does, everywhere. For a scrambled one it does not: the
  // first 65 keys are still dependent (any 65 vectors in 64 dimensions are),
  // but the relation is an accident of those particular keys and says nothing
  // about the rest.
  const auto keys = keys_in_draw_order();
  const auto sources = solve_for_next_key(keys, 0);

  ASSERT_NE(sources, 0U) << "65 keys must be linearly dependent; the solver is broken";
  ASSERT_TRUE(relation_holds(keys, 0, sources)) << "the solver returned a wrong subset";

  for (const std::size_t offset : {100U, 300U, 500U, 700U}) {
    EXPECT_FALSE(relation_holds(keys, offset, sources))
        << "the relation found at offset 0 also holds at offset " << offset
        << ": the key generator is linear, so its keys carry built-in collisions";
  }
}

TEST(Zobrist, NoKeyIsRepeatedAndNoPairXorsToAThird) {
  // A cheap sanity sweep over the whole table: every key distinct, and no key
  // equal to the XOR of two others.
  //
  // WHAT THIS PROVES: those two specific shapes of collision are absent. A
  // repeated key would make two placements interchangeable; a weight-three
  // relation would mean placement A plus placement B hash the same as
  // placement C.
  //
  // WHAT THIS DOES NOT PROVE: that the keys are linearly independent. They
  // cannot be—793 vectors in a 64-dimensional space are always dependent, so
  // relations DO exist and this test would not see them. It also says nothing
  // about relations of four keys or more. The test above is the one that cares
  // whether those relations are structural; this one only rules out the two
  // cases small enough to check exhaustively.
  const auto keys = keys_in_draw_order();
  const std::unordered_set<std::uint64_t> distinct(keys.begin(), keys.end());

  EXPECT_EQ(distinct.size(), keys.size()) << "two Zobrist keys are identical";

  for (std::size_t i = 0; i < keys.size(); ++i) {
    for (std::size_t j = i + 1; j < keys.size(); ++j) {
      EXPECT_EQ(distinct.count(keys[i] ^ keys[j]), 0U)
          << "keys " << i << " and " << j << " XOR to a third key";
    }
  }
}
