#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
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

  HashRng rng(c3::HASH_SEED);
  const auto shift = static_cast<std::uint8_t>(64 - bit_count);

  while (true) {
    const Bitboard candidate = rng.next_sparse();
    std::vector<std::optional<Bitboard>> table(table_size);

    bool collision = false;
    for (std::size_t i = 0; i < table_size; ++i) {
      const auto idx = static_cast<std::size_t>((occupancies[i] * candidate) >> shift);
      if (table[idx].has_value()) {
        collision = true;
        break;
      }
      table[idx] = attacks[i];
    }

    if (!collision) {
      std::vector<Bitboard> flattened;
      flattened.reserve(table_size);
      for (const auto& entry : table) {
        flattened.push_back(*entry);
      }
      return FoundMagic{
          .mask = mask,
          .num = candidate,
          .shift = shift,
          .table = std::move(flattened),
      };
    }
  }
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

  out << "#pragma once\n\n";
  out << "#include <array>\n";
  out << "#include <cstddef>\n";
  out << "#include <cstdint>\n\n";
  out << "namespace c3 {\n\n";
  out << "struct Magic { std::uint64_t mask; std::uint64_t num; std::uint8_t shift; std::size_t "
         "offset; };\n\n";

  write_tables(rook, "ROOK", out);
  write_tables(bishop, "BISHOP", out);

  out << "}  // namespace c3\n";
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
