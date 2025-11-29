#include "c3/tablebase.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#ifdef C3_USE_FATHOM
extern "C" {
#include "tbprobe.h"
}
#endif

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
// Fathom Tablebase Implementation
// =============================================================================
// Real implementation using the Fathom library for Syzygy tablebase probing.

#ifdef C3_USE_FATHOM

namespace {

// Convert C3 square to Fathom square (both use 0=A1, 63=H8)
unsigned to_fathom_square(Square sq) { return static_cast<unsigned>(sq.index()); }

// Convert Fathom WDL to our WdlResult
WdlResult from_fathom_wdl(unsigned wdl) {
  switch (wdl) {
  case TB_WIN:
    return WdlResult::Win;
  case TB_CURSED_WIN:
    return WdlResult::CursedWin;
  case TB_DRAW:
    return WdlResult::Draw;
  case TB_BLESSED_LOSS:
    return WdlResult::BlessedLoss;
  case TB_LOSS:
    return WdlResult::Loss;
  default:
    return WdlResult::Draw;
  }
}

// Helper to build Fathom position bitboards from C3 Position
struct FathomPosition {
  std::uint64_t white{0};
  std::uint64_t black{0};
  std::uint64_t kings{0};
  std::uint64_t queens{0};
  std::uint64_t rooks{0};
  std::uint64_t bishops{0};
  std::uint64_t knights{0};
  std::uint64_t pawns{0};

  explicit FathomPosition(const Position& pos) {
    // Populate piece bitboards
    white = pos.board.pieces_by_colour(Colour::White);
    black = pos.board.pieces_by_colour(Colour::Black);

    kings = pos.board.pieces(Piece::WK) | pos.board.pieces(Piece::BK);
    queens = pos.board.pieces(Piece::WQ) | pos.board.pieces(Piece::BQ);
    rooks = pos.board.pieces(Piece::WR) | pos.board.pieces(Piece::BR);
    bishops = pos.board.pieces(Piece::WB) | pos.board.pieces(Piece::BB);
    knights = pos.board.pieces(Piece::WN) | pos.board.pieces(Piece::BN);
    pawns = pos.board.pieces(Piece::WP) | pos.board.pieces(Piece::BP);
  }
};

} // namespace

class FathomTablebase : public Tablebase {
public:
  ~FathomTablebase() override { free(); }

  bool init(const std::string& path) override {
    if (path.empty()) {
      free();
      return false;
    }

    // Free existing resources before reinitializing
    if (available_) {
      tb_free();
    }

    available_ = tb_init(path.c_str());
    if (available_) {
      max_pieces_ = static_cast<std::uint8_t>(TB_LARGEST);
    }
    return available_;
  }

  void free() override {
    if (available_) {
      tb_free();
      available_ = false;
      max_pieces_ = 0;
    }
  }

  [[nodiscard]] bool is_available() const override { return available_; }

  [[nodiscard]] std::uint8_t max_pieces() const override { return max_pieces_; }

  [[nodiscard]] std::optional<WdlResult> probe_wdl(const Position& pos) const override {
    if (!available_) {
      return std::nullopt;
    }

    FathomPosition fp(pos);
    unsigned ep =
        pos.en_passant_square.has_value() ? to_fathom_square(*pos.en_passant_square) : 0;
    bool turn = pos.colour_to_move == Colour::White;

    // WDL probe requires rule50=0 and castling=0 (checked by Fathom internally)
    unsigned result = tb_probe_wdl(fp.white, fp.black, fp.kings, fp.queens, fp.rooks, fp.bishops,
                                   fp.knights, fp.pawns, 0, 0, ep, turn);

    if (result == TB_RESULT_FAILED) {
      return std::nullopt;
    }

    return from_fathom_wdl(result);
  }

  [[nodiscard]] std::optional<DtzResult> probe_dtz(const Position& pos) const override {
    if (!available_) {
      return std::nullopt;
    }

    FathomPosition fp(pos);
    unsigned ep =
        pos.en_passant_square.has_value() ? to_fathom_square(*pos.en_passant_square) : 0;
    bool turn = pos.colour_to_move == Colour::White;
    unsigned rule50 = Config::get_50_move_rule() ? pos.half_move_clock : 0;

    unsigned result = tb_probe_root(fp.white, fp.black, fp.kings, fp.queens, fp.rooks, fp.bishops,
                                    fp.knights, fp.pawns, rule50, 0, ep, turn, nullptr);

    if (result == TB_RESULT_FAILED) {
      return std::nullopt;
    }

    DtzResult dtz_result;
    dtz_result.wdl = from_fathom_wdl(TB_GET_WDL(result));
    dtz_result.dtz = static_cast<std::int16_t>(TB_GET_DTZ(result));

    // Negate DTZ for losing positions
    if (dtz_result.wdl == WdlResult::Loss || dtz_result.wdl == WdlResult::BlessedLoss) {
      dtz_result.dtz = static_cast<std::int16_t>(-dtz_result.dtz);
    }

    return dtz_result;
  }

  [[nodiscard]] std::optional<std::vector<RootMove>>
  probe_root(const Position& pos, const MoveList& legal_moves) const override {
    if (!available_ || legal_moves.empty()) {
      return std::nullopt;
    }

    // Probe for each legal move
    std::vector<RootMove> root_moves;
    root_moves.reserve(legal_moves.size());

    for (const auto& move : legal_moves) {
      // Make the move temporarily
      Position copy = pos;
      copy.make_move(move);

      // Probe the resulting position
      auto dtz = probe_dtz(copy);
      if (!dtz.has_value()) {
        // If any move can't be probed, return nullopt
        return std::nullopt;
      }

      // Negate the result since we're looking from the opponent's perspective
      WdlResult negated_wdl;
      switch (dtz->wdl) {
      case WdlResult::Win:
        negated_wdl = WdlResult::Loss;
        break;
      case WdlResult::CursedWin:
        negated_wdl = WdlResult::BlessedLoss;
        break;
      case WdlResult::Draw:
        negated_wdl = WdlResult::Draw;
        break;
      case WdlResult::BlessedLoss:
        negated_wdl = WdlResult::CursedWin;
        break;
      case WdlResult::Loss:
        negated_wdl = WdlResult::Win;
        break;
      }

      root_moves.push_back(RootMove{
          .move = move,
          .dtz_result = DtzResult{.wdl = negated_wdl,
                                  .dtz = static_cast<std::int16_t>(-dtz->dtz)},
      });
    }

    // Sort by WDL (wins first), then by DTZ (shortest wins first, longest losses)
    std::sort(root_moves.begin(), root_moves.end(), [](const RootMove& a, const RootMove& b) {
      // Higher WDL is better (Win > CursedWin > Draw > BlessedLoss > Loss)
      if (a.dtz_result.wdl != b.dtz_result.wdl) {
        return static_cast<int>(a.dtz_result.wdl) > static_cast<int>(b.dtz_result.wdl);
      }
      // For wins, prefer shorter DTZ (faster mate)
      if (a.dtz_result.wdl == WdlResult::Win || a.dtz_result.wdl == WdlResult::CursedWin) {
        return a.dtz_result.dtz < b.dtz_result.dtz;
      }
      // For losses, prefer longer DTZ (delay mate)
      if (a.dtz_result.wdl == WdlResult::Loss || a.dtz_result.wdl == WdlResult::BlessedLoss) {
        return a.dtz_result.dtz > b.dtz_result.dtz;
      }
      // For draws, order doesn't matter
      return false;
    });

    return root_moves;
  }

private:
  bool available_{false};
  std::uint8_t max_pieces_{0};
};

#endif // C3_USE_FATHOM

// =============================================================================
// Null Tablebase Implementation
// =============================================================================
// Fallback implementation when Fathom is not available.

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
#ifdef C3_USE_FATHOM
    g_tablebase = std::make_unique<FathomTablebase>();
#else
    g_tablebase = std::make_unique<NullTablebase>();
#endif
  }
  return *g_tablebase;
}

void set_tablebase(std::unique_ptr<Tablebase> tb) {
  std::lock_guard lock(g_tablebase_mutex);
  g_tablebase = std::move(tb);
}

void reset_tablebase() {
  std::lock_guard lock(g_tablebase_mutex);
#ifdef C3_USE_FATHOM
  g_tablebase = std::make_unique<FathomTablebase>();
#else
  g_tablebase = std::make_unique<NullTablebase>();
#endif
}

} // namespace c3::tablebase
