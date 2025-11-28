#pragma once

// =============================================================================
// SYZYGY TABLEBASE PROBING
// =============================================================================
//
// Endgame tablebases contain precomputed perfect play for all positions with
// a small number of pieces (typically 6 or fewer). This allows the engine to
// play perfectly in the endgame without any search.
//
// SYZYGY FORMAT:
// The Syzygy tablebases split information into two parts:
//
//   WDL (Win/Draw/Loss): Indicates the game-theoretic outcome assuming
//        optimal play. This is compact and fast to probe.
//
//   DTZ (Distance To Zeroing): The number of moves until a pawn move or
//        capture (which "zeros" the 50-move counter). This ensures optimal
//        play under the 50-move draw rule.
//
// INTEGRATION STRATEGY:
//   - At root: Use DTZ to select the optimal move that wins fastest or
//              draws if the position is drawn.
//   - Mid-search: Use WDL for cutoffs when we're within the piece limit.
//                 A proven win/loss can immediately cut off the search.
//
// The engine uses an abstraction layer to allow testing without actual
// tablebase files and to potentially support different tablebase formats
// in the future.
//
// =============================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "c3/move.hpp"
#include "c3/movegen.hpp"
#include "c3/position.hpp"

namespace c3::tablebase {

// =============================================================================
// Configuration
// =============================================================================

struct Config {
  std::string path;            // Path to Syzygy tablebase files
  std::uint8_t probe_depth{1}; // Minimum remaining depth before probing
  bool use_50_move_rule{true}; // Consider 50-move rule in evaluations
  std::uint8_t probe_limit{6}; // Maximum number of pieces for probing

  // Global configuration (set via UCI options)
  static void set_path(const std::string& path);
  static std::string get_path();

  static void set_probe_depth(std::uint8_t depth);
  static std::uint8_t get_probe_depth();

  static void set_50_move_rule(bool enabled);
  static bool get_50_move_rule();

  static void set_probe_limit(std::uint8_t limit);
  static std::uint8_t get_probe_limit();
};

// =============================================================================
// WDL Result
// =============================================================================
// Win/Draw/Loss from the perspective of the side to move.
// The result accounts for the 50-move rule if configured.

enum class WdlResult : std::int8_t {
  Loss = -2,        // Losing position (opponent wins with best play)
  BlessedLoss = -1, // Losing but saved by 50-move rule
  Draw = 0,         // Drawn position
  CursedWin = 1,    // Winning but will be claimed as draw (50-move rule)
  Win = 2,          // Winning position
};

// Convert WDL result to centipawn evaluation
// Uses standard conventions: Win ~= +10000, Loss ~= -10000
constexpr int wdl_to_centipawns(WdlResult wdl) {
  switch (wdl) {
  case WdlResult::Win:
    return 10000;
  case WdlResult::CursedWin:
    return 50; // Slight advantage but will be draw
  case WdlResult::Draw:
    return 0;
  case WdlResult::BlessedLoss:
    return -50; // Slight disadvantage but will be draw
  case WdlResult::Loss:
    return -10000;
  }
  return 0;
}

// =============================================================================
// DTZ Result
// =============================================================================
// Distance To Zeroing: number of moves until a capture or pawn push.
// This is used at the root to select the optimal move.

struct DtzResult {
  WdlResult wdl;    // Game-theoretic outcome
  std::int16_t dtz; // Distance to zeroing move (negative if losing)

  // Check if this result represents a successful probe
  [[nodiscard]] constexpr bool is_valid() const { return dtz != 0 || wdl == WdlResult::Draw; }
};

// =============================================================================
// Root Move with DTZ
// =============================================================================
// When probing at the root, we want the best move based on DTZ.

struct RootMove {
  Move move;
  DtzResult dtz_result;
};

// =============================================================================
// Tablebase Interface
// =============================================================================
// Abstract interface for tablebase probing. The default implementation uses
// Fathom when available, but this can be replaced for testing.

class Tablebase {
public:
  virtual ~Tablebase() = default;

  // Initialize tablebase with path to Syzygy files
  // Returns true if at least some tablebases were found
  virtual bool init(const std::string& path) = 0;

  // Free all tablebase resources
  virtual void free() = 0;

  // Check if tablebases are available
  [[nodiscard]] virtual bool is_available() const = 0;

  // Get maximum number of pieces supported
  [[nodiscard]] virtual std::uint8_t max_pieces() const = 0;

  // Probe WDL for a position
  // Returns std::nullopt if position cannot be probed (too many pieces,
  // castling rights, etc.)
  [[nodiscard]] virtual std::optional<WdlResult> probe_wdl(const Position& pos) const = 0;

  // Probe DTZ for a position
  // Returns std::nullopt if position cannot be probed
  [[nodiscard]] virtual std::optional<DtzResult> probe_dtz(const Position& pos) const = 0;

  // Probe at root to get the best move
  // Returns the list of legal moves sorted by DTZ (best first)
  [[nodiscard]] virtual std::optional<std::vector<RootMove>>
  probe_root(const Position& pos, const MoveList& legal_moves) const = 0;
};

// =============================================================================
// Global Tablebase Instance
// =============================================================================
// The engine uses a global tablebase instance for probing.

// Get the global tablebase instance
Tablebase& get_tablebase();

// Set a custom tablebase instance (for testing)
void set_tablebase(std::unique_ptr<Tablebase> tb);

// Reset to default tablebase implementation
void reset_tablebase();

// =============================================================================
// Utility Functions
// =============================================================================

// Count total pieces on the board
std::uint8_t count_pieces(const Position& pos);

// Check if position is probeable (piece count, no castling rights)
bool is_probeable(const Position& pos);

// Check if we should probe at this depth
bool should_probe(const Position& pos, std::uint8_t remaining_depth);

} // namespace c3::tablebase
