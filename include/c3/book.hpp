#pragma once

// =============================================================================
// POLYGLOT OPENING BOOK SUPPORT
// =============================================================================
//
// Polyglot is an industry-standard binary format for chess opening books.
// Each 16-byte entry contains:
//   - 64-bit Zobrist key (Polyglot-specific, NOT the engine's Zobrist)
//   - 16-bit encoded move
//   - 16-bit weight (for probabilistic selection)
//   - 32-bit learn data (often unused)
//
// The key insight: Polyglot uses standardized Zobrist random values that differ
// from our engine's HASH_SEED-based Zobrist table. We must compute a separate
// "Polyglot key" for each position to probe the book.
//
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "c3/move.hpp"
#include "c3/position.hpp"

namespace c3 {

// Polyglot book entry - 16 bytes, stored big-endian in file
struct PolyglotEntry {
  std::uint64_t key;    // Polyglot Zobrist hash
  std::uint16_t move;   // Encoded move (see decode_polyglot_move)
  std::uint16_t weight; // Selection weight (higher = more likely)
  std::uint32_t learn;  // Learning data (typically unused)

  // For sorting/searching by key
  bool operator<(const PolyglotEntry& other) const { return key < other.key; }
  bool operator<(std::uint64_t k) const { return key < k; }
  friend bool operator<(std::uint64_t k, const PolyglotEntry& e) { return k < e.key; }
};

// Opening book interface supporting Polyglot .bin format
class OpeningBook {
public:
  OpeningBook() = default;

  // Load a Polyglot book from disk. Returns false if file cannot be read
  // or is invalid (e.g., size not multiple of 16 bytes).
  bool load(const std::filesystem::path& path);

  // Unload the current book, freeing memory
  void unload();

  // Check if a book is currently loaded
  [[nodiscard]] bool is_loaded() const { return loaded_; }

  // Probe for a book move in the given position.
  // Returns nullopt if no matching entry is found.
  // Uses weighted random selection when multiple moves exist.
  [[nodiscard]] std::optional<Move> probe(const Position& pos) const;

  // Get all book moves for a position with their weights (for analysis/debugging)
  [[nodiscard]] std::vector<std::pair<Move, std::uint16_t>> probe_all(const Position& pos) const;

  // Compute the Polyglot Zobrist key for a position.
  // This uses the standardized Polyglot random values, NOT the engine's Zobrist.
  [[nodiscard]] static std::uint64_t compute_polyglot_key(const Position& pos);

private:
  // Decode a Polyglot 16-bit move encoding to our Move type.
  // Returns nullopt if the move is invalid or illegal in the position.
  [[nodiscard]] static std::optional<Move> decode_polyglot_move(std::uint16_t encoded,
                                                                const Position& pos);

  // Find all entries matching the given key (binary search)
  [[nodiscard]] std::vector<PolyglotEntry> find_entries(std::uint64_t key) const;

  // Select one entry from candidates using weighted random selection.
  // Uses the position key as seed for deterministic behavior per position.
  [[nodiscard]] static std::optional<PolyglotEntry>
  select_weighted(const std::vector<PolyglotEntry>& entries, std::uint64_t seed);

  std::vector<PolyglotEntry> entries_;
  bool loaded_{false};
};

} // namespace c3
