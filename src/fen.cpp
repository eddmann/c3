#include "c3/position.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "c3/bitboard.hpp"
#include "c3/piece.hpp"

namespace c3 {
namespace {

[[nodiscard]] std::runtime_error make_error(const std::string& msg) {
  return std::runtime_error(msg);
}

std::string_view colour_name(Colour colour) {
  return colour == Colour::White ? "white" : "black";
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

  // Only the two ranks a double push can jump over can name an en passant target.
  if (parsed->rank() != Square::A3.rank() && parsed->rank() != Square::A6.rank()) {
    throw make_error("invalid en passant square");
  }

  return parsed;
}

std::uint16_t parse_move_counter(std::string_view text) {
  std::uint16_t value = 0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();

  // from_chars rejects a trailing "1x" (it stops early) and a value too large for
  // the counter's type, both of which std::stoul would have accepted silently.
  const auto [stopped_at, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || stopped_at != end) {
    throw make_error("invalid move counters");
  }

  return value;
}

// FEN walks the board from a8 to h1, one rank per '/'-separated row. Each row
// must describe exactly eight files, and that has to be checked WHILE the row is
// filled rather than from a running total afterwards: placing a piece past h8
// means shifting a bitboard by 64, which is undefined behaviour, so an overrun
// such as "44P/8/8/8/8/8/8/8" must be caught before the piece is ever placed.
Board parse_board(std::string_view str) {
  const auto row_count = static_cast<std::size_t>(std::count(str.begin(), str.end(), '/')) + 1U;
  if (row_count != 8) {
    std::ostringstream oss;
    oss << "board must contain 8 rows, got " << row_count;
    throw make_error(oss.str());
  }

  const auto require_full_rank = [](std::uint8_t files) {
    if (files != 8) {
      throw make_error("board must contain 64 squares");
    }
  };

  Board board = Board::empty();
  std::uint8_t rank = Square::A8.rank();
  std::uint8_t file = 0;

  for (char c : str) {
    if (c == '/') {
      require_full_rank(file);
      --rank;
      file = 0;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      file = static_cast<std::uint8_t>(file + (c - '0'));
      if (file > 8) {
        throw make_error("board must contain 64 squares");
      }
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

    if (file > 7) {
      throw make_error("board must contain 64 squares");
    }

    board.put_piece(piece, Square::from_file_and_rank(file, rank));
    ++file;
  }

  require_full_rank(file);

  return board;
}

// =============================================================================
// LEGALITY VALIDATION
// =============================================================================
// Parsing only proves a FEN is well FORMED; the checks below prove it describes
// a position the rest of the engine can reason about. They matter because that
// code trusts its input: move generation assumes a pawn always has a rank ahead
// of it, castling generation assumes a right implies a king and rook still at
// home, and the Zobrist key folds in the en passant file. A FEN that lies about
// any of these does not misbehave where the lie is—it misbehaves several plies
// later, in code that looks innocent.
// =============================================================================

void validate_pawn_placement(const Board& board) {
  // A pawn reaching the last rank must promote, and no pawn ever moves backwards
  // onto its own first rank. Either would leave a pawn with nowhere to advance.
  const Bitboard pawns = board.pieces(Piece::WP) | board.pieces(Piece::BP);
  if ((pawns & BACK_RANKS) != 0) {
    throw make_error("pawns cannot occupy the first or last rank");
  }
}

void validate_king_count(const Board& board) {
  // Boards with no king at all are accepted on purpose: stripping a position down
  // to a single piece is how this engine's own tests isolate move generation and
  // evaluation. A second king of the same colour is a different matter—every
  // "where is the king?" lookup would silently pick just one of the two.
  for (const Colour side : {Colour::White, Colour::Black}) {
    if (board.count_pieces(king(side)) > 1) {
      std::ostringstream oss;
      oss << "there can be at most one " << colour_name(side) << " king";
      throw make_error(oss.str());
    }
  }
}

struct CastlingRequirement {
  CastlingRight right;
  char symbol;
  Colour colour;
  Square king_square;
  Square rook_square;
};

constexpr std::array<CastlingRequirement, 4> CASTLING_REQUIREMENTS = {{
    {CastlingRight::WhiteKing, 'K', Colour::White, Square::E1, Square::H1},
    {CastlingRight::WhiteQueen, 'Q', Colour::White, Square::E1, Square::A1},
    {CastlingRight::BlackKing, 'k', Colour::Black, Square::E8, Square::H8},
    {CastlingRight::BlackQueen, 'q', Colour::Black, Square::E8, Square::A8},
}};

void validate_castling_rights(const Board& board, CastlingRights rights) {
  // A castling right is a claim that neither the king nor that rook has moved, so
  // both must still be on their home squares. An unsupported right is rejected
  // rather than quietly dropped: dropping it would make from_fen(fen).to_fen()
  // differ from fen, hiding the mistake instead of reporting it.
  for (const auto& requirement : CASTLING_REQUIREMENTS) {
    if (!rights.has(requirement.right)) {
      continue;
    }

    const bool king_at_home = board.piece_at(requirement.king_square) == king(requirement.colour);
    const bool rook_at_home = board.piece_at(requirement.rook_square) == rook(requirement.colour);
    if (king_at_home && rook_at_home) {
      continue;
    }

    std::ostringstream oss;
    oss << "castling right '" << requirement.symbol << "' requires a "
        << colour_name(requirement.colour) << " king on " << requirement.king_square << " and a "
        << colour_name(requirement.colour) << " rook on " << requirement.rook_square;
    throw make_error(oss.str());
  }
}

void validate_en_passant_square(const Board& board, Colour colour_to_move,
                                std::optional<Square> en_passant_square) {
  if (!en_passant_square.has_value()) {
    return;
  }

  // The square names where a pawn that has just double-pushed can be captured, so
  // that pawn must be sitting one step beyond it and its opponent must be the
  // side to move. A stale or invented square is not harmless: the Zobrist key
  // folds in the en passant file, so it would give two identical positions
  // different hashes and break transposition lookups.
  const Square square = *en_passant_square;
  const Colour pusher = square.rank() == Square::A6.rank() ? Colour::Black : Colour::White;
  const Square pawn_square = square.advance(pusher);

  if (colour_to_move != !pusher || board.piece_at(pawn_square) != pawn(pusher)) {
    std::ostringstream oss;
    oss << "en passant square " << square << " requires a " << colour_name(pusher) << " pawn on "
        << pawn_square << " and " << colour_name(!pusher) << " to move";
    throw make_error(oss.str());
  }
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

  // Six fields is the full FEN. EPD sources (opening books, test suites) drop the
  // two clocks, so four fields are accepted with the conventional defaults.
  constexpr std::size_t FULL_PARTS = 6;
  constexpr std::size_t EPD_PARTS = 4;
  if (parts.size() != FULL_PARTS && parts.size() != EPD_PARTS) {
    std::ostringstream oss;
    oss << "FEN must contain " << EPD_PARTS << " or " << FULL_PARTS << " parts, got "
        << parts.size();
    throw make_error(oss.str());
  }

  Board board = parse_board(parts[0]);
  Colour colour_to_move = parse_colour_to_move(parts[1]);
  CastlingRights castling_rights = parse_castling_rights(parts[2]);
  auto en_passant_square = parse_en_passant_square(parts[3]);

  validate_pawn_placement(board);
  validate_king_count(board);
  validate_castling_rights(board, castling_rights);
  validate_en_passant_square(board, colour_to_move, en_passant_square);

  std::uint16_t half_move_clock = 0;
  std::uint16_t full_move_counter = 1;
  if (parts.size() == FULL_PARTS) {
    half_move_clock = parse_move_counter(parts[4]);
    full_move_counter = parse_move_counter(parts[5]);

    // Move one is the first move of the game; there is no move zero to number.
    if (full_move_counter == 0) {
      throw make_error("full move counter must be at least 1");
    }
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
