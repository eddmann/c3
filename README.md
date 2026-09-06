# c3

![c3](README/heading.png)

An educational chess engine that balances performance with readability, built to explore chess programming techniques using modern C++23.

## Why

This project grew out of a fascination with chess programming and a desire to deepen my C++ experience. The engine is heavily documented and draws inspiration from Tom Cant's [chess-rs](https://github.com/tomcant/chess-rs) 🙏🏻.

## Features

- Bitboards with magic bitboard move generation
- Iterative deepening with aspiration windows
- Negamax with alpha-beta pruning and principal variation search
- Null-move pruning, futility pruning, quiescence search, check extensions
- Transposition table with Zobrist hashing
- Move ordering: TT move, MVV-LVA, killer moves
- Tapered evaluation: material and piece-square tables (kept as an incremental
  running total), pawn structure, king safety, rook placement, mobility, tempo
- Full UCI protocol with time management
- GoogleTest suite with perft validation
- Fastchess gauntlet and SPRT strength testing

## Roadmap

- Late-move reductions (LMR)
- Endgame patterns (king-pawn races, opposition, drawn-endgame scaling)
- Pawn hash table, and evaluation weights fitted by tuning rather than by hand
  (`scripts/texel_tune.py` sketches the machinery)
- Opening book support
- Tablebase support
- Multi-threaded search

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
info string bench time 696 ms
info string bench nodes 1133254 nps 1628238
```

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
