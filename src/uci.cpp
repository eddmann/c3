#include "c3/uci.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
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
#include "c3/attacks.hpp"
#include "c3/engine.hpp"
#include "c3/eval.hpp"
#include "c3/movegen.hpp"
#include "c3/search.hpp"

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

UciMove to_uci_move(const Move& mv) {
  return UciMove{
      .from = mv.from,
      .to = mv.to,
      .promotion_piece = mv.promotion_piece,
  };
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

std::uint32_t parse_u32_attr(const std::string& attr, const std::string& value) {
  try {
    const long long parsed = std::stoll(value);
    if (parsed < 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::out_of_range("out of range");
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    throw std::runtime_error("invalid value for '" + attr + "' attribute");
  }
}

std::uint64_t parse_u64_attr(const std::string& attr, const std::string& value) {
  // `nodes` is a 64-bit count, so it is read unsigned. std::stoull silently
  // wraps a leading '-' into a huge positive number, hence the explicit sign
  // check before parsing.
  try {
    if (value.empty() || value.front() == '-') {
      throw std::out_of_range("negative");
    }
    return std::stoull(value);
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

// A `go` line may bound the search twice (`go mate 2 depth 8`). Keeping the
// tighter of the two bounds makes the outcome independent of token order.
void tighten_depth(GoParams& params, std::uint32_t plies) {
  const auto clamped =
      static_cast<std::uint8_t>(std::clamp<std::uint32_t>(plies, 1, search::MAX_DEPTH));
  params.depth = params.depth.has_value() ? std::min(*params.depth, clamped) : clamped;
}

// Applies one `<attribute> <value>` pair of a `go` command. Returns false when
// the attribute is not one we know, so the caller can skip that single token.
bool apply_go_attribute(GoParams& params, const std::string& attr, const std::string& value) {
  if (attr == "depth") {
    // Clamped rather than dropped: a request we cannot honour exactly is still
    // a request for a BOUNDED search, and dropping it would silently turn
    // `go depth 256` into a search only `stop` can end.
    tighten_depth(params, parse_u32_attr(attr, value));
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
  } else if (attr == "movestogo") {
    params.movestogo = parse_u32_attr(attr, value);
  } else if (attr == "mate") {
    // "Find a mate in N moves." Such a mate takes N moves from us and N-1
    // replies from the opponent, so 2N-1 plies of search are enough to see it.
    // We approximate the request with that depth limit.
    const auto moves_to_mate =
        std::clamp<std::uint32_t>(parse_u32_attr(attr, value), 1, search::MAX_DEPTH);
    tighten_depth(params, (2 * moves_to_mate) - 1);
  } else {
    return false;
  }

  return true;
}

// `go` carries every search limit a GUI knows about, and GUIs keep inventing
// new ones. The UCI spec therefore tells engines to IGNORE tokens they do not
// understand: aborting the whole command would leave the GUI waiting for a
// `bestmove` that never arrives, which loses the game on time. The same goes
// for a value we cannot read—it is reported through `diagnostics` and skipped,
// so THIS FUNCTION NEVER THROWS and every `go` reaches the search.
GoParams parse_go(const std::vector<std::string>& args, std::vector<std::string>& diagnostics) {
  GoParams params;

  std::size_t i = 0;
  while (i < args.size()) {
    const std::string& attr = args[i];

    // Value-less flags.
    if (attr == "infinite") {
      params.infinite = true;
      ++i;
      continue;
    }

    if (attr == "ponder") {
      params.ponder = true;
      ++i;
      continue;
    }

    // `searchmoves` is followed by an open-ended list of moves, so it ends at
    // the first token that is not a move (usually the next attribute).
    if (attr == "searchmoves") {
      ++i;
      while (i < args.size()) {
        const auto mv = parse_uci_move(args[i]);
        if (!mv.has_value()) {
          break;
        }
        params.searchmoves.push_back(*mv);
        ++i;
      }
      continue;
    }

    // Everything below takes a value; a dangling attribute is ignored.
    if (i + 1 >= args.size()) {
      break;
    }

    const std::string& value = args[i + 1];

    bool recognised = false;
    try {
      recognised = apply_go_attribute(params, attr, value);
    } catch (const std::exception& ex) {
      // A known attribute with an unreadable value: drop the limit, keep the
      // rest of the line, and let the loop tell the user what we skipped.
      std::string diagnostic = ex.what();
      diagnostic += " (ignoring '";
      diagnostic += attr;
      diagnostic += ' ';
      diagnostic += value;
      diagnostic += "')";
      diagnostics.push_back(std::move(diagnostic));
      recognised = true;
    }

    // An unknown attribute costs one token, not two: the token after it may
    // well be an attribute we do know, so we must not swallow it as a value.
    i += recognised ? 2 : 1;
  }

  // `infinite` means "search until `stop`". Some GUIs send it alongside a
  // clock; the flag wins, otherwise we would stop on a timer the GUI never
  // intended to apply.
  if (params.infinite) {
    params.movetime.reset();
    params.wtime.reset();
    params.btime.reset();
    params.winc.reset();
    params.binc.reset();
    params.movestogo.reset();
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

    std::size_t size_mb = 0;
    try {
      size_mb = std::stoull(*option.value);
    } catch (...) {
      throw std::runtime_error("could not parse value for 'hash' option");
    }

    // The range check lives outside the try: raising it inside would be caught
    // by the catch-all above and misreported as a parse failure, hiding the
    // real reason from whoever typed the command.
    if (size_mb < search::TT_MIN_SIZE_MB || size_mb > search::TT_MAX_SIZE_MB) {
      throw std::runtime_error("value for 'hash' option must be between " +
                               std::to_string(search::TT_MIN_SIZE_MB) + " and " +
                               std::to_string(search::TT_MAX_SIZE_MB) + " MB");
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
    result.go_params = parse_go(args, result.diagnostics);
  } else if (head == "setoption") {
    result.type = CommandType::SetOption;
    result.option = parse_setoption(args);
  } else if (head == "debug" || head == "register" || head == "ponderhit") {
    // Accepted and ignored on purpose. `debug` asks for extra `info string`
    // tracing we do not produce, `register` only matters to engines that need
    // a licence key, and `ponderhit` confirms a ponder search this engine
    // never started. Silence is a valid answer to all three.
    result.type = CommandType::NoOp;
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

namespace {

// How many more moves we assume the game lasts when the GUI does not say.
// Spending 1/30th of the clock per move is deliberately cautious: the budget
// is recomputed every move, so what we do not spend now stays available and
// the allocation shrinks smoothly as the clock does.
constexpr std::int64_t ASSUMED_MOVES_REMAINING = 30;

// Fraction of the clock held back for protocol and GUI latency, plus a floor
// so that even a tiny clock keeps a few milliseconds of slack. Note that the
// search subtracts a further safety margin of its own (TIME_SAFETY_MARGIN,
// 5ms) from whatever budget we hand it: the two reserves stack.
constexpr std::int64_t TIME_RESERVE_DIVISOR = 20;
constexpr auto MINIMUM_TIME_RESERVE = std::chrono::milliseconds{50};

// With `movestogo` the natural share is time_left / movestogo, but a GUI that
// says "1 move to go" would have us burn the entire clock on one move. Never
// commit more than half of what is left to a single move.
constexpr std::int64_t MOVES_TO_GO_CAP_DIVISOR = 2;

// Smallest budget we will ever hand the search. A zero-millisecond budget
// makes the stopper fire before the first evaluation, so the search returns no
// move at all. Ten milliseconds usually buys a depth-1 answer—after the
// search's own 5ms margin, half of it survives—but it cannot promise one on a
// slow machine. The fallback move in the `go` handler is what actually
// guarantees the reply; this floor only improves its quality.
constexpr auto MINIMUM_ALLOCATED_TIME = std::chrono::milliseconds{10};

} // namespace

// TIME BUDGET FOR ONE MOVE
//
//   share     = time_left / movestogo          (or / 30 when movestogo is
//                                               absent, capped at half the
//                                               clock when it is present)
//   allocated = min(share + increment/2, time_left - reserve)
//   budget    = max(allocated, min(10ms, time_left))
//
// Half of the increment is added because the increment is credited after the
// move: spending all of it would slowly drain the main clock.
std::optional<std::chrono::milliseconds>
calculate_allocated_time(std::chrono::milliseconds time_left,
                         std::optional<std::chrono::milliseconds> increment,
                         std::optional<std::uint32_t> moves_to_go) noexcept {
  if (time_left.count() <= 0) {
    // Nothing left to spend: only the fallback move can save us now.
    return std::chrono::milliseconds{0};
  }

  const auto reserve = std::max(time_left / TIME_RESERVE_DIVISOR, MINIMUM_TIME_RESERVE);
  const auto max_time = time_left > reserve ? time_left - reserve : std::chrono::milliseconds{0};

  auto share = time_left / ASSUMED_MOVES_REMAINING;
  if (moves_to_go.has_value()) {
    const auto moves = static_cast<std::int64_t>(std::max<std::uint32_t>(*moves_to_go, 1U));
    share = std::min(time_left / moves, time_left / MOVES_TO_GO_CAP_DIVISOR);
  }

  const auto allocated =
      std::min(share + increment.value_or(std::chrono::milliseconds{0}) / 2, max_time);

  return std::max(std::chrono::milliseconds{allocated.count()},
                  std::min(MINIMUM_ALLOCATED_TIME, time_left));
}

// ---------------------------------------------------------------------------
// Position helpers
// ---------------------------------------------------------------------------

// Both helpers below lean on `legal_moves()` from movegen: the generator
// produces PSEUDO-legal moves, and that shared filter turns them into the
// exact legal list by making each move, testing the mover's king and unmaking.

// A UCI move string ("e2e4", "e7e8q") names only squares and a promotion
// letter. Matching it against the legal move list does two jobs at once: it
// rejects input the position does not allow—a wrong-side move, a promotion
// suffix on a non-promoting move such as "e2e4q", anything a buggy GUI or a
// typo produces—and it fills in what the string leaves out: which piece moves,
// what it captures, and whether the capture is en passant.
Move to_engine_move(const UciMove& uci_move, const Position& pos) {
  for (const auto& mv : legal_moves(pos)) {
    if (mv.from == uci_move.from && mv.to == uci_move.to &&
        mv.promotion_piece == uci_move.promotion_piece) {
      return mv;
    }
  }

  throw std::runtime_error("illegal move for this position: " + to_uci_string(uci_move));
}

void apply_position_command(const PositionCommand& command, Position& pos) {
  // Built into a scratch position and only handed over once every move has
  // been accepted: a half-applied move list would silently corrupt the game
  // state the GUI believes we are holding.
  Position next = Position::from_fen(command.fen);

  for (const auto& uci_move : command.moves) {
    next.make_move(to_engine_move(uci_move, next));
  }

  pos = next;
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
        pv_stream << to_uci_string(to_uci_move(moves[i]));
        if (i + 1 < moves.size()) {
          pv_stream << ' ';
        }
      }

      info.push_back("pv " + pv_stream.str());
      best_move_ = to_uci_move(moves[0]);
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

// A one-shot latch the search thread opens once it has something to report.
// The UCI loop waits on it before interrupting a search: see SearchHandle.
class SearchGate {
public:
  // Bounded on purpose. The reply itself is guaranteed by the fallback move,
  // so this wait only UPGRADES the answer from "first legal move" to "the move
  // depth 1 chose". Waiting forever would let a wedged search hold `quit`
  // hostage, which is a worse failure than a slightly weaker move.
  static constexpr auto TIMEOUT = std::chrono::seconds{2};

  void open() {
    {
      const std::scoped_lock lock(mutex_);
      open_ = true;
    }
    condition_.notify_all();
  }

  void wait() {
    std::unique_lock lock(mutex_);
    (void)condition_.wait_for(lock, TIMEOUT, [this] { return open_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool open_{false};
};

// Opens a gate when it leaves scope, however it is left—a normal return, or an
// exception unwinding out of the search.
class GateOpener {
public:
  explicit GateOpener(SearchGate& gate) : gate_(&gate) {}
  ~GateOpener() { gate_->open(); }

  GateOpener(const GateOpener&) = delete;
  GateOpener& operator=(const GateOpener&) = delete;
  GateOpener(GateOpener&&) = delete;
  GateOpener& operator=(GateOpener&&) = delete;

private:
  SearchGate* gate_;
};

// Wraps UciReporter so that the first `info` line—i.e. the first completed
// iterative-deepening iteration—also opens the search's gate.
class GatedUciReporter : public UciReporter {
public:
  GatedUciReporter(std::ostream& out, std::mutex* mutex, SearchGate& gate)
      : UciReporter(out, mutex), gate_(&gate) {}

  void send(const search::Report& report) override {
    UciReporter::send(report);
    gate_->open();
  }

private:
  SearchGate* gate_;
};

// The search runs on its own thread so the loop can keep reading stdin—the
// GUI is allowed to send `stop` at any moment, including microseconds after
// `go`. Two rules make that safe:
//
//   1. NEVER ABANDON A SEARCH THAT HAS NOTHING TO PLAY. `stop()` waits on the
//      gate (briefly, see SearchGate::TIMEOUT) before raising the stop flag,
//      so the depth-1 iteration normally gets to finish. The search thread
//      also opens the gate as it exits, so a search that ends without
//      reporting (an exhausted clock, a mated position) never makes the loop
//      wait at all.
//   2. JOIN BEFORE ANYTHING THE THREAD USES DIES. The thread writes to the
//      output stream under a mutex owned by the loop, so it must be joined
//      while both are still alive—hence the destructor, and the explicit
//      stop() when stdin reaches EOF.
struct SearchHandle {
  std::thread thread;
  std::shared_ptr<std::atomic_bool> stop_signal;
  std::shared_ptr<SearchGate> gate;

  SearchHandle() = default;
  ~SearchHandle() { stop(); }

  // Non-copyable, non-movable (due to thread ownership)
  SearchHandle(const SearchHandle&) = delete;
  SearchHandle& operator=(const SearchHandle&) = delete;
  SearchHandle(SearchHandle&&) = delete;
  SearchHandle& operator=(SearchHandle&&) = delete;

  void stop() {
    if (!thread.joinable()) {
      return;
    }

    if (gate) {
      gate->wait();
    }
    if (stop_signal) {
      stop_signal->store(true, std::memory_order_release);
    }

    thread.join();
    stop_signal.reset();
    gate.reset();
  }
};

// Turns the clock half of a `go` command into a single search budget.
std::optional<std::chrono::milliseconds> resolve_time_limit(const GoParams& params,
                                                            Colour colour_to_move) {
  if (params.infinite) {
    return std::nullopt;
  }

  if (params.movetime.has_value()) {
    return params.movetime;
  }

  const bool white = colour_to_move == Colour::White;
  const auto time_left = white ? params.wtime : params.btime;
  if (!time_left.has_value()) {
    return std::nullopt;
  }

  return calculate_allocated_time(*time_left, white ? params.winc : params.binc, params.movestogo);
}

// Runs one search on `handle`'s thread and reports its result.
//
// FALLBACK MOVE: a `go` must always be answered with a `bestmove`, and the
// search can legitimately come back empty-handed—stopped before its first
// iteration finished, handed a clock that had already run out, or unable to
// allocate its transposition table. Holding the first legal move of the root
// position in reserve means the only way to answer `bestmove (none)` is the
// honest one: there is no legal move at all (checkmate or stalemate).
void start_search(SearchHandle& handle, const GoParams& params, const Position& root,
                  std::ostream& out, std::mutex& out_mutex) {
  search::Limits limits;
  limits.depth = params.depth;
  limits.nodes = params.nodes;
  limits.time = resolve_time_limit(params, root.colour_to_move);

  const auto root_moves = legal_moves(root);
  const std::optional<UciMove> fallback_move =
      root_moves.empty() ? std::nullopt : std::make_optional(to_uci_move(root_moves[0]));

  auto stop_signal = std::make_shared<std::atomic_bool>(false);
  auto gate = std::make_shared<SearchGate>();
  Position pos_copy = root;

  handle.thread =
      std::thread([pos_copy, limits, stop_signal, gate, fallback_move, &out, &out_mutex]() mutable {
        // An exception escaping a std::thread calls std::terminate, so this body
        // swallows everything: a failed search costs us one move, a terminated
        // process costs us the game.
        try {
          // Opened however we leave this scope—normal return, or an exception
          // unwinding out of the search—so a waiting `stop` is never left sitting
          // out the gate's whole timeout for nothing.
          const GateOpener gate_opener(*gate);

          auto emit = [&out, &out_mutex](const std::string& reply) {
            const std::scoped_lock lock(out_mutex);
            out << reply << '\n' << std::flush;
          };

          std::optional<UciMove> searched_move;

          try {
            GatedUciReporter reporter(out, &out_mutex, *gate);
            search::search(pos_copy, limits, reporter, stop_signal);
            searched_move = reporter.best_move();
          } catch (const std::exception& ex) {
            // Most plausibly std::bad_alloc while sizing the transposition table
            // after a large `setoption name Hash`.
            emit("info string search failed: " + std::string(ex.what()));
          } catch (...) {
            emit("info string search failed: unknown error");
          }

          // Exactly one bestmove, whatever happened above.
          const auto best = searched_move.has_value() ? searched_move : fallback_move;
          emit("bestmove " + (best.has_value() ? to_uci_string(*best) : "(none)"));
        } catch (...) { // NOLINT(bugprone-empty-catch)
          // Deliberately empty: reporting itself failed (a broken stream, or no
          // memory left to build the line). There is nothing useful left to say,
          // and the one thing we must not do is let this escape the thread.
        }
      });

  handle.stop_signal = stop_signal;
  handle.gate = gate;
}

} // namespace

namespace {
void run_loop_impl(std::istream& in,
                   std::ostream& out) { // NOLINT(readability-function-cognitive-complexity)
  // DECLARATION ORDER MATTERS: locals are destroyed in reverse, so the search
  // handle (which joins the search thread) must be declared last. The thread
  // writes to `out` while holding `out_mutex`; destroying either before the
  // join would leave it writing through dead objects.
  std::mutex out_mutex;
  Engine engine;
  SearchHandle search_handle;

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

      // Parts of the command we skipped rather than obeyed.
      for (const auto& diagnostic : cmd.diagnostics) {
        write_line("info string " + diagnostic);
      }

      switch (cmd.type) {
      case CommandType::Init:
        write_line("id name " + engine_name());
        write_line("id author " + engine_author());
        write_line("option name Hash type spin default " +
                   std::to_string(search::TT_DEFAULT_SIZE_MB) + " min " +
                   std::to_string(search::TT_MIN_SIZE_MB) + " max " +
                   std::to_string(search::TT_MAX_SIZE_MB));
        write_line("uciok");
        break;

      case CommandType::IsReady:
        write_line("readyok");
        break;

      case CommandType::NewGame:
        search_handle.stop();
        engine.new_game();
        break;

      // The commands below are debugging aids, not part of UCI. Their output
      // still goes out as `info string` so that every line this loop writes is
      // something a GUI can parse and skip.
      case CommandType::PrintBoard:
      case CommandType::PrintFen:
        write_line("info string " + engine.position().to_fen());
        break;

      case CommandType::Eval:
        write_line("info string eval: " + std::to_string(eval(engine.position())));
        break;

      case CommandType::Zobrist: {
        std::ostringstream ss;
        ss << "info string zobrist: " << std::showbase << std::hex << std::setw(18)
           << std::setfill('0') << engine.position().key;
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

        write_line("info string nodes: " + std::to_string(nodes));
        write_line("info string time: " + std::to_string(ms) + " ms");
        write_line("info string nps: " + std::to_string(nps));
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

      case CommandType::Go:
        if (!cmd.go_params.has_value()) {
          throw std::runtime_error("missing go parameters");
        }
        search_handle.stop();
        start_search(search_handle, *cmd.go_params, engine.position(), out, out_mutex);
        break;

      case CommandType::SetOption:
        if (!cmd.option.has_value()) {
          throw std::runtime_error("missing option payload");
        }
        if (cmd.option->name == "hash") {
          engine.set_hash_size_mb(std::stoull(cmd.option->value.value()));
        }
        break;

      case CommandType::NoOp:
        break;

      case CommandType::Stop:
        search_handle.stop();
        break;

      case CommandType::Quit:
        search_handle.stop();
        return;
      }
    } catch (const std::exception& ex) {
      // UCI has no error channel: every line we print must be something a GUI
      // can parse. `info string` is the protocol's free-text line, so a
      // diagnostic can be shown or ignored without ever confusing the GUI.
      write_line("info string " + std::string(ex.what()));
    }
  }

  // stdin closed without a `quit` (a GUI crashed, or a pipe ran dry). Stop the
  // search here rather than relying on the destructor: it makes the ordering
  // explicit, and the search still reports its `bestmove` before we return.
  search_handle.stop();
}
} // namespace

void run_loop(std::istream& in, std::ostream& out) {
  run_loop_impl(in, out);
}

void run_loop() {
  run_loop_impl(std::cin, std::cout);
}

#ifdef C3_TESTING
// LEGACY in-process harness — see the note in uci.hpp. It calls the search
// directly instead of going through run_loop_impl, so `stop`/`quit` do
// nothing and no thread is involved. The `UciLoop` tests exercise the real
// loop and are authoritative for protocol behaviour; this one exists so
// search-quality tests get byte-for-byte deterministic output.
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
      limits.time = resolve_time_limit(*cmd.go_params, pos.colour_to_move);

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
      }
      break;

    case CommandType::NoOp:
    case CommandType::Stop:
    case CommandType::Quit:
      break;
    }
  }

  return out.str();
}
#endif

} // namespace c3::uci
