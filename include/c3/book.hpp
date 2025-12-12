#pragma once

// =============================================================================
// OPENING BOOKS: Playing Known Theory Instantly
// =============================================================================
//
// THE PROBLEM: Opening Move Selection
// The opening phase of chess is well-studied—millions of grandmaster games have
// mapped out optimal play for the first 10-20 moves. Without a book, the engine
// must "discover" these known moves through search, wasting precious time.
//
// THE SOLUTION: Opening Books
// An opening book is a precomputed database of positions and their best moves.
// When the engine recognizes a position in the book, it plays instantly—no
// search needed. This saves time for the complex middlegame where search matters.
//
// WHY POLYGLOT FORMAT?
// Polyglot is the de facto standard for chess opening books:
//   - Binary format: compact and fast to load (16 bytes per entry)
//   - Zobrist-based: O(log n) position lookup via binary search
//   - Weighted moves: multiple candidate moves with popularity weights
//   - Universal: compatible with most chess engines and GUIs
//
// KEY CONCEPTS:
//
//   1. POLYGLOT ZOBRIST
//      Polyglot uses standardized random values (different from our engine's).
//      We must compute a "Polyglot key" separately to probe the book.
//
//   2. WEIGHTED SELECTION
//      When multiple book moves exist, weights indicate popularity/strength.
//      Higher weight = played more often by strong players. We select
//      probabilistically, adding variety while favoring better moves.
//
//   3. BOOK DEPTH
//      Books typically cover the first 10-20 moves. Beyond that, we fall
//      back to search. The BookDepth UCI option controls this cutoff.
//
// TYPICAL ELO IMPACT: +20-50 Elo (from time savings, not move quality)
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
