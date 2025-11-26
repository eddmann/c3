#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace c3::fixtures {

struct PerftRecord {
  std::string name;
  std::string fen;
  int depth{};
  std::uint64_t nodes{};
};

struct EvalRecord {
  std::string name;
  std::string fen;
  int score{};
};

struct ZobristRecord {
  std::string name;
  std::string fen;
  std::uint64_t key{};
};

struct MagicSample {
  std::string piece;  // "rook" or "bishop"
  std::string square; // algebraic like "a1"
  std::uint64_t mask{};
  std::uint64_t num{};
  std::uint8_t shift{};
  std::size_t offset{};
  std::uint64_t occupancy{};
  std::uint64_t attack{};
};

std::filesystem::path fixtures_root();
std::filesystem::path perft_path();
std::filesystem::path eval_path();
std::filesystem::path zobrist_path();
std::filesystem::path magic_path();

std::vector<PerftRecord> load_perft(const std::filesystem::path& file);
std::vector<EvalRecord> load_eval(const std::filesystem::path& file);
std::vector<ZobristRecord> load_zobrist(const std::filesystem::path& file);
std::vector<MagicSample> load_magic_samples(const std::filesystem::path& file);

} // namespace c3::fixtures
