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
- Material, piece-square table, and king safety evaluation
- Full UCI protocol with time management
- GoogleTest suite with perft validation
- Fastchess gauntlet testing with SPRT

## Roadmap

- Late-move reductions (LMR)
- Enhanced evaluation (pawn structure, endgame patterns)
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
make build      # Debug build with sanitizers
make release    # Release build with LTO
make test       # Run unit tests
make fmt        # Format code
make lint       # Build with clang-tidy
make clean      # Clean all build directories
```

## Building

The project uses CMake presets for different build configurations:

| Preset    | Purpose                        | Output             | Make target     |
| --------- | ------------------------------ | ------------------ | --------------- |
| `debug`   | Development with ASan/UBSan    | `build/c3`         | `make build`    |
| `release` | Optimized with LTO             | `build-release/c3` | `make release`  |
| `lint`    | Static analysis via clang-tidy | `build-tidy/`      | `make lint`     |

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
make test
# or: ctest --preset tests
```

## Linting & Formatting

```bash
make fmt        # Format all source files
make lint       # Run clang-tidy
make can-release  # Run all CI checks (format, lint, test)
```

Style: 2-space indent, 100-column limit (configured in `.clang-format`).

## Gauntlet Testing

The `scripts/run_fastchess_gauntlet.py` script runs strength tests against other engines, outputting PGN files and SPRT summaries to `Testing/fastchess/`.

```bash
# Quick gauntlet vs opponent
make gauntlet OPPONENT=/path/to/engine GAMES=200

# Compare HEAD vs origin/main
make compare GAMES=500 DEPTH=8

# Or call scripts directly for more options:
python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --games 200 --concurrency 4 --depth 6
python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --mode movetime --movetime-ms 75
python3 scripts/run_fastchess_gauntlet.py --summarize-only tests/fixtures/fastchess_sample.pgn
```

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
