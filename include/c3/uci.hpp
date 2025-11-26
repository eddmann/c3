#pragma once

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
  Quit
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

[[nodiscard]] std::optional<std::chrono::milliseconds>
calculate_allocated_time(std::chrono::milliseconds time_left,
                         std::optional<std::chrono::milliseconds> increment) noexcept;

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
// Convenience helper used by unit tests to run a small scripted UCI session
// synchronously (avoids background search thread so output is deterministic).
std::string run_script_for_test(const std::vector<std::string>& lines);
#endif

} // namespace c3::uci
