#include "c3/uci.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "c3/about.hpp"
#include "c3/engine.hpp"
#include "c3/eval.hpp"
#include "c3/movegen.hpp"
#include "c3/search.hpp"
#include "c3/tablebase.hpp"

namespace c3::uci {

namespace {

std::string to_lower(std::string str) {
  std::ranges::transform(str, str.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return str;
}

template <typename T> T clamp_non_negative(T value) {
  return value < T{0} ? T{0} : value;
}

template <typename T> T div_ceil(T value, T divisor) {
  return static_cast<T>((value + divisor - 1) / divisor);
}

std::vector<std::string> split_tokens(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> parts;
  std::string token;
  while (iss >> token) {
    parts.push_back(token);
  }
  return parts;
}

} // namespace

// ---------------------------------------------------------------------------
// UCI move helpers
// ---------------------------------------------------------------------------

std::optional<UciMove> parse_uci_move(const std::string& str) {
  if (str.size() != 4 && str.size() != 5) {
    return std::nullopt;
  }

  const auto from = Square::parse(std::string_view{str}.substr(0, 2));
  const auto to = Square::parse(std::string_view{str}.substr(2, 2));

  if (!from.has_value() || !to.has_value()) {
    return std::nullopt;
  }

  std::optional<Piece> promo = std::nullopt;

  if (str.size() == 5) {
    const char promo_char = static_cast<char>(std::tolower(str[4]));
    const Colour colour_to_move = (*to).rank() == 0 ? Colour::Black : Colour::White;

    switch (promo_char) {
    case 'n':
      promo = knight(colour_to_move);
      break;
    case 'b':
      promo = bishop(colour_to_move);
      break;
    case 'r':
      promo = rook(colour_to_move);
      break;
    case 'q':
      promo = queen(colour_to_move);
      break;
    default:
      return std::nullopt;
    }
  }

  return UciMove{
      .from = *from,
      .to = *to,
      .promotion_piece = promo,
  };
}

std::string to_uci_string(const UciMove& mv) {
  std::string out;
  out.reserve(5);
  out += mv.from.to_string();
  out += mv.to.to_string();

  if (mv.promotion_piece.has_value()) {
    out.push_back(static_cast<char>(std::tolower(to_char(*mv.promotion_piece))));
  }

  return out;
}

// ---------------------------------------------------------------------------
// Command parsing
// ---------------------------------------------------------------------------

namespace {

std::uint8_t parse_u8_attr(const std::string& attr, const std::string& value) {
  try {
    const int parsed = std::stoi(value);
    if (parsed < 0 || parsed > 255) {
      throw std::out_of_range("depth out of range");
    }
    return static_cast<std::uint8_t>(parsed);
  } catch (...) {
    throw std::runtime_error("invalid value for '" + attr + "' attribute");
  }
}

std::uint64_t parse_u64_attr(const std::string& attr, const std::string& value) {
  try {
    const long long parsed = std::stoll(value);
    if (parsed < 0) {
      throw std::out_of_range("negative");
    }
    return static_cast<std::uint64_t>(parsed);
  } catch (...) {
    throw std::runtime_error("invalid value for '" + attr + "' attribute");
  }
}

std::chrono::milliseconds parse_duration_attr(const std::string& attr, const std::string& value) {
  try {
    const long long ms = std::stoll(value);
    return std::chrono::milliseconds{clamp_non_negative(ms)};
  } catch (...) {
    throw std::runtime_error("invalid value for '" + attr + "' attribute");
  }
}

PositionCommand parse_position(const std::vector<std::string>& args) {
  enum class Token : std::uint8_t {
    None,
    Fen,
    Move,
  };

  Token token = Token::None;
  std::string fen;
  std::vector<UciMove> moves;

  for (const auto& arg : args) {
    if (arg == "fen") {
      token = Token::Fen;
      continue;
    }

    if (arg == "moves") {
      token = Token::Move;
      continue;
    }

    if (arg == "startpos") {
      fen = std::string(Position::START_POS_FEN);
      continue;
    }

    switch (token) {
    case Token::Fen:
      fen += fen.empty() ? arg : " " + arg;
      break;
    case Token::Move: {
      const auto mv = parse_uci_move(arg);
      if (!mv.has_value()) {
        throw std::runtime_error("invalid UCI move: " + arg);
      }
      moves.push_back(*mv);
      break;
    }
    case Token::None:
      break;
    }
  }

  if (fen.empty()) {
    throw std::runtime_error("missing FEN in position command");
  }

  // Validate FEN
  (void)Position::from_fen(fen);

  return PositionCommand{
      .fen = fen,
      .moves = moves,
  };
}

GoParams parse_go(const std::vector<std::string>& args) {
  GoParams params;

  for (std::size_t i = 0; i < args.size();) {
    const std::string& attr = args[i];

    if (attr == "infinite") {
      return params;
    }

    if (i + 1 >= args.size()) {
      throw std::runtime_error("missing value for '" + attr + "' attribute");
    }

    const std::string& value = args[i + 1];

    if (attr == "depth") {
      params.depth = parse_u8_attr(attr, value);
    } else if (attr == "movetime") {
      params.movetime = parse_duration_attr(attr, value);
    } else if (attr == "wtime") {
      params.wtime = parse_duration_attr(attr, value);
    } else if (attr == "btime") {
      params.btime = parse_duration_attr(attr, value);
    } else if (attr == "winc") {
      params.winc = parse_duration_attr(attr, value);
    } else if (attr == "binc") {
      params.binc = parse_duration_attr(attr, value);
    } else if (attr == "nodes") {
      params.nodes = parse_u64_attr(attr, value);
    } else {
      throw std::runtime_error("unknown attribute '" + attr + "'");
    }

    i += 2;
  }

  return params;
}

SetOptionCommand parse_setoption(
    const std::vector<std::string>& args) { // NOLINT(readability-function-cognitive-complexity)
  if (args.empty() || args[0] != "name") {
    throw std::runtime_error("missing option name");
  }

  std::vector<std::string> name_parts;
  std::vector<std::string> value_parts;
  bool in_value = false;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "value" && !in_value) {
      in_value = true;
      continue;
    }

    if (in_value) {
      value_parts.push_back(args[i]);
    } else {
      name_parts.push_back(args[i]);
    }
  }

  std::string name;
  for (std::size_t i = 0; i < name_parts.size(); ++i) {
    name += (i == 0 ? "" : " ") + name_parts[i];
  }
  name = to_lower(name);

  std::string value;
  for (std::size_t i = 0; i < value_parts.size(); ++i) {
    value += (i == 0 ? "" : " ") + value_parts[i];
  }
  value = to_lower(value);

  if (name.empty()) {
    throw std::runtime_error("missing option name");
  }

  SetOptionCommand option{
      .name = name,
      .value = value.empty() ? std::nullopt : std::make_optional(value),
  };

  if (name == "hash") {
    if (!option.value.has_value()) {
      throw std::runtime_error("missing value for 'hash' option");
    }

    try {
      const std::size_t size_mb = std::stoull(*option.value);
      if (size_mb < search::TT_MIN_SIZE_MB || size_mb > search::TT_MAX_SIZE_MB) {
        throw std::runtime_error("invalid value for 'hash' option");
      }
    } catch (...) {
      throw std::runtime_error("could not parse value for 'hash' option");
    }
  } else if (name == "syzygypath") {
    // SyzygyPath can be any string (path), including empty
    // Value is not lowercased for paths - reconstruct from original parts
    std::string path_value;
    for (std::size_t i = 0; i < value_parts.size(); ++i) {
      path_value += (i == 0 ? "" : " ") + value_parts[i];
    }
    option.value = path_value.empty() ? std::nullopt : std::make_optional(path_value);
  } else if (name == "syzygyprobedepth") {
    if (!option.value.has_value()) {
      throw std::runtime_error("missing value for 'syzygyprobedepth' option");
    }
    try {
      const int depth = std::stoi(*option.value);
      if (depth < 0 || depth > 255) {
        throw std::runtime_error("invalid value for 'syzygyprobedepth' option");
      }
    } catch (...) {
      throw std::runtime_error("could not parse value for 'syzygyprobedepth' option");
    }
  } else if (name == "syzygy50moverule") {
    if (!option.value.has_value()) {
      throw std::runtime_error("missing value for 'syzygy50moverule' option");
    }
    if (*option.value != "true" && *option.value != "false") {
      throw std::runtime_error("invalid value for 'syzygy50moverule' option");
    }
  } else if (name == "syzygyprobelimit") {
    if (!option.value.has_value()) {
      throw std::runtime_error("missing value for 'syzygyprobelimit' option");
    }
    try {
      const int limit = std::stoi(*option.value);
      if (limit < 0 || limit > 7) {
        throw std::runtime_error("invalid value for 'syzygyprobelimit' option");
      }
    } catch (...) {
      throw std::runtime_error("could not parse value for 'syzygyprobelimit' option");
    }
  } else {
    throw std::runtime_error("unknown option '" + name + "'");
  }

  return option;
}

} // namespace

UciCommand parse_command(const std::string& command) {
  const std::vector<std::string> parts = split_tokens(command);
  if (parts.empty()) {
    throw std::runtime_error("empty command");
  }

  const std::string& head = parts[0];
  const std::vector<std::string> args(parts.begin() + 1, parts.end());

  UciCommand result{};

  if (head == "uci") {
    result.type = CommandType::Init;
  } else if (head == "isready") {
    result.type = CommandType::IsReady;
  } else if (head == "ucinewgame") {
    result.type = CommandType::NewGame;
  } else if (head == "printboard") {
    result.type = CommandType::PrintBoard;
  } else if (head == "printfen") {
    result.type = CommandType::PrintFen;
  } else if (head == "eval") {
    result.type = CommandType::Eval;
  } else if (head == "zobrist") {
    result.type = CommandType::Zobrist;
  } else if (head == "perft") {
    if (args.empty()) {
      throw std::runtime_error("missing depth");
    }
    result.type = CommandType::Perft;
    result.perft_depth = parse_u8_attr("depth", args[0]);
  } else if (head == "domove") {
    if (args.empty()) {
      throw std::runtime_error("missing move");
    }
    result.type = CommandType::DoMove;
    const auto mv = parse_uci_move(args[0]);
    if (!mv.has_value()) {
      throw std::runtime_error("invalid move");
    }
    result.move = mv;
  } else if (head == "position") {
    result.type = CommandType::Position;
    result.position = parse_position(args);
  } else if (head == "go") {
    result.type = CommandType::Go;
    result.go_params = parse_go(args);
  } else if (head == "setoption") {
    result.type = CommandType::SetOption;
    result.option = parse_setoption(args);
  } else if (head == "stop") {
    result.type = CommandType::Stop;
  } else if (head == "quit") {
    result.type = CommandType::Quit;
  } else {
    throw std::runtime_error("unknown command '" + head + "'");
  }

  return result;
}

// ---------------------------------------------------------------------------
// Time management
// ---------------------------------------------------------------------------

std::optional<std::chrono::milliseconds>
calculate_allocated_time(std::chrono::milliseconds time_left,
                         std::optional<std::chrono::milliseconds> increment) noexcept {
  if (time_left.count() == 0) {
    return time_left;
  }

  const auto reserve = std::max(time_left / 20, std::chrono::milliseconds{50});
  const auto max_time = time_left > reserve ? time_left - reserve : std::chrono::milliseconds{0};

  const auto allocated =
      std::min(time_left / 30 + increment.value_or(std::chrono::milliseconds{0}) / 2, max_time);

  return std::chrono::milliseconds{allocated.count()};
}

// ---------------------------------------------------------------------------
// Position helpers
// ---------------------------------------------------------------------------

Move to_engine_move(const UciMove& uci_move, const Position& pos) {
  const auto piece = pos.board.piece_at(uci_move.from);
  if (!piece.has_value()) {
    throw std::runtime_error("no piece at from-square");
  }

  const bool is_en_passant =
      is_pawn(*piece) && pos.en_passant_square.has_value() && uci_move.to == *pos.en_passant_square;

  const auto captured_piece =
      is_en_passant ? std::make_optional(pawn(!colour(*piece))) : pos.board.piece_at(uci_move.to);

  return Move{
      .piece = *piece,
      .from = uci_move.from,
      .to = uci_move.to,
      .captured_piece = captured_piece,
      .promotion_piece = uci_move.promotion_piece,
      .is_en_passant = is_en_passant,
  };
}

void apply_position_command(const PositionCommand& command, Position& pos) {
  pos = Position::from_fen(command.fen);

  for (const auto& uci_move : command.moves) {
    const Move mv = to_engine_move(uci_move, pos);
    pos.make_move(mv);
  }
}

// ---------------------------------------------------------------------------
// UCI reporter
// ---------------------------------------------------------------------------

UciReporter::UciReporter(std::ostream& out, std::mutex* mutex) : out_(&out), mutex_(mutex) {}

void UciReporter::send(const search::Report& report) {
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(report.elapsed()).count();
  const auto safe_elapsed = std::max<std::int64_t>(elapsed_ms, 1);
  const auto nps = report.nodes * 1000 / static_cast<std::uint64_t>(safe_elapsed);

  const auto hashfull =
      report.tt_stats.second == 0
          ? 0
          : static_cast<std::uint32_t>((report.tt_stats.first * 1000) / report.tt_stats.second);

  std::vector<std::string> info{
      "depth " + std::to_string(report.depth),
      "nodes " + std::to_string(report.nodes),
      "nps " + std::to_string(nps),
      "hashfull " + std::to_string(hashfull),
      "time " + std::to_string(elapsed_ms),
  };

  if (report.pv.has_value()) {
    const auto& [moves, eval] = *report.pv;

    if (const auto mate_in = report.moves_until_mate()) {
      const auto moves_to_mate = div_ceil<std::uint8_t>(*mate_in, 2);
      const int signed_mate = static_cast<int>(moves_to_mate) * (eval >= 0 ? 1 : -1);
      info.push_back("score mate " + std::to_string(signed_mate));
    } else {
      info.push_back("score cp " + std::to_string(eval));
    }

    if (!moves.empty()) {
      std::ostringstream pv_stream;
      for (std::size_t i = 0; i < moves.size(); ++i) {
        pv_stream << to_uci_string(UciMove{
            .from = moves[i].from,
            .to = moves[i].to,
            .promotion_piece = moves[i].promotion_piece,
        });
        if (i + 1 < moves.size()) {
          pv_stream << ' ';
        }
      }

      info.push_back("pv " + pv_stream.str());
      best_move_ = UciMove{
          .from = moves[0].from,
          .to = moves[0].to,
          .promotion_piece = moves[0].promotion_piece,
      };
    }
  }

  std::ostringstream line;
  line << "info ";
  for (std::size_t i = 0; i < info.size(); ++i) {
    line << info[i];
    if (i + 1 < info.size()) {
      line << ' ';
    }
  }

  if (mutex_ != nullptr) {
    std::scoped_lock lock(*mutex_);
    *out_ << line.str() << '\n' << std::flush;
  } else {
    *out_ << line.str() << '\n' << std::flush;
  }
}

// ---------------------------------------------------------------------------
// UCI loop
// ---------------------------------------------------------------------------

namespace {

struct SearchHandle {
  std::thread thread;
  std::shared_ptr<std::atomic_bool> stop_signal;
  bool running{false};

  SearchHandle() = default;
  ~SearchHandle() { stop(); }

  // Non-copyable, non-movable (due to thread ownership)
  SearchHandle(const SearchHandle&) = delete;
  SearchHandle& operator=(const SearchHandle&) = delete;
  SearchHandle(SearchHandle&&) = delete;
  SearchHandle& operator=(SearchHandle&&) = delete;

  void stop() {
    if (stop_signal) {
      stop_signal->store(true, std::memory_order_release);
    }
    if (thread.joinable()) {
      thread.join();
    }
    running = false;
  }
};

} // namespace

namespace {
void run_loop_impl(std::istream& in,
                   std::ostream& out) { // NOLINT(readability-function-cognitive-complexity)
  Engine engine;
  SearchHandle search_handle;
  std::mutex out_mutex;

  auto write_line = [&](const std::string& line) {
    std::scoped_lock lock(out_mutex);
    out << line << '\n' << std::flush;
  };

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }

    try {
      const auto cmd = parse_command(line);

      switch (cmd.type) {
      case CommandType::Init:
        write_line("id name " + engine_name());
        write_line("id author " + engine_author());
        write_line("option name Hash type spin default " +
                   std::to_string(search::TT_DEFAULT_SIZE_MB) + " min " +
                   std::to_string(search::TT_MIN_SIZE_MB) + " max " +
                   std::to_string(search::TT_MAX_SIZE_MB));
        write_line("option name SyzygyPath type string default <empty>");
        write_line("option name SyzygyProbeDepth type spin default 1 min 1 max 100");
        write_line("option name Syzygy50MoveRule type check default true");
        write_line("option name SyzygyProbeLimit type spin default 6 min 0 max 7");
        write_line("uciok");
        break;

      case CommandType::IsReady:
        write_line("readyok");
        break;

      case CommandType::NewGame:
        search_handle.stop();
        engine.new_game();
        break;

      case CommandType::PrintBoard:
      case CommandType::PrintFen:
        write_line(engine.position().to_fen());
        break;

      case CommandType::Eval:
        write_line("eval: " + std::to_string(eval(engine.position())));
        break;

      case CommandType::Zobrist: {
        std::ostringstream ss;
        ss << "zobrist: " << std::showbase << std::hex << std::setw(18) << std::setfill('0')
           << engine.position().key;
        write_line(ss.str());
        break;
      }

      case CommandType::Perft: {
        if (!cmd.perft_depth.has_value()) {
          throw std::runtime_error("missing depth");
        }

        Position copy = engine.position();
        const auto started = std::chrono::steady_clock::now();
        const auto nodes = perft(copy, *cmd.perft_depth);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        const auto ms = std::max<std::int64_t>(elapsed_ms.count(), 1);
        const auto nps = nodes * 1000 / static_cast<std::uint64_t>(ms);

        write_line("");
        write_line("nodes: " + std::to_string(nodes));
        write_line("time: " + std::to_string(ms) + " ms");
        write_line("nps: " + std::to_string(nps));
        write_line("");
        break;
      }

      case CommandType::DoMove:
        if (!cmd.move.has_value()) {
          throw std::runtime_error("missing move");
        }
        engine.apply_move(to_engine_move(*cmd.move, engine.position()));
        break;

      case CommandType::Position:
        if (!cmd.position.has_value()) {
          throw std::runtime_error("missing position payload");
        }
        search_handle.stop();
        apply_position_command(*cmd.position, engine.position());
        break;

      case CommandType::Go: {
        if (!cmd.go_params.has_value()) {
          throw std::runtime_error("missing go parameters");
        }

        search_handle.stop();

        search::Limits limits;
        limits.depth = cmd.go_params->depth;
        limits.nodes = cmd.go_params->nodes;

        if (cmd.go_params->movetime.has_value()) {
          limits.time = cmd.go_params->movetime;
        } else {
          const bool white = engine.position().colour_to_move == Colour::White;
          const auto time_left = white ? cmd.go_params->wtime : cmd.go_params->btime;
          const auto inc = white ? cmd.go_params->winc : cmd.go_params->binc;
          limits.time =
              time_left.has_value() ? calculate_allocated_time(*time_left, inc) : std::nullopt;
        }

        auto stop_signal = std::make_shared<std::atomic_bool>(false);
        Position pos_copy = engine.position();

        search_handle.thread =
            std::thread([pos_copy, limits, stop_signal, &out, &out_mutex]() mutable {
              UciReporter reporter(out, &out_mutex);
              search::search(pos_copy, limits, reporter, stop_signal);

              const auto best = reporter.best_move();
              std::scoped_lock lock(out_mutex);
              if (best.has_value()) {
                out << "bestmove " << to_uci_string(*best) << '\n' << std::flush;
              } else {
                out << "bestmove (none)" << '\n' << std::flush;
              }
            });

        search_handle.stop_signal = stop_signal;
        search_handle.running = true;
        break;
      }

      case CommandType::SetOption:
        if (!cmd.option.has_value()) {
          throw std::runtime_error("missing option payload");
        }
        if (cmd.option->name == "hash") {
          engine.set_hash_size_mb(std::stoull(cmd.option->value.value()));
        } else if (cmd.option->name == "syzygypath") {
          tablebase::Config::set_path(cmd.option->value.value_or(""));
          tablebase::get_tablebase().init(cmd.option->value.value_or(""));
        } else if (cmd.option->name == "syzygyprobedepth") {
          tablebase::Config::set_probe_depth(
              static_cast<std::uint8_t>(std::stoi(cmd.option->value.value())));
        } else if (cmd.option->name == "syzygy50moverule") {
          tablebase::Config::set_50_move_rule(cmd.option->value.value() == "true");
        } else if (cmd.option->name == "syzygyprobelimit") {
          tablebase::Config::set_probe_limit(
              static_cast<std::uint8_t>(std::stoi(cmd.option->value.value())));
        }
        break;

      case CommandType::Stop:
        search_handle.stop();
        break;

      case CommandType::Quit:
        search_handle.stop();
        return;
      }
    } catch (const std::exception& ex) {
      write_line(std::string("error: ") + ex.what());
    }
  }
}
} // namespace

void run_loop(std::istream& in, std::ostream& out) {
  run_loop_impl(in, out);
}

void run_loop() {
  run_loop_impl(std::cin, std::cout);
}

#ifdef C3_TESTING
std::string run_script_for_test(
    const std::vector<std::string>& lines) { // NOLINT(readability-function-cognitive-complexity)
  std::ostringstream out;
  Position pos = Position::startpos();

  auto write_line = [&](const std::string& line) { out << line << '\n'; };

  for (const auto& raw : lines) {
    if (raw.empty()) {
      continue;
    }

    const auto cmd = parse_command(raw);

    switch (cmd.type) {
    case CommandType::Init:
      write_line("id name " + engine_name());
      write_line("id author " + engine_author());
      write_line("option name Hash type spin default " +
                 std::to_string(search::TT_DEFAULT_SIZE_MB) + " min " +
                 std::to_string(search::TT_MIN_SIZE_MB) + " max " +
                 std::to_string(search::TT_MAX_SIZE_MB));
      write_line("option name SyzygyPath type string default <empty>");
      write_line("option name SyzygyProbeDepth type spin default 1 min 1 max 100");
      write_line("option name Syzygy50MoveRule type check default true");
      write_line("option name SyzygyProbeLimit type spin default 6 min 0 max 7");
      write_line("uciok");
      break;

    case CommandType::IsReady:
      write_line("readyok");
      break;

    case CommandType::NewGame:
      pos = Position::startpos();
      break;

    case CommandType::PrintBoard:
    case CommandType::PrintFen:
      write_line(pos.to_fen());
      break;

    case CommandType::Eval:
      write_line("eval: " + std::to_string(eval(pos)));
      break;

    case CommandType::Zobrist: {
      std::ostringstream ss;
      ss << "zobrist: " << std::showbase << std::hex << std::setw(18) << std::setfill('0')
         << pos.key;
      write_line(ss.str());
      break;
    }

    case CommandType::Perft:
      if (!cmd.perft_depth.has_value()) {
        throw std::runtime_error("missing depth");
      }
      write_line(std::to_string(perft(pos, *cmd.perft_depth)));
      break;

    case CommandType::DoMove:
      pos.make_move(to_engine_move(*cmd.move, pos));
      break;

    case CommandType::Position:
      apply_position_command(*cmd.position, pos);
      break;

    case CommandType::Go: {
      search::Limits limits;
      limits.depth = cmd.go_params->depth;
      limits.nodes = cmd.go_params->nodes;

      if (cmd.go_params->movetime.has_value()) {
        limits.time = cmd.go_params->movetime;
      } else {
        const bool white = pos.colour_to_move == Colour::White;
        const auto time_left = white ? cmd.go_params->wtime : cmd.go_params->btime;
        const auto inc = white ? cmd.go_params->winc : cmd.go_params->binc;
        limits.time =
            time_left.has_value() ? calculate_allocated_time(*time_left, inc) : std::nullopt;
      }

      search::Report report;
      search::Stopper stopper;
      search::TranspositionTable tt;
      search::KillerMoves killers;
      MoveList pv;

      const int eval_final =
          search::detail::alphabeta(pos, limits.depth.value_or(1), CENTIPAWN_MIN, CENTIPAWN_MAX, pv,
                                    tt, killers, report, stopper);

      report.depth = limits.depth.value_or(1);
      report.pv = std::make_pair(pv, eval_final);
      report.tt_stats = {tt.usage(), tt.capacity()};

      UciReporter reporter(out);
      reporter.send(report);

      if (const auto best = reporter.best_move()) {
        write_line("bestmove " + to_uci_string(*best));
      } else {
        write_line("bestmove (none)");
      }

      break;
    }

    case CommandType::SetOption:
      if (cmd.option->name == "hash" && cmd.option->value.has_value()) {
        search::TranspositionTable::set_size_mb(std::stoull(*cmd.option->value));
      } else if (cmd.option->name == "syzygypath") {
        tablebase::Config::set_path(cmd.option->value.value_or(""));
      } else if (cmd.option->name == "syzygyprobedepth" && cmd.option->value.has_value()) {
        tablebase::Config::set_probe_depth(
            static_cast<std::uint8_t>(std::stoi(*cmd.option->value)));
      } else if (cmd.option->name == "syzygy50moverule" && cmd.option->value.has_value()) {
        tablebase::Config::set_50_move_rule(*cmd.option->value == "true");
      } else if (cmd.option->name == "syzygyprobelimit" && cmd.option->value.has_value()) {
        tablebase::Config::set_probe_limit(
            static_cast<std::uint8_t>(std::stoi(*cmd.option->value)));
      }
      break;

    case CommandType::Stop:
    case CommandType::Quit:
      break;
    }
  }

  return out.str();
}
#endif

} // namespace c3::uci
