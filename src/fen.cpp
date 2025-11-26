#include "c3/position.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "c3/piece.hpp"

namespace c3 {
namespace {

[[nodiscard]] std::runtime_error make_error(const std::string& msg) {
  return std::runtime_error(msg);
}

Colour parse_colour_to_move(std::string_view colour) {
  if (colour == "w") {
    return Colour::White;
  }
  if (colour == "b") {
    return Colour::Black;
  }
  throw make_error("invalid colour to move '" + std::string(colour) + "'");
}

CastlingRights parse_castling_rights(std::string_view str) {
  if (str == "-") {
    return CastlingRights::none();
  }

  CastlingRights rights = CastlingRights::none();
  for (const char c : str) {
    switch (c) {
    case 'K':
      rights.add(CastlingRight::WhiteKing);
      break;
    case 'Q':
      rights.add(CastlingRight::WhiteQueen);
      break;
    case 'k':
      rights.add(CastlingRight::BlackKing);
      break;
    case 'q':
      rights.add(CastlingRight::BlackQueen);
      break;
    default:
      throw make_error("invalid castling rights");
    }
  }
  return rights;
}

std::optional<Square> parse_en_passant_square(std::string_view square) {
  if (square == "-") {
    return std::nullopt;
  }

  const auto parsed = Square::parse(square);
  if (!parsed.has_value()) {
    throw make_error("invalid en passant square");
  }

  if (parsed->rank() != 2 && parsed->rank() != 5) {
    throw make_error("invalid en passant square");
  }

  return parsed;
}

Board parse_board(std::string_view str) {
  const auto row_count = static_cast<std::size_t>(std::count(str.begin(), str.end(), '/')) + 1U;
  if (row_count != 8) {
    std::ostringstream oss;
    oss << "board must contain 8 rows, got " << row_count;
    throw make_error(oss.str());
  }

  Board board = Board::empty();
  std::uint8_t square_index = Square::A8.index();

  for (char c : str) {
    if (c == '/') {
      square_index -= 16;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      square_index += static_cast<std::uint8_t>(c - '0');
      continue;
    }

    const Piece piece = [&]() -> Piece {
      switch (c) {
      case 'P':
        return Piece::WP;
      case 'N':
        return Piece::WN;
      case 'B':
        return Piece::WB;
      case 'R':
        return Piece::WR;
      case 'Q':
        return Piece::WQ;
      case 'K':
        return Piece::WK;
      case 'p':
        return Piece::BP;
      case 'n':
        return Piece::BN;
      case 'b':
        return Piece::BB;
      case 'r':
        return Piece::BR;
      case 'q':
        return Piece::BQ;
      case 'k':
        return Piece::BK;
      default:
        throw make_error(std::string("invalid piece '") + c + "'");
      }
    }();

    board.put_piece(piece, Square::from_index(square_index));
    ++square_index;
  }

  if (square_index != 8) {
    throw make_error("board must contain 64 squares");
  }

  return board;
}

std::string board_to_fen(const Board& board) {
  std::string output;
  output.reserve(64 + 7); // pieces plus slashes

  for (int rank = 7; rank >= 0; --rank) {
    std::uint8_t empty_run = 0;

    for (int file = 0; file < 8; ++file) {
      const Square square = Square::from_file_and_rank(static_cast<std::uint8_t>(file),
                                                       static_cast<std::uint8_t>(rank));

      if (const auto piece = board.piece_at(square); piece.has_value()) {
        if (empty_run > 0) {
          output.push_back(static_cast<char>('0' + empty_run));
          empty_run = 0;
        }
        output.push_back(to_char(*piece));
      } else {
        ++empty_run;
      }
    }

    if (empty_run > 0) {
      output.push_back(static_cast<char>('0' + empty_run));
    }

    if (rank > 0) {
      output.push_back('/');
    }
  }

  return output;
}

} // namespace

Position Position::from_fen(std::string_view fen) {
  std::vector<std::string_view> parts;
  parts.reserve(6);

  std::size_t start = 0;
  while (start < fen.size()) {
    const auto pos = fen.find(' ', start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(fen.substr(start));
      break;
    }
    parts.emplace_back(fen.substr(start, pos - start));
    start = pos + 1;
  }

  constexpr std::size_t NUM_PARTS = 6;
  if (parts.size() != NUM_PARTS) {
    std::ostringstream oss;
    oss << "FEN must contain " << NUM_PARTS << " parts, got " << parts.size();
    throw make_error(oss.str());
  }

  Board board = parse_board(parts[0]);
  Colour colour_to_move = parse_colour_to_move(parts[1]);
  CastlingRights castling_rights = parse_castling_rights(parts[2]);
  auto en_passant_square = parse_en_passant_square(parts[3]);

  std::uint8_t half_move_clock = 0;
  std::uint8_t full_move_counter = 0;
  try {
    const auto half_raw = std::stoul(std::string(parts[4]));
    const auto full_raw = std::stoul(std::string(parts[5]));
    if (half_raw > 255 || full_raw > 255) {
      throw make_error("invalid move counters");
    }
    half_move_clock = static_cast<std::uint8_t>(half_raw);
    full_move_counter = static_cast<std::uint8_t>(full_raw);
  } catch (const std::exception&) {
    throw make_error("invalid move counters");
  }

  return Position{board,           colour_to_move,   castling_rights, en_passant_square,
                  half_move_clock, full_move_counter};
}

std::string Position::to_fen() const {
  std::string board_fen = board_to_fen(board);
  const char colour_char = colour_to_move == Colour::White ? 'w' : 'b';
  const std::string castling_fen = castling_rights_to_fen(castling_rights);
  const std::string en_passant_fen =
      en_passant_square.has_value() ? en_passant_square->to_string() : "-";

  std::ostringstream oss;
  oss << board_fen << ' ' << colour_char << ' ' << castling_fen << ' ' << en_passant_fen << ' '
      << static_cast<unsigned>(half_move_clock) << ' ' << static_cast<unsigned>(full_move_counter);
  return oss.str();
}

std::string castling_rights_to_fen(CastlingRights rights) {
  std::string output;
  if (rights & CastlingRight::WhiteKing) {
    output.push_back('K');
  }
  if (rights & CastlingRight::WhiteQueen) {
    output.push_back('Q');
  }
  if (rights & CastlingRight::BlackKing) {
    output.push_back('k');
  }
  if (rights & CastlingRight::BlackQueen) {
    output.push_back('q');
  }

  if (output.empty()) {
    output.push_back('-');
  }
  return output;
}

} // namespace c3
