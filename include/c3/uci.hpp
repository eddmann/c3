#pragma once

// =============================================================================
// UCI: HOW THE ENGINE TALKS TO A GRAPHICAL FRONT-END
// =============================================================================
//
// The Universal Chess Interface is a line-based text protocol spoken over
// stdin/stdout. The GUI sends commands ("position", "go", "stop"), the engine
// answers with a handful of reply lines ("uciok", "readyok", "info ...",
// "bestmove ..."). The protocol has no error channel and no way to ask for a
// message to be repeated, which shapes two rules this layer must enforce.
//
// RULE 1: EVERY `go` IS ANSWERED WITH A LEGAL `bestmove`
// A GUI blocks until the engine replies. If we ever finish a search without a
// move—because the clock was already empty, or because `stop` arrived before
// the first iteration completed—the GUI simply waits forever and the game is
// lost on time. So the search is never abandoned before its first iterative
// deepening iteration has reported, and if even that is missing we play the
// first legal move rather than say nothing. `bestmove (none)` is reserved for
// the one honest case: a position with no legal move at all (mate/stalemate).
//
// RULE 2: TIME BUDGETS NEED A FLOOR
// Time allocation divides the remaining clock among the moves we still expect
// to play. Integer division and safety reserves can drive that share to zero
// in a time scramble, and a zero-millisecond budget means the search aborts
// before it evaluates anything. A small floor (never more than what is
// actually left on the clock) guarantees at least a depth-1 answer.
//
// Anything else we want to say—diagnostics, warnings about input we did not
// understand—travels as `info string ...`, the protocol's free-text line. The
// UCI spec also requires unknown commands and unknown tokens to be ignored
// rather than rejected, so a newer GUI never deadlocks against an older
// engine.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <istream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "c3/move.hpp"
#include "c3/position.hpp"
#include "c3/search.hpp"

namespace c3::uci {

enum class CommandType {
  Init,
  IsReady,
  NewGame,
  PrintBoard,
  PrintFen,
  Eval,
  Zobrist,
  Perft,
  DoMove,
  Position,
  Go,
  SetOption,
  Stop,
  Quit,
  // Protocol commands we recognise but deliberately do nothing about
  // (`debug`, `register`, `ponderhit`). Accepting them keeps the session
  // alive; answering them would require features this engine does not have.
  NoOp
};

struct UciMove {
  Square from{};
  Square to{};
  std::optional<Piece> promotion_piece{};

  friend constexpr bool operator==(const UciMove&, const UciMove&) = default;
};

[[nodiscard]] std::optional<UciMove> parse_uci_move(const std::string& str);
[[nodiscard]] std::string to_uci_string(const UciMove& mv);
inline std::ostream& operator<<(std::ostream& os, const UciMove& mv) {
  os << to_uci_string(mv);
  return os;
}

struct GoParams {
  std::optional<std::uint8_t> depth{};
  std::optional<std::chrono::milliseconds> movetime{};
  std::optional<std::chrono::milliseconds> wtime{};
  std::optional<std::chrono::milliseconds> btime{};
  std::optional<std::chrono::milliseconds> winc{};
  std::optional<std::chrono::milliseconds> binc{};
  std::optional<std::uint64_t> nodes{};

  // Moves left until the next time control. With it we can budget the clock
  // per remaining move instead of guessing how long the game still runs.
  std::optional<std::uint32_t> movestogo{};

  // Root moves the GUI wants us to restrict the search to. Parsed so the rest
  // of the `go` line is understood; the search does not honour the
  // restriction yet, so these are currently recorded and ignored.
  std::vector<UciMove> searchmoves{};

  // "Search until `stop` arrives." Overrides every clock the GUI may also
  // have sent on the same line.
  bool infinite{false};

  // "This search is a guess at the opponent's reply." We do not ponder, so
  // the flag is recorded and the search runs normally.
  bool ponder{false};
};

struct PositionCommand {
  std::string fen;
  std::vector<UciMove> moves;
};

struct SetOptionCommand {
  std::string name;
  std::optional<std::string> value;
};

struct UciCommand {
  CommandType type{CommandType::Init};
  std::optional<std::uint8_t> perft_depth{};
  std::optional<UciMove> move{};
  std::optional<PositionCommand> position{};
  std::optional<GoParams> go_params{};
  std::optional<SetOptionCommand> option{};
};

UciCommand parse_command(const std::string& command);

// Budget for a single move. See the formula (and its floor) in uci.cpp.
[[nodiscard]] std::optional<std::chrono::milliseconds>
calculate_allocated_time(std::chrono::milliseconds time_left,
                         std::optional<std::chrono::milliseconds> increment,
                         std::optional<std::uint32_t> moves_to_go = std::nullopt) noexcept;

// Resolves a UCI move string against the legal moves of `pos`; throws if the
// move is not legal there.
Move to_engine_move(const UciMove& uci_move, const Position& pos);
void apply_position_command(const PositionCommand& command, Position& pos);

class UciReporter : public search::Reporter {
public:
  explicit UciReporter(std::ostream& out, std::mutex* mutex = nullptr);

  void send(const search::Report& report) override;

  [[nodiscard]] std::optional<UciMove> best_move() const { return best_move_; }

private:
  std::ostream* out_;
  std::mutex* mutex_;
  std::optional<UciMove> best_move_{};
};

void run_loop(std::istream& in, std::ostream& out);
void run_loop(); // uses std::cin/std::cout internally

#ifdef C3_TESTING
// LEGACY in-process harness: replays a scripted UCI session synchronously by
// calling the search directly, so `stop`/`quit` are no-ops and no background
// thread is involved. It is kept because several search-quality tests read its
// deterministic output, but it is a parallel implementation of the dispatcher
// and NOT the protocol's source of truth—the `UciLoop` tests, which drive the
// real `run_loop`, are authoritative for protocol behaviour.
std::string run_script_for_test(const std::vector<std::string>& lines);
#endif

} // namespace c3::uci
