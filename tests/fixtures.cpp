#include "fixtures.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace c3::fixtures {
namespace {

std::vector<std::string> split(const std::string& line, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::stringstream stream(line);

  while (std::getline(stream, current, delimiter)) {
    parts.push_back(current);
  }

  return parts;
}

std::vector<std::string> read_records(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open fixture file: " + file.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    lines.push_back(line);
  }

  if (lines.empty()) {
    throw std::runtime_error("Fixture file is empty: " + file.string());
  }

  return lines;
}

} // namespace

std::filesystem::path fixtures_root() {
#ifdef C3_FIXTURE_DIR
  return std::filesystem::path{C3_FIXTURE_DIR};
#else
  return std::filesystem::path{"tests/fixtures"};
#endif
}

std::filesystem::path perft_path() {
  return fixtures_root() / "perft.txt";
}

std::filesystem::path eval_path() {
  return fixtures_root() / "eval.txt";
}

std::filesystem::path zobrist_path() {
  return fixtures_root() / "zobrist.txt";
}

std::filesystem::path magic_path() {
  return fixtures_root() / "magic.txt";
}

std::vector<PerftRecord> load_perft(const std::filesystem::path& file) {
  std::vector<PerftRecord> records;
  for (const auto& line : read_records(file)) {
    const auto parts = split(line, '|');
    if (parts.size() != 4) {
      throw std::runtime_error("Invalid perft record: " + line);
    }

    PerftRecord record;
    record.name = parts[0];
    record.fen = parts[1];
    record.depth = std::stoi(parts[2]);
    record.nodes = std::stoull(parts[3]);
    records.push_back(record);
  }

  return records;
}

std::vector<MagicSample> load_magic_samples(const std::filesystem::path& file) {
  std::vector<MagicSample> records;
  for (const auto& line : read_records(file)) {
    const auto parts = split(line, '|');
    if (parts.size() != 8) {
      throw std::runtime_error("Invalid magic record: " + line);
    }

    MagicSample record;
    record.piece = parts[0];
    record.square = parts[1];
    record.mask = std::stoull(parts[2], nullptr, 16);
    record.num = std::stoull(parts[3], nullptr, 16);
    record.shift = static_cast<std::uint8_t>(std::stoul(parts[4]));
    record.offset = static_cast<std::size_t>(std::stoull(parts[5]));
    record.occupancy = std::stoull(parts[6], nullptr, 16);
    record.attack = std::stoull(parts[7], nullptr, 16);
    records.push_back(record);
  }

  return records;
}

std::vector<EvalRecord> load_eval(const std::filesystem::path& file) {
  std::vector<EvalRecord> records;
  for (const auto& line : read_records(file)) {
    const auto parts = split(line, '|');
    if (parts.size() != 3) {
      throw std::runtime_error("Invalid eval record: " + line);
    }

    EvalRecord record;
    record.name = parts[0];
    record.fen = parts[1];
    record.score = std::stoi(parts[2]);
    records.push_back(record);
  }

  return records;
}

std::vector<ZobristRecord> load_zobrist(const std::filesystem::path& file) {
  std::vector<ZobristRecord> records;
  for (const auto& line : read_records(file)) {
    const auto parts = split(line, '|');
    if (parts.size() != 3) {
      throw std::runtime_error("Invalid zobrist record: " + line);
    }

    ZobristRecord record;
    record.name = parts[0];
    record.fen = parts[1];
    record.key = std::stoull(parts[2], nullptr, 16);
    records.push_back(record);
  }

  return records;
}

} // namespace c3::fixtures
