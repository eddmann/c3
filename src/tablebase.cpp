#include "c3/tablebase.hpp"

#include <atomic>
#include <bit>
#include <mutex>

extern "C" {
#include "tbprobe.h"
}

#include "c3/eval.hpp"

namespace c3::tablebase {
namespace {

std::mutex g_config_mutex;
Config g_config;
std::atomic<bool> g_initialized{false};
std::atomic<std::uint8_t> g_max_pieces{0};

std::uint8_t count_all_pieces(const Position& pos) {
  return static_cast<std::uint8_t>(std::popcount(pos.board.pieces_by_colour(Colour::White)) +
                                   std::popcount(pos.board.pieces_by_colour(Colour::Black)));
}

} // namespace

void set_config(const Config& config) {
  std::lock_guard lock(g_config_mutex);
  g_config = config;
}

Config get_config() {
  std::lock_guard lock(g_config_mutex);
  return g_config;
}

std::uint8_t init() {
  const Config config = get_config();
  if (config.path.empty()) {
    g_initialized.store(false);
    g_max_pieces.store(0);
    return 0;
  }

  const bool ok = tb_init(config.path.c_str());
  g_initialized.store(ok);

  if (ok) {
    g_max_pieces.store(static_cast<std::uint8_t>(TB_LARGEST));
    return static_cast<std::uint8_t>(TB_LARGEST);
  }

  g_max_pieces.store(0);
  return 0;
}

bool is_available() {
  return g_initialized.load() && g_max_pieces.load() > 0;
}

std::uint8_t max_pieces() {
  return g_max_pieces.load();
}

void free() {
  if (g_initialized.load()) {
    tb_free();
    g_initialized.store(false);
    g_max_pieces.store(0);
  }
}

WdlResult probe_wdl(const Position& pos) {
  if (!is_available()) {
    return WdlResult::Failed;
  }

  const Config config = get_config();
  const std::uint8_t piece_count = count_all_pieces(pos);

  if (piece_count > config.probe_limit || piece_count > max_pieces()) {
    return WdlResult::Failed;
  }

  if (pos.castling_rights.value() != 0) {
    return WdlResult::Failed;
  }

  const std::uint64_t white = pos.board.pieces_by_colour(Colour::White);
  const std::uint64_t black = pos.board.pieces_by_colour(Colour::Black);
  const std::uint64_t kings = pos.board.pieces(Piece::WK) | pos.board.pieces(Piece::BK);
  const std::uint64_t queens = pos.board.pieces(Piece::WQ) | pos.board.pieces(Piece::BQ);
  const std::uint64_t rooks = pos.board.pieces(Piece::WR) | pos.board.pieces(Piece::BR);
  const std::uint64_t bishops = pos.board.pieces(Piece::WB) | pos.board.pieces(Piece::BB);
  const std::uint64_t knights = pos.board.pieces(Piece::WN) | pos.board.pieces(Piece::BN);
  const std::uint64_t pawns = pos.board.pieces(Piece::WP) | pos.board.pieces(Piece::BP);

  const unsigned rule50 = config.use_50_move_rule ? pos.half_move_clock : 0;
  const unsigned castling = 0;
  const unsigned ep = pos.en_passant_square.has_value() ? pos.en_passant_square->index() : 0;
  const bool turn = (pos.colour_to_move == Colour::White);

  const unsigned result = tb_probe_wdl(white, black, kings, queens, rooks, bishops, knights, pawns,
                                       rule50, castling, ep, turn);

  if (result == TB_RESULT_FAILED) {
    return WdlResult::Failed;
  }

  return static_cast<WdlResult>(result);
}

std::optional<Move> probe_root_move(const Position& pos) {
  if (!is_available()) {
    return std::nullopt;
  }

  const Config config = get_config();
  const std::uint8_t piece_count = count_all_pieces(pos);

  if (piece_count > config.probe_limit || piece_count > max_pieces()) {
    return std::nullopt;
  }

  if (pos.castling_rights.value() != 0) {
    return std::nullopt;
  }

  const std::uint64_t white = pos.board.pieces_by_colour(Colour::White);
  const std::uint64_t black = pos.board.pieces_by_colour(Colour::Black);
  const std::uint64_t kings = pos.board.pieces(Piece::WK) | pos.board.pieces(Piece::BK);
  const std::uint64_t queens = pos.board.pieces(Piece::WQ) | pos.board.pieces(Piece::BQ);
  const std::uint64_t rooks = pos.board.pieces(Piece::WR) | pos.board.pieces(Piece::BR);
  const std::uint64_t bishops = pos.board.pieces(Piece::WB) | pos.board.pieces(Piece::BB);
  const std::uint64_t knights = pos.board.pieces(Piece::WN) | pos.board.pieces(Piece::BN);
  const std::uint64_t pawns = pos.board.pieces(Piece::WP) | pos.board.pieces(Piece::BP);

  const unsigned rule50 = config.use_50_move_rule ? pos.half_move_clock : 0;
  const unsigned castling = 0;
  const unsigned ep = pos.en_passant_square.has_value() ? pos.en_passant_square->index() : 0;
  const bool turn = (pos.colour_to_move == Colour::White);

  unsigned results[TB_MAX_MOVES];
  const unsigned result = tb_probe_root(white, black, kings, queens, rooks, bishops, knights, pawns,
                                        rule50, castling, ep, turn, results);

  if (result == TB_RESULT_FAILED) {
    return std::nullopt;
  }

  const unsigned from_sq = TB_GET_FROM(result);
  const unsigned to_sq = TB_GET_TO(result);
  const unsigned promotes = TB_GET_PROMOTES(result);
  const bool is_ep = TB_GET_EP(result) != 0;

  const Square from = Square::from_index(static_cast<std::uint8_t>(from_sq));
  const Square to = Square::from_index(static_cast<std::uint8_t>(to_sq));

  const auto piece = pos.board.piece_at(from);
  if (!piece.has_value()) {
    return std::nullopt;
  }

  auto captured = pos.board.piece_at(to);
  if (is_ep && is_pawn(*piece)) {
    captured = pawn(!pos.colour_to_move);
  }

  std::optional<Piece> promo = std::nullopt;
  if (promotes != TB_PROMOTES_NONE) {
    const Colour colour = pos.colour_to_move;
    switch (promotes) {
    case TB_PROMOTES_QUEEN:
      promo = queen(colour);
      break;
    case TB_PROMOTES_ROOK:
      promo = rook(colour);
      break;
    case TB_PROMOTES_BISHOP:
      promo = bishop(colour);
      break;
    case TB_PROMOTES_KNIGHT:
      promo = knight(colour);
      break;
    default:
      break;
    }
  }

  return Move{
      .piece = *piece,
      .from = from,
      .to = to,
      .captured_piece = captured,
      .promotion_piece = promo,
      .is_en_passant = is_ep,
  };
}

int wdl_to_score(WdlResult wdl, std::uint8_t ply) {
  switch (wdl) {
  case WdlResult::Win:
    return CENTIPAWN_TB_WIN - static_cast<int>(ply);
  case WdlResult::CursedWin:
    return CENTIPAWN_DRAW + 1;
  case WdlResult::Draw:
    return CENTIPAWN_DRAW;
  case WdlResult::BlessedLoss:
    return CENTIPAWN_DRAW - 1;
  case WdlResult::Loss:
    return -CENTIPAWN_TB_WIN + static_cast<int>(ply);
  case WdlResult::Failed:
  default:
    return 0;
  }
}

} // namespace c3::tablebase
