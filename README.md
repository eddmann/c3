# c3

![c3](README/heading.png)

An educational chess engine that balances performance with readability, built to explore chess programming techniques using modern C++23.

## Why

This project grew out of a fascination with chess programming and a desire to deepen my C++ experience. The engine is heavily documented and draws inspiration from Tom Cant's [chess-rs](https://github.com/tomcant/chess-rs) 🙏🏻.

## Features

Everything below is implemented. [How a move is chosen](#how-a-move-is-chosen)
walks through the order these actually run in.

**Board and move generation** — `include/c3/board.hpp`, `movegen.hpp`, `move_list.hpp`, `zobrist.hpp`, `src/fen.cpp`

- Hybrid board representation: a 64-square mailbox for "what is on e4", plus a
  bitboard per piece type, per colour and for total occupancy for everything
  else.
- Magic bitboards for sliding attacks, read from the checked-in
  `include/c3/magic.hpp`, which carries a "do not edit" banner naming the option
  that regenerates it.
- The magic generator's search is bounded rather than a bare `while (true)`: a
  round that exhausts `MAX_ATTEMPTS_PER_ROUND` reseeds and draws denser
  candidates, and only a square that defeats every round fails the build — with
  a message saying which square, instead of hanging for ever.
- A `Move` is at most 8 bytes (`static_assert` in `move.hpp`), and moves live in
  a fixed-capacity `MoveList` of 256 entries, so nothing on the hot path
  allocates.
- `pseudo_legal_moves_into()` fills a list the caller already owns;
  `legal_moves()` is the make/unmake king test layered on top of it.
- Material, piece-square and game-phase totals are maintained incrementally by
  an `EvalAccumulator` the Board updates on every piece add and remove.
- Zobrist keys are drawn from a seeded xorshift64\*, whose output is scrambled by
  a multiply: plain xorshift64 draws are linear in the seed, and keys built from
  them share XOR relations no set of chess positions should have.
- FEN parsing validates what it reads — rank widths, piece letters, side to
  move, castling rights against the pieces actually on the board, the en-passant
  square, king counts, pawns on the first or last rank, and the move counters.

**Search** — `include/c3/search.hpp`, `src/search.cpp`

- Iterative deepening: depth 1, then 2, then 3, each iteration ordering its moves
  by what the last one found.
- Aspiration windows around the previous iteration's score, re-searched wider on
  a fail.
- Fail-hard negamax alpha-beta with principal variation search: the first move
  gets a full window, the rest a zero window and a re-search only when they
  threaten to beat alpha.
- A 16-byte packed transposition table, owned by the `Engine` and therefore
  living across moves. Entries carry a 6-bit generation ("age") that the
  replacement policy prefers to overwrite, and the table is wiped only on
  `ucinewgame` or a change of `Hash`.
- TT moves are stored as 16 bits and validated on the way out by finding them in
  the position's own move list, so a key collision can never put an impossible
  move on the board.
- Two time limits: a soft one checked only between iterations, and a hard one
  polled during them. Another iteration is started only while
  `elapsed + 4 x last_iteration` still fits inside the hard limit — 6x when soft
  and hard coincide, as they do for `go movetime`, because there is then no
  headroom to absorb a bad guess.
- Move ordering, scored once per move and then picked one at a time rather than
  fully sorted: hash move, captures and promotions by MVV-LVA, killers,
  counter-move, history.
- Promotions are scored as trading the pawn for the piece it becomes, so queen
  promotions lead their underpromotions instead of trailing them.
- Butterfly history with gravity (`score += bonus - score * |bonus| / HISTORY_MAX`),
  so the table saturates instead of overflowing and old evidence decays without
  an ageing pass.
- Late move reductions, with a full-depth re-search whenever a reduced move turns
  out to beat alpha.
- Null-move pruning.
- Futility pruning and late move pruning inside the move loop; reverse futility
  pruning and razoring at the node, razoring's claim verified by quiescence
  before it is acted on.
- Internal iterative reduction: a node the table knows nothing about is searched
  a ply shallower, and the move it finds is left behind for the next iteration to
  order by.
- Check extensions, capped at `MAX_CHECK_EXTENSIONS` (4) per path so a perpetual
  check cannot buy plies for ever, under an explicit ply ceiling of `MAX_DEPTH`.
- Quiescence search that stands pat on quiet positions but searches every legal
  reply when in check, and filters the captures it does search by delta pruning,
  by a bitboard static exchange evaluation, and down to queen promotions only.
- Per-ply scratch — move lists, ordering scores, principal variations, searched
  quiets — owned by the `SearchContext` rather than by the stack. Generating
  through `pseudo_legal_moves_into()` took alphabeta's Release frame from 2528
  bytes to 464 (measured with `-fstack-usage`; see WHY THE SCRATCH SPACE LIVES
  HERE TOO in `search.hpp`), which is what keeps a 255-ply recursion inside a
  512 KiB thread stack.
- `seldepth`: the deepest ply any single line reached, quiescence included.

**Evaluation** — `include/c3/eval.hpp`, `eval_terms.hpp`, `pawns.hpp`, `src/eval.cpp`

- Tapered: every term has a middlegame and an endgame reading, blended over a
  24-point game phase (`PHASE_MAX`).
- Material and piece-square tables in the "Simplified Evaluation Function"
  flavour (Tomasz Michniewski, Chess Programming Wiki).
- Bishop pair bonus.
- Insufficient-material detection, so a position neither side can mate from
  scores as a draw.
- Pawn structure: passed pawns (the front pawn of a file only), doubled pawns,
  isolated pawns.
- King safety: pawn shield, open and semi-open files beside the king, and enemy
  pieces whose attacks touch the king zone — all middlegame numbers, all zero in
  the endgame.
- Rooks: open files, semi-open files at half value, and the seventh rank.
- Mobility, weighted per piece and per phase, excluding for knights and bishops
  the squares an enemy pawn attacks.
- Tempo for the side to move.

**UCI** — `include/c3/uci.hpp`, `src/uci.cpp`

- The full protocol: `uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`,
  `setoption`, `quit`, with `debug`, `register` and `ponderhit` accepted and
  ignored.
- `go` understands `depth`, `movetime`, `wtime`/`btime`, `winc`/`binc`,
  `movestogo`, `nodes`, `mate` and `infinite`; tokens it does not know are
  skipped rather than aborting the command, as the spec requires.
- Time budgeting from the clock: a share of what is left (divided by `movestogo`
  when given, and never more than half the clock), plus half the increment, minus
  a reserve; the hard limit is three times the soft one, capped by the clock.
- Exactly one `bestmove` per `go`, always. A fallback legal move is chosen before
  the search starts, so a search that throws — or a clock with nothing left on it
  — still answers.
- The search runs on its own thread so the loop can keep reading `stop`, and the
  thread body catches everything: an exception escaping a `std::thread` would
  call `std::terminate`.
- `info` lines report `depth`, `seldepth`, `nodes`, `nps`, `hashfull`, `time`,
  `score` and `pv`.
- Non-UCI helpers for development: `bench`, `perft`, `eval`, `zobrist`,
  `printboard`, `printfen`, `domove`.

**Testing** — `tests/`

- Over 450 GoogleTest cases, each registered as its own ctest entry.
- Perft suite against Chess Programming Wiki ground truth, plus an opt-in deep
  run of some 15 million nodes.
- A tactical sanity suite: fixed positions whose best move the search must still
  find.
- A/B toggles on `SearchContext`, so a heuristic that only changes the size of the
  tree can be measured by searching the same position twice.
- `bench`: twelve fixed positions to a fixed depth, whose node total is a
  per-commit signature for the search.
- SPRT and gauntlet runs through fastchess, and a perft speed benchmark, driven
  from `scripts/`.

**Tooling**

- Lichess bot bridge in `bot/`: a setup script and a lichess-bot config template.

## How a move is chosen

One `go`, in the order the code runs. Each step names the file and the comment
block that argues for it.

1. **`go` arrives** (`src/uci.cpp`). The line is parsed into `GoParams`; unknown
   tokens are skipped so the command always reaches the search.
2. **A budget is set** (`calculate_time_budget`, `src/uci.cpp` — TIME BUDGET FOR
   ONE MOVE, SOFT AND HARD BUDGET FOR ONE MOVE). Clock, increment and `movestogo`
   become a soft limit and a hard limit three times as large.
3. **The search thread starts** (`src/uci.cpp`), leaving the loop free to read
   `stop`; a fallback legal move is picked first, so a `bestmove` is guaranteed.
4. **Iterative deepening** (`search()`, `src/search.cpp` — ITERATIVE DEEPENING).
   Each depth is searched inside an aspiration window around the last score, and
   `should_continue_deepening()` decides whether the next one is affordable.
5. **`alphabeta()` enters a node** (`src/search.cpp` — ALPHA-BETA SEARCH WITH
   NEGAMAX). It counts the node, then asks the cheap questions first: is this a
   draw by repetition or the fifty-move rule, and has the ply ceiling been
   reached (THE PLY CEILING)?
6. **Out of depth?** The node hands over to quiescence — unless it is in check,
   which buys a ply back until the path's budget runs out (CAPPING THE CHECK
   EXTENSION).
7. **The table is probed** (TRANSPOSITION TABLE PROBE). A deep enough entry can
   end the node outright; a shallower one still leaves a move worth trying first.
   A node with no entry gives up a ply instead (INTERNAL ITERATIVE REDUCTION).
8. **One static evaluation is taken** (THE ONE STATIC EVALUATION THIS NODE GETS)
   and reused by everything below it.
9. **Whole-node pruning**: REVERSE FUTILITY PRUNING when the score is far above
   beta, RAZORING when it is far below alpha and quiescence agrees, NULL-MOVE
   PRUNING when passing the move still beats beta.
10. **Moves are generated into this ply's row and scored once** (MOVE ORDERING),
    then handed over one at a time, best first.
11. **Per move**: FUTILITY PRUNING and LATE MOVE PRUNING can skip a quiet move
    outright, and LATE MOVE REDUCTIONS can search it shallower.
12. **PVS** (PRINCIPAL VARIATION SEARCH): full window for the first move, zero
    window for the rest, re-searched at full width only when one of them looks
    like a new best move.
13. **A beta cutoff** records the killer, the counter-move and a history bonus
    for a quiet move — with a malus for the quiet moves tried before it — and
    stores a lower bound in the table.
14. **Otherwise the node stores its result**, exact or upper bound, unless the
    stopper fired below it: a score assembled from an abandoned tree is thrown
    away rather than written.
15. **Quiescence** (`quiescence()`, `src/search.cpp` — QUIESCENCE SEARCH) plays
    the captures out until the position is quiet, filtered by DELTA PRUNING and
    STATIC EXCHANGE EVALUATION, and **`eval()`** (`src/eval.cpp`) finally puts a
    number on it.

## Roadmap

- More engine-versus-engine measurement. One SPRT run against the pre-rebuild
  build exists (see Strength Testing); per-term runs behind the `SearchContext`
  A/B toggles, and a run against an external engine of known rating, are the
  next step before trusting any single heuristic's weight.
- Pawn hash table. The pawn-structure term, and the shield and open-file halves
  of king safety, read only the pawn bitboards and the king squares, so they
  could be computed once per pawn structure rather than once per node (see THE
  NEXT OPTIMISATION in `include/c3/eval.hpp`).
- Texel tuning of the evaluation weights, which needs a labelled dataset;
  `scripts/texel_tune.py` has the machinery and no data.
- Adaptive time management. The current rule predicts the next iteration as a
  fixed multiple of the last one, and the constant is a compromise — see WHY SIX
  in `include/c3/search.hpp` for two cases the rule can barely tell apart.
- Opening book support.
- Endgame tablebases.
- Lazy SMP. The search is single-threaded; the transposition table it would have
  to share is already the right shape for it.
- Chess960.

## Usage

After building, run the engine in UCI mode:

```bash
./build-release/c3
```

Example UCI session:

```
uci
isready
position startpos moves e2e4 e7e5
go depth 10
```

### The `bench` command

`bench` is a c3 extension rather than part of UCI. It searches twelve fixed
positions - openings, middlegames, tactics and endgames - to a fixed depth
(8 by default, 12 at most) and reports the total:

```bash
printf 'bench\nquit\n' | ./build-release/c3 | tail -2
```

```
info string bench time 301 ms
info string bench nodes 218288 nps 725209
```

That output is from the commit that wrote this section. Every change to search,
evaluation or move ordering moves the node count, which is the whole point of
the number, so take the figure you compare against from the build in front of
you rather than from this file.

The transposition table is cleared before each position and once more at the
end, so the node total is reproducible: run it twice on one build and the
`nodes` figure is identical, while `nps` moves with whatever else the machine
is doing. Two builds that print the same total are searching the same tree,
which makes that number a per-commit signature for the search - quote it before
and after any change to search, evaluation or move ordering, including
"unchanged at N" when the change is meant to be behaviour-neutral.

`bench <depth>` runs the same list shallower or deeper (1 to 12). It runs on
the main thread and cannot be interrupted, which is why the depth is capped.

## Prerequisites

- CMake 3.20+
- Ninja (for CMake presets)
- C++23 compiler (clang++ 16+ or g++ 13+)
- Python 3 (for scripts)
- clang-format and clang-tidy (for formatting/linting)
- fastchess (optional, for gauntlet testing)

## Quick Start

A Makefile wraps common commands. Run `make help` to see all targets:

```bash
make build         # Debug build with sanitizers
make release       # Release build with LTO
make profile       # -O2 build with debug info for profilers
make run           # Build and run the debug binary
make test          # Run unit tests (debug tree)
make test-release  # Run unit tests against the optimized build
make fmt           # Format code
make lint          # Build with clang-tidy
make can-release   # Run all CI checks (format, lint, test)
make gauntlet      # Play a gauntlet vs another engine
make compare       # SPRT HEAD against origin/main
make magic         # Regenerate the magic bitboard tables
make clean         # Clean all build directories
```

## Building

The project uses CMake presets for different build configurations:

| Preset           | Purpose                         | Output                  | Make target         |
| ---------------- | ------------------------------- | ----------------------- | ------------------- |
| `debug`          | Development with ASan/UBSan     | `build/c3`              | `make build`        |
| `release`        | Optimized with LTO, engine only | `build-release/c3`      | `make release`      |
| `release-tests`  | Optimized build plus test suite | `build-release-tests/`  | `make test-release` |
| `relwithdebinfo` | -O2 with debug info, no LTO     | `build-relwithdebinfo/` | `make profile`      |
| `lint`           | Static analysis via clang-tidy  | `build-tidy/`           | `make lint`         |

The `release` preset builds the engine only: tests are the one thing that needs
GoogleTest, so leaving them out keeps a release build free of any download.

### Options

All options are `-D<name>=ON|OFF` at configure time:

| Option                  | Purpose                                         | Default            |
| ----------------------- | ----------------------------------------------- | ------------------ |
| `C3_BUILD_TESTS`        | Configure and build the GoogleTest suite        | `ON`               |
| `C3_WARNINGS_AS_ERRORS` | Add `-Werror` (`/WX` on MSVC); CI turns this on | `OFF`              |
| `C3_EXPENSIVE_ASSERTS`  | Slow Debug-only consistency checks              | `OFF`              |
| `C3_ENABLE_ASAN`        | AddressSanitizer in Debug builds                | `ON` (not Windows) |
| `C3_ENABLE_UBSAN`       | UndefinedBehaviorSanitizer in Debug builds      | `ON` (not Windows) |
| `C3_ENABLE_CLANG_TIDY`  | Run clang-tidy during the build                 | `OFF`              |
| `C3_REGENERATE_MAGIC`   | Rebuild `include/c3/magic.hpp`                  | `OFF`              |

`C3_EXPENSIVE_ASSERTS` compiles in the Debug checks that are too costly to run
on every node - chiefly rebuilding the evaluation accumulator from scratch after
each make/unmake and comparing it against the maintained running total. It
roughly doubles the runtime of the perft suite, which is why the everyday
sanitizer build leaves it off; turn it on when hunting a suspected drift in an
incremental update.

GoogleTest is fetched at the commit tagged v1.14.0 (pinned by sha, since a tag
can be moved). An already installed GTest is reused instead when CMake finds
one, which requires CMake 3.24 or newer (older versions always fetch), and
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest` always wins over
both, pointing the build at a local checkout for fully offline work:

```bash
cmake --preset debug -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
```

### Why the engine compiles twice in test builds

A few helpers, such as `c3::uci::run_script_for_test`, exist only for the unit
tests and sit behind the `C3_TESTING` macro. Defining it project-wide would
compile that scaffolding into the released engine, so the sources are built as
two libraries instead: `c3_core` without the define for the `c3` binary, and
`c3_core_testing` with it for the test executable.

### Development (Debug + sanitizers)

```bash
make build
# or: cmake --preset debug && cmake --build --preset debug
```

### Production (Release + LTO)

```bash
make release
# or: cmake --preset release && cmake --build --preset release
```

### Regenerating magic bitboards

The magic bitboard tables are checked in at `include/c3/magic.hpp`. To regenerate:

```bash
make magic
```

### Running tests

```bash
make test          # debug tree, sanitizers on
make test-release  # same suite against the optimized build
# or: ctest --preset tests / ctest --preset release-tests
```

Each GoogleTest case becomes its own ctest entry via `gtest_discover_tests`, so
a failure names itself. Alongside them ctest registers three Python cases,
whenever CMake finds a Python 3 interpreter:

| Test                       | What it covers                                         |
| -------------------------- | ------------------------------------------------------ |
| `Fastchess.SummaryFixture` | The gauntlet summariser, over a checked-in sample PGN   |
| `Scripts.CommonUnitTests`  | The statistics helpers in `scripts/common.py`           |
| `Scripts.TexelTuneUnitTests` | The pure functions behind `scripts/texel_tune.py`     |

#### The deep perft run

The `slow-` records in `tests/fixtures/perft.txt` count some 15 million nodes
between them, which takes minutes under ASan/UBSan. They are skipped unless
`C3_SLOW_PERFT` is set, and one ctest entry - `Perft.SlowOptIn`, labelled
`slow` - supplies it:

```bash
ctest --preset slow-tests
```

`ctest --preset tests` and `ctest --preset release-tests` both exclude that
label, so the everyday run stays quick. Run the slow one before a release, or
after any change to move generation.

#### What the fixtures are for

`tests/fixtures/perft.txt` holds ground truth: those node counts come from the
Chess Programming Wiki and a mismatch means move generation is broken. The
other fixtures are regression pins rather than oracles - `zobrist.txt` and
`eval.txt` record what this build currently produces, so an unintended change
has to be noticed and a deliberate one has to be committed. Retune an
evaluation weight and every number in `eval.txt` moves; that is the point. The
chess claims live in the relational assertions in `tests/eval_test.cpp`, which
say things like "a centralised knight outscores one on the rim" and survive a
retune untouched.

## Linting & Formatting

```bash
make fmt        # Format all source files
make lint       # Run clang-tidy
make can-release  # Run all CI checks (format, lint, test)
```

Style: 2-space indent, 100-column limit (configured in `.clang-format`).

## Scripts

Each of the four entry points below takes `--help`, and three of them share
their statistics and process handling through `scripts/common.py`:

| Script                      | What it does                                                         |
| --------------------------- | -------------------------------------------------------------------- |
| `run_fastchess_gauntlet.py` | Plays a fixed number of games against another engine and summarises them |
| `compare_branches.py`       | Builds two git revisions and runs a real SPRT between them            |
| `perft_benchmark.py`        | Compares move generation speed between two revisions                  |
| `texel_tune.py`             | Texel tuning skeleton for the evaluation; bring your own labelled EPD  |
| `common.py`                 | Not a CLI: shared Elo/LOS/LLR statistics and subprocess helpers        |

`common.py` and `texel_tune.py` have their own unit tests, run by ctest as
`Scripts.CommonUnitTests` and `Scripts.TexelTuneUnitTests`.

## Strength Testing

The gauntlet and SPRT scripts drive
[fastchess](https://github.com/Disservin/fastchess) and write their PGN, log and
summary into `Testing/fastchess/`.

Evaluation changes need this more than search changes do, because they buy
knowledge with speed. Mobility is the expensive term: it asks the magic bitboard
tables a question for every sliding piece at every leaf, and the positional terms
together cost roughly 30–55% more time per node than material and piece squares
alone (measured here at depth 8: 2.15 → 1.65 Mnps on the start position,
3.14 → 2.04 Mnps on kiwipete, with about half of that down to mobility). A
release should therefore confirm with a gauntlet that the extra knowledge is
worth more than the depth it gives up; nodes per second alone cannot answer that.

### Measured so far

One SPRT has been run with this harness: the rebuilt engine against the build
it started from (`03e993b`), real clock of 10 s + 0.1 s per game, paired games
from `tests/fixtures/openings.epd`, bounds elo0 = 0 and elo1 = 5, on a single
four-core container. fastchess accepted H1 after 298 games: 291 wins, 4 draws,
3 losses for the rebuilt engine, every game ending normally with no time
forfeits. The point estimate is around +700 Elo, a figure so lopsided that the
error bar is not meaningful; read it as "a different class of engine", not as a
precise rating. The PGN and fastchess log are written to `Testing/fastchess/`,
which is gitignored, so re-run it rather than looking for the file:

```bash
# build the old binary once, then let fastchess play until a bound is crossed
python3 scripts/compare_branches.py --base 03e993b --test HEAD --mode movetime --movetime-ms 200
```

### Gauntlet

`scripts/run_fastchess_gauntlet.py` plays a fixed number of games against another
engine and summarises them: score, Elo with a draw-aware error bar, LOS, and the
log-likelihood ratio (LLR) of the run.

```bash
# Quick gauntlet vs opponent
make gauntlet OPPONENT=/path/to/engine GAMES=200

# Or call the script directly for more options:
python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --games 200 --concurrency 4 --depth 6
python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --mode movetime --movetime-ms 75
python3 scripts/run_fastchess_gauntlet.py --summarize-only tests/fixtures/fastchess_sample.pgn
```

### SPRT (comparing two branches)

`scripts/compare_branches.py` builds two revisions and hands fastchess a real
[sequential probability ratio test](https://www.chessprogramming.org/Sequential_Probability_Ratio_Test):
games keep being played until the evidence is decisive, rather than always
playing a fixed number of them.

The hypotheses are stated in Elo, and with `alpha = beta = 0.05` the LLR bounds
are +/-2.94:

| Option        | Meaning                                        | Default |
| ------------- | ---------------------------------------------- | ------- |
| `--elo0`      | H0, the "no improvement" hypothesis            | `0`     |
| `--elo1`      | H1, the smallest gain worth detecting          | `5`     |
| `--max-games` | Stop if no bound is crossed by this many games | `2000`  |
| `--depth`     | Fixed search depth per move                    | `5`     |

`make compare` and the strength-test workflow both raise the depth to 8; the
script on its own stays at 5 so an ad-hoc run finishes quickly.

```bash
# SPRT HEAD vs origin/main
make compare GAMES=2000 DEPTH=8

# Or call the script directly:
python3 scripts/compare_branches.py --base main --test HEAD --elo0 0 --elo1 5 --max-games 2000
```

Exit codes make the outcome scriptable: `0` H1 accepted (stronger), `1` H0
accepted (not stronger), `2` inconclusive at the game cap, `3` error. The
strength-test workflow turns them into a PR comment.

### Perft benchmark

`scripts/perft_benchmark.py` compares move generation speed between two
revisions. It verifies the node count of every position first (a wrong count
makes any NPS figure meaningless), takes the fastest of three runs, and only
reports a regression beyond 10% - smaller differences are within the noise of a
shared CI runner.

```bash
python3 scripts/perft_benchmark.py --base main --test HEAD
```

## Continuous Integration

`.github/workflows/test.yml` runs on every push and pull request:

| Job              | What it checks                                                                     |
| ---------------- | ---------------------------------------------------------------------------------- |
| Lint & Format    | clang-tidy via the `lint` preset, then `clang-format --dry-run --Werror` over `src include tests` |
| Build & test     | Debug build plus `ctest --preset tests` across linux x64/arm64, macOS x64/arm64 and Windows |
| Release tests    | `release-tests` preset plus `ctest --preset release-tests` on linux x64            |

Warnings are fatal (`C3_WARNINGS_AS_ERRORS=ON`) on the clang legs. The arm64
leg is cross-compiled with gcc and run under qemu, so its different warning set
is advisory only and sanitizers are off there.

`.github/workflows/strength-test.yml` adds two advisory jobs to pull requests
that touch `src/**` or `include/**`: the perft benchmark, and an SPRT run at
depth 8 with `elo0=0`, `elo1=5` and a 2000-game cap over the 48 openings in
`tests/fixtures/openings.epd`. Neither blocks a merge; both post their output
as a PR comment, and the SPRT uploads its PGN as an artifact.

`.github/workflows/release.yml` runs the whole test workflow first, then builds
and publishes binaries for all five platforms.

## Lichess Bot

C3 can play on Lichess through the
[lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) bridge. The
`bot/` directory holds a setup script and a config template:

```bash
cd bot && ./setup.sh
```

See [bot/README.md](./bot/README.md) for the manual setup, the UCI options the
bridge passes through, and troubleshooting.

## Resources

- [chess-rs (Tom Cant)](https://github.com/tomcant/chess-rs) - Rust chess engine inspiration
- [Chess Programming Wiki](https://www.chessprogramming.org/) - Comprehensive chess programming resource
  - [Perft Results](https://www.chessprogramming.org/Perft_Results)
  - [Test Positions](https://www.chessprogramming.org/Test-Positions)
  - [SPRT](https://www.chessprogramming.org/Sequential_Probability_Ratio_Test)
- [uci-suite](https://github.com/cosmobobak/uci-suite) - UCI testing utilities
- [autoperft](https://github.com/sohamkorade/autoperft) - Automated perft testing
- [Stockfish Opening Books](https://github.com/official-stockfish/books)
  - [8moves_v3.pgn.zip](https://github.com/official-stockfish/books/blob/master/8moves_v3.pgn.zip)
- [fastchess](https://github.com/Disservin/fastchess) - Engine testing framework

## License

MIT - see [LICENSE](./LICENSE) for details.
