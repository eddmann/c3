#include "c3/tablebase.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace c3::tablebase {

// =============================================================================
// Global Configuration State
// =============================================================================

namespace {

std::atomic<std::uint8_t> g_probe_depth{1};
std::atomic<bool> g_use_50_move_rule{true};
std::atomic<std::uint8_t> g_probe_limit{6};

std::mutex g_path_mutex;
std::string g_path;

} // namespace

void Config::set_path(const std::string& path) {
  std::lock_guard lock(g_path_mutex);
  g_path = path;
}

std::string Config::get_path() {
  std::lock_guard lock(g_path_mutex);
  return g_path;
}

void Config::set_probe_depth(std::uint8_t depth) {
  g_probe_depth.store(depth, std::memory_order_relaxed);
}

std::uint8_t Config::get_probe_depth() {
  return g_probe_depth.load(std::memory_order_relaxed);
}

void Config::set_50_move_rule(bool enabled) {
  g_use_50_move_rule.store(enabled, std::memory_order_relaxed);
}

bool Config::get_50_move_rule() {
  return g_use_50_move_rule.load(std::memory_order_relaxed);
}

void Config::set_probe_limit(std::uint8_t limit) {
  g_probe_limit.store(limit, std::memory_order_relaxed);
}

std::uint8_t Config::get_probe_limit() {
  return g_probe_limit.load(std::memory_order_relaxed);
}

// =============================================================================
// Utility Functions
// =============================================================================

std::uint8_t count_pieces(const Position& pos) {
  std::uint8_t count = 0;
  for (const auto piece : ALL_PIECES) {
    count += static_cast<std::uint8_t>(pos.board.count_pieces(piece));
  }
  return count;
}

bool is_probeable(const Position& pos) {
  // Cannot probe if castling rights exist (position is not in tablebase)
  if (pos.castling_rights != CastlingRights::none()) {
    return false;
  }

  // Cannot probe if too many pieces
  const auto piece_count = count_pieces(pos);
  if (piece_count > Config::get_probe_limit()) {
    return false;
  }

  return true;
}

bool should_probe(const Position& pos, std::uint8_t remaining_depth) {
  if (!is_probeable(pos)) {
    return false;
  }

  // Only probe at sufficient depth to avoid probing too often
  return remaining_depth >= Config::get_probe_depth();
}

// =============================================================================
// Null Tablebase Implementation
// =============================================================================
// This is a placeholder implementation that always returns "not available".
// When Fathom is integrated, this will be replaced with actual probing.

class NullTablebase : public Tablebase {
public:
  bool init(const std::string& /*path*/) override {
    // In a real implementation, this would load Syzygy files
    return false;
  }

  void free() override {
    // Nothing to free
  }

  [[nodiscard]] bool is_available() const override { return false; }

  [[nodiscard]] std::uint8_t max_pieces() const override { return 0; }

  [[nodiscard]] std::optional<WdlResult> probe_wdl(const Position& /*pos*/) const override {
    return std::nullopt;
  }

  [[nodiscard]] std::optional<DtzResult> probe_dtz(const Position& /*pos*/) const override {
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::vector<RootMove>>
  probe_root(const Position& /*pos*/, const MoveList& /*legal_moves*/) const override {
    return std::nullopt;
  }
};

// =============================================================================
// Global Tablebase Instance
// =============================================================================

namespace {

std::mutex g_tablebase_mutex;
std::unique_ptr<Tablebase> g_tablebase;

} // namespace

Tablebase& get_tablebase() {
  std::lock_guard lock(g_tablebase_mutex);
  if (!g_tablebase) {
    g_tablebase = std::make_unique<NullTablebase>();
  }
  return *g_tablebase;
}

void set_tablebase(std::unique_ptr<Tablebase> tb) {
  std::lock_guard lock(g_tablebase_mutex);
  g_tablebase = std::move(tb);
}

void reset_tablebase() {
  std::lock_guard lock(g_tablebase_mutex);
  g_tablebase = std::make_unique<NullTablebase>();
}

} // namespace c3::tablebase
