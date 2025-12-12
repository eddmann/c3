#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "c3/move.hpp"
#include "c3/position.hpp"

namespace c3::tablebase {

// WDL result values matching Fathom's conventions
enum class WdlResult : std::uint8_t {
  Loss = 0,
  BlessedLoss = 1,
  Draw = 2,
  CursedWin = 3,
  Win = 4,
  Failed = 5,
};

// Configuration for tablebase probing
struct Config {
  std::string path;
  std::uint8_t probe_depth{1};
  std::uint8_t probe_limit{7};
  bool use_50_move_rule{true};
};

// Score constant for TB wins (below mate threshold, above any material)
inline constexpr int CENTIPAWN_TB_WIN = 9000;

void set_config(const Config& config);
Config get_config();

// Initialize tablebases from the configured path.
// Returns the maximum piece count supported, or 0 if initialization failed.
std::uint8_t init();

// Check if tablebases are available and initialized.
bool is_available();

// Get the maximum piece count the loaded tablebases support.
std::uint8_t max_pieces();

// Free tablebase resources.
void free();

// Probe WDL for search cutoffs.
// Returns Failed if probe is not possible (not available, too many pieces,
// castling rights present, etc.)
WdlResult probe_wdl(const Position& pos);

// Probe DTZ at root to get the optimal move.
// Returns nullopt if probe fails.
std::optional<Move> probe_root_move(const Position& pos);

// Convert WDL result to centipawn score for search.
// Win/Loss scores are adjusted by ply to prefer faster wins / delay losses.
int wdl_to_score(WdlResult wdl, std::uint8_t ply);

} // namespace c3::tablebase
