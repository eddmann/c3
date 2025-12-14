#pragma once

// =============================================================================
// ENDGAME TABLEBASES: Perfect Play from Precomputed Knowledge
// =============================================================================
//
// Problem: In complex middlegames, engines rely on heuristics—but in simple
// endgames (≤7 pieces), every position has been solved. King+Rook vs King is
// always a win with perfect play, but how quickly? Which move is optimal?
//
// Solution: Syzygy tablebases store the game-theoretic result (win/draw/loss)
// and distance-to-zeroing (DTZ) for all positions with up to 7 pieces. Instead
// of searching millions of nodes, we look up the answer instantly.
//
// TWO TYPES OF PROBES:
//
// 1. WDL (Win/Draw/Loss) - Used during search for cutoffs
//    Returns whether the position is won, drawn, or lost. Five possible values:
//    - Win: Side to move wins with perfect play
//    - CursedWin: Win, but will be drawn by 50-move rule
//    - Draw: Neither side can force a win
//    - BlessedLoss: Loss, but saved by 50-move rule
//    - Loss: Side to move loses with perfect play
//
// 2. Root Probe (DTZ) - Used at search root for best move
//    Returns the optimal move that either wins fastest or loses slowest.
//    Only used at the root since it's more expensive than WDL.
//
// INTEGRATION POINTS:
// - After TT probe in search: WDL cutoffs when depth >= probe_depth
// - Before iterative deepening: Root probe for immediate tablebase moves
//
// CONFIGURATION (UCI options):
// - SyzygyPath: Directory containing .rtbw (WDL) and .rtbz (DTZ) files
// - SyzygyProbeDepth: Minimum depth before probing (default: 1)
// - SyzygyProbeLimit: Max pieces to probe (default: 7, max available)
// - Syzygy50MoveRule: Whether to respect 50-move rule in probes
//
// SCORE ENCODING:
// TB wins score as CENTIPAWN_TB_WIN (9000), adjusted by ply to prefer faster
// wins and delay losses. This is above any realistic material evaluation but
// below mate scores, so the engine correctly prioritizes checkmate over TB wins.
//
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>

#include "c3/move.hpp"
#include "c3/position.hpp"

namespace c3::tablebase {

// WDL probe results (values match Fathom library conventions)
enum class WdlResult : std::uint8_t {
  Loss = 0,        // Side to move loses with perfect play
  BlessedLoss = 1, // Would lose, but 50-move rule saves the draw
  Draw = 2,        // Neither side can force a win
  CursedWin = 3,   // Would win, but 50-move rule forces a draw
  Win = 4,         // Side to move wins with perfect play
  Failed = 5,      // Probe failed (TB unavailable, too many pieces, castling rights)
};

// Configuration for tablebase probing (set via UCI options)
struct Config {
  std::string path;            // Directory containing Syzygy files (.rtbw, .rtbz)
  std::uint8_t probe_depth{1}; // Minimum search depth before probing TBs
  std::uint8_t probe_limit{7}; // Max piece count to probe (saves time in complex positions)
  bool use_50_move_rule{true}; // Whether DTZ respects the 50-move draw rule
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
