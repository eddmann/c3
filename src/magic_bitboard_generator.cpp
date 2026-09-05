// =============================================================================
// MAGIC BITBOARD TABLE GENERATOR
// =============================================================================
//
// Finds, for every square, a "magic" 64-bit multiplier that turns the relevant
// occupancy bits of a rook or bishop ray into a dense table index, then writes
// the resulting tables out as include/c3/magic.hpp.
//
// The search is brute force: draw a random candidate, try to index the whole
// occupancy set with it, and keep the first candidate that produces no index
// collisions. That is the standard technique, and it works because valid magics
// are common enough that a few thousand draws normally find one.
//
// "Normally" is doing real work in that sentence, which is why the search below
// is bounded rather than a bare while(true): see find_magic_for_square.
//
// This program is only built and run when the build is configured with
// -DC3_REGENERATE_MAGIC=ON. Its output is checked in, so a normal build never
// pays for the search.
//
// =============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "c3/bitboard.hpp"
#include "c3/rng.hpp"
#include "c3/square.hpp"

namespace c3::magicgen {

using c3::Bitboard;
using c3::HashRng;
using c3::Square;

struct GeneratorMagic {
  Bitboard mask{};
  Bitboard num{};
  std::uint8_t shift{};
  std::size_t offset{};
};

struct FoundMagic {
  Bitboard mask{};
  Bitboard num{};
  std::uint8_t shift{};
  std::vector<Bitboard> table;
};

struct BuiltTables {
  std::array<GeneratorMagic, 64> magics{};
  std::vector<Bitboard> attacks;
};

Bitboard rook_mask(Square square) {
  Bitboard mask = 0;
  const int file = square.file();
  const int rank = square.rank();

  for (int r = rank + 1; r <= 6; ++r) {
    mask |=
        Square::from_file_and_rank(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(r));
  }

  for (int r = rank - 1; r >= 1; --r) {
    mask |=
        Square::from_file_and_rank(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(r));
  }

  for (int f = file - 1; f >= 1; --f) {
    mask |=
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(rank));
  }

  for (int f = file + 1; f <= 6; ++f) {
    mask |=
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(rank));
  }

  return mask;
}

Bitboard bishop_mask(Square square) {
  Bitboard mask = 0;
  const int file = square.file();
  const int rank = square.rank();

  // Up-right
  for (int f = file + 1, r = rank + 1; f <= 6 && r <= 6; ++f, ++r) {
    mask |= Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
  }

  // Up-left
  for (int f = file - 1, r = rank + 1; f >= 1 && r <= 6; --f, ++r) {
    mask |= Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
  }

  // Down-right
  for (int f = file + 1, r = rank - 1; f <= 6 && r >= 1; ++f, --r) {
    mask |= Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
  }

  // Down-left
  for (int f = file - 1, r = rank - 1; f >= 1 && r >= 1; --f, --r) {
    mask |= Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
  }

  return mask;
}

Bitboard rook_attacks(Square square, Bitboard occupancy) {
  Bitboard attacks = 0;
  const int file = square.file();
  const int rank = square.rank();

  for (int r = rank + 1; r < 8; ++r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  for (int r = rank - 1; r >= 0; --r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  for (int f = file + 1; f < 8; ++f) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(rank));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  for (int f = file - 1; f >= 0; --f) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(rank));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  return attacks;
}

Bitboard bishop_attacks(Square square, Bitboard occupancy) {
  Bitboard attacks = 0;
  const int file = square.file();
  const int rank = square.rank();

  // Up-right
  for (int f = file + 1, r = rank + 1; f < 8 && r < 8; ++f, ++r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  // Up-left
  for (int f = file - 1, r = rank + 1; f >= 0 && r < 8; --f, ++r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  // Down-right
  for (int f = file + 1, r = rank - 1; f < 8 && r >= 0; ++f, --r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  // Down-left
  for (int f = file - 1, r = rank - 1; f >= 0 && r >= 0; --f, --r) {
    const Bitboard sq =
        Square::from_file_and_rank(static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(r));
    attacks |= sq;
    if ((occupancy & sq) != 0ULL) {
      break;
    }
  }

  return attacks;
}

std::vector<Bitboard> bit_positions(Bitboard mask) {
  std::vector<Bitboard> bits;
  while (mask != 0) {
    const Bitboard lsb = mask & (~mask + 1); // lowest set bit
    bits.push_back(lsb);
    mask ^= lsb;
  }
  return bits;
}

Bitboard bit_permutation_from_index(std::size_t index, const std::vector<Bitboard>& bits) {
  Bitboard occupancy = 0;
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (((index >> i) & 1U) != 0U) {
      occupancy |= bits[i];
    }
  }
  return occupancy;
}

// =============================================================================
// CANDIDATE MAGICS
// =============================================================================
// Magic candidates are drawn SPARSE—the AND of three random words, so roughly
// eight bits set out of sixty-four. That is not superstition: the multiply has
// to fold the occupancy bits into the top of the word without them landing on
// top of each other, and a candidate with few set bits creates far fewer
// overlapping partial products, so it succeeds far more often than a dense one.
//
// The number of terms ANDed together is a knob rather than a constant only so
// that the search has somewhere to go if sparse candidates run out; see
// find_magic_for_square.
// =============================================================================

struct CandidateSource {
  HashRng rng;
  int terms{3}; // 3 = sparse (the default), 2 = less so, 1 = a plain draw

  Bitboard next() {
    // AND is commutative, so this consumes the same three draws, in the same
    // order, as HashRng::next_sparse() does at terms == 3. Keeping that exact
    // is what lets the generator reproduce the checked-in magic.hpp.
    Bitboard candidate = rng.next();
    for (int term = 1; term < terms; ++term) {
      candidate &= rng.next();
    }
    return candidate;
  }
};

// =============================================================================
// BOUNDING THE SEARCH
// =============================================================================
// A valid magic exists for every square and the sparse search finds one within
// a few thousand draws, so in practice the first round below always succeeds.
// The bound is not there for the expected case; it is there because the
// alternative is a bare while(true), and a bare while(true) turns any future
// mistake—a wrong mask, a bad shift, a generator seeded to a degenerate
// state—into a build that hangs forever with no output. A build that stops and
// says which square defeated it is worth the handful of lines.
//
// Each round that runs out of attempts falls back rather than giving up:
// reseeding moves the search to a different part of the candidate space, and
// widening (fewer terms ANDed together) makes candidates denser, which is the
// right direction if a square somehow has no sparse magic at all. Only when
// every round is exhausted does the generator fail, and it fails loudly.
// =============================================================================

inline constexpr std::size_t MAX_ATTEMPTS_PER_ROUND = 1'000'000;

// How sparse each round's candidates are: three rounds of the usual sparse
// draws from three different seeds, then two rounds of denser ones, then a
// round of plain random words.
inline constexpr std::array<int, 6> ROUND_TERMS = {3, 3, 3, 2, 2, 1};
inline constexpr int SEARCH_ROUNDS = static_cast<int>(ROUND_TERMS.size());

FoundMagic find_magic_for_square(Square square, const std::function<Bitboard(Square)>& mask_fn,
                                 const std::function<Bitboard(Square, Bitboard)>& attacks_fn) {
  const Bitboard mask = mask_fn(square);
  const auto occupancy_bits = bit_positions(mask);
  const auto bit_count = static_cast<std::uint8_t>(std::popcount(mask));
  const auto table_size = 1ULL << bit_count;

  std::vector<Bitboard> occupancies;
  occupancies.reserve(table_size);
  std::vector<Bitboard> attacks;
  attacks.reserve(table_size);

  for (std::size_t index = 0; index < table_size; ++index) {
    const Bitboard occupancy = bit_permutation_from_index(index, occupancy_bits);
    occupancies.push_back(occupancy);
    attacks.push_back(attacks_fn(square, occupancy));
  }

  const auto shift = static_cast<std::uint8_t>(64 - bit_count);

  // The candidate table and its "which attempt last wrote this slot" stamps are
  // allocated ONCE and reused across every attempt. The obvious version clears
  // the table between attempts, which is a memset of up to 4096 entries per
  // candidate and dominates the cost of the search; bumping a counter instead
  // makes every stale slot invalid for free. The stamp cannot wrap: the whole
  // search makes at most SEARCH_ROUNDS * MAX_ATTEMPTS_PER_ROUND attempts, far
  // below the range of a 32-bit counter.
  std::vector<Bitboard> table(table_size);
  std::vector<std::uint32_t> written_on_attempt(table_size, 0);
  std::uint32_t attempt_stamp = 0;

  for (int round = 0; round < SEARCH_ROUNDS; ++round) {
    // Round 0 is the sparse search from the standard seed—the one that produced
    // the checked-in tables. Later rounds only exist as an escape hatch.
    CandidateSource candidates{
        .rng = HashRng(c3::HASH_SEED + static_cast<std::uint64_t>(round)),
        .terms = ROUND_TERMS[static_cast<std::size_t>(round)],
    };

    for (std::size_t attempt = 0; attempt < MAX_ATTEMPTS_PER_ROUND; ++attempt) {
      const Bitboard candidate = candidates.next();
      ++attempt_stamp;

      bool collision = false;
      for (std::size_t i = 0; i < table_size; ++i) {
        const auto idx = static_cast<std::size_t>((occupancies[i] * candidate) >> shift);
        if (written_on_attempt[idx] == attempt_stamp) {
          collision = true;
          break;
        }
        written_on_attempt[idx] = attempt_stamp;
        table[idx] = attacks[i];
      }

      // No collisions means the multiply mapped the table_size occupancies onto
      // the table_size indices one-to-one, so every slot now holds its attacks.
      if (!collision) {
        return FoundMagic{
            .mask = mask,
            .num = candidate,
            .shift = shift,
            .table = std::move(table),
        };
      }
    }
  }

  throw std::runtime_error("no magic found for square " + square.to_string() + " after " +
                           std::to_string(SEARCH_ROUNDS) + " rounds");
}

BuiltTables build_magics(const std::function<Bitboard(Square)>& mask_fn,
                         const std::function<Bitboard(Square, Bitboard)>& attacks_fn) {
  BuiltTables result;

  for (std::uint8_t index = 0; index < 64; ++index) {
    const Square square = Square::from_index(index);
    const FoundMagic found = find_magic_for_square(square, mask_fn, attacks_fn);

    GeneratorMagic magic{
        .mask = found.mask,
        .num = found.num,
        .shift = found.shift,
        .offset = result.attacks.size(),
    };

    result.magics[index] = magic;
    result.attacks.insert(result.attacks.end(), found.table.begin(), found.table.end());
  }

  return result;
}

std::string magic_to_string(const GeneratorMagic& magic) {
  std::ostringstream os;
  os << "    Magic{0x" << std::hex << magic.mask << "ull, 0x" << magic.num << "ull, " << std::dec
     << static_cast<unsigned>(magic.shift) << ", " << magic.offset << "},\n";
  return os.str();
}

std::string attacks_to_string(std::span<const Bitboard> attacks) {
  std::ostringstream os;
  os << std::hex;
  for (Bitboard attack : attacks) {
    os << "    0x" << attack << "ull,\n";
  }
  return os.str();
}

void write_tables(const BuiltTables& tables, const std::string& name, std::ostream& out) {
  out << "inline constexpr std::array<Magic, 64> " << name << "_MAGICS = {\n";
  for (const auto& magic : tables.magics) {
    out << magic_to_string(magic);
  }
  out << "};\n\n";

  out << "inline constexpr std::array<std::uint64_t, " << tables.attacks.size() << "> " << name
      << "_ATTACKS = {\n";
  out << attacks_to_string(tables.attacks);
  out << "};\n\n";
}

void write_header(const BuiltTables& rook, const BuiltTables& bishop,
                  const std::filesystem::path& out_path) {
  if (const auto parent = out_path.parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream out(out_path);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to open output file: " + out_path.string());
  }

  // Everything below is written in the shape clang-format would produce, so
  // that regenerating the header and running the repo's formatter over it are
  // the same thing. Emitting a "close enough" layout instead would make every
  // regeneration show up as a large formatting diff on top of the real one.
  out << "#pragma once\n\n";
  out << "// Generated by c3_magic_bitboard_generator - do not edit; regenerate with\n";
  out << "// -DC3_REGENERATE_MAGIC=ON\n\n";
  out << "#include <array>\n";
  out << "#include <cstddef>\n";
  out << "#include <cstdint>\n\n";
  out << "namespace c3 {\n\n";
  out << "struct Magic {\n";
  out << "  std::uint64_t mask;\n";
  out << "  std::uint64_t num;\n";
  out << "  std::uint8_t shift;\n";
  out << "  std::size_t offset;\n";
  out << "};\n\n";

  write_tables(rook, "ROOK", out);
  write_tables(bishop, "BISHOP", out);

  out << "} // namespace c3\n";
}

} // namespace c3::magicgen

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <output_path>\n";
    return 1;
  }

  try {
    const auto rook_tables =
        c3::magicgen::build_magics(c3::magicgen::rook_mask, c3::magicgen::rook_attacks);
    const auto bishop_tables =
        c3::magicgen::build_magics(c3::magicgen::bishop_mask, c3::magicgen::bishop_attacks);
    c3::magicgen::write_header(rook_tables, bishop_tables, std::filesystem::path{argv[1]});
  } catch (const std::exception& ex) {
    std::cerr << "magic generation failed: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
