# Repository Guidelines

## Project Structure & Module Organization

- `src/` holds engine sources (UCI glue, search, move generation, evaluation); headers live in `include/c3/` under the `c3` namespace.
- Header highlights: `move_list.hpp` (fixed-capacity move container, no allocator on the hot path), `eval_terms.hpp` (piece values, PSQTs and the incremental accumulator the Board maintains), `pawns.hpp` (compile-time masks behind the pawn-structure terms), `search.hpp` (the enhancement list at the top is the map of what the search does, and `SearchContext` owns the per-ply scratch the recursion would otherwise put on the stack).
- Hot-path move generation is `pseudo_legal_moves_into(pos, moves)`, not the `MoveList`-returning `pseudo_legal_moves(pos)`. The returning form builds two kilobytes in the caller's frame on the way to wherever the list is going, which undoes the reason the scratch rows exist: measured with `-fstack-usage`, alphabeta's Release frame was 2528 bytes that way and 464 once generation wrote into the row directly, and the search is up to 255 frames deep on a thread stack that is 512 KiB on macOS. Use the returning form in tests and tools, the `_into` form anywhere the search can reach.
- `tests/` contains GoogleTest units plus fixtures in `tests/fixtures/`; `tests/scripts/` holds `unittest` cases for the Python helpers. `Testing/` is an output dir created by test/gauntlet runs.
- `scripts/` provides `common.py` (shared stats/process helpers), `run_fastchess_gauntlet.py`, `compare_branches.py` (SPRT), `perft_benchmark.py` and `texel_tune.py` (tuning skeleton; no dataset ships with the repo).
- `bot/` holds the Lichess bridge: `setup.sh`, `config.yml` and its own `README.md`.
- Generated file: `include/c3/magic.hpp` carries a "do not edit" banner and is produced by the `generate_magic` target—never hand-edit. Regenerate with `make magic` (or configure `-DC3_REGENERATE_MAGIC=ON` and build the target).
- Twin libraries: the engine sources compile twice. `c3_core` (in `CMakeLists.txt`) links the shipped `c3` binary; `c3_core_testing` (in `tests/CMakeLists.txt`) is the same sources with `C3_TESTING` defined `PUBLIC`, so test-only helpers such as `c3::uci::run_script_for_test` exist for the tests and never reach the release binary. Side effect: `compile_commands.json` holds two entries per source file.
- Build trees: `build/` (Debug + sanitizers), `build-release/` (Release + LTO, engine only), `build-release-tests/` (Release + tests), `build-relwithdebinfo/` (profiling), `build-tidy/` (Debug + clang-tidy). `make clean` removes all five.

## Build, Test, and Development Commands

- Configure & build Debug (ASan/UBSan on): `cmake --preset debug && cmake --build --preset debug` (or `make build`).
- Release: `cmake --preset release && cmake --build --preset release` → `build-release/c3` (or `make release`). Tests are off in this preset, which is what keeps a release build free of any GoogleTest download.
- Release + tests: `cmake --preset release-tests && cmake --build --preset release-tests` → `build-release-tests/` with sanitizers off (or `make test-release`). This is the configuration that ships, so the suite runs against it at least once.
- Profiling: `cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo` → `-O2` with debug info and no LTO (or `make profile`).
- Lint build with clang-tidy: `cmake --preset lint && cmake --build --preset lint` (or `make lint`). Only `c3_core_testing` is linted; `c3_core` is skipped so diagnostics are not reported twice.
- Unit tests: `ctest --preset tests` (Debug tree) and `ctest --preset release-tests` (optimized tree). Both exclude the `slow` label.
- Deep perft: `ctest --preset slow-tests` runs the one `slow`-labelled entry, `Perft.SlowOptIn`, which sets `C3_SLOW_PERFT=1` so the `slow-` records in `tests/fixtures/perft.txt` (~15M nodes) are no longer skipped. Timeout is 900s; expect minutes under sanitizers.
- Offline GoogleTest: `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest` points the build at a local checkout and always wins. Otherwise an installed GTest is reused when CMake is 3.24+ (`FIND_PACKAGE_ARGS NAMES GTest`), and older CMake always clones the commit pinned in `tests/CMakeLists.txt` (v1.14.0 by sha, since a tag can be moved).
- `-DC3_WARNINGS_AS_ERRORS=ON` adds `-Werror` (`/WX` on MSVC). Off by default so contributors on other compiler versions are not blocked; CI turns it on for the clang legs.
- `-DC3_EXPENSIVE_ASSERTS=ON` compiles in the Debug-only checks that are too costly to run always—chiefly rebuilding the evaluation accumulator from scratch after every make/unmake and comparing it with the maintained total. It roughly doubles perft runtime, so leave it off and turn it on when hunting a suspected incremental-update drift.
- Run engine locally: `build/c3` (Debug) or `build-release/c3` (Release).
- Reading `info` output: the line is `info depth D seldepth S nodes N nps R hashfull H time T score ... pv ...`. `seldepth` is the deepest ply any single line reached and counts quiescence, so it is normally well above `depth`; a `seldepth` BELOW `depth` is a bug worth chasing, and the ply ceiling (`search::MAX_DEPTH`) is asserted against the same number.
- `bench` (a c3 extension, not UCI): `printf 'bench\nquit\n' | build-release/c3 | tail -2`. It searches 12 fixed positions at a fixed depth (default 8, cap 12 via `BENCH_MAX_DEPTH`), clearing the transposition table before each and once at the end, and ends with `info string bench nodes <N> nps <M>`. `bench [depth]` takes a shallower or deeper run. **The node count is the per-commit search signature**: two builds printing the same total are searching the same tree, so a change to search or evaluation announces itself as a changed bench.
- Gauntlet: `python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --games 200 --concurrency 4`; `--summarize-only <pgn>` aggregates an existing run without fastchess.
- SPRT: `python3 scripts/compare_branches.py --base origin/main --test HEAD --elo0 0 --elo1 5 --max-games 2000 --depth 8 --openings tests/fixtures/openings.epd` (or `make compare`). Exit codes: `0` H1 accepted, `1` H0 accepted, `2` inconclusive at the cap, `3` error.
- Perft speed: `python3 scripts/perft_benchmark.py --base main --test HEAD`. It verifies every node count before timing anything—a wrong count makes an NPS figure meaningless—and reports a regression only past `--threshold` (default 10%).

## Coding Style & Naming Conventions

- Formatting: `.clang-format` (LLVM base, 2-space indent, 100-col limit, sorted includes). Apply with `make fmt`, i.e. `clang-format -i $(git ls-files '*.cpp' '*.hpp')`. CI gates this with `clang-format --dry-run --Werror` over `src include tests`.
- Tooling: `.clang-tidy` (enable via `C3_ENABLE_CLANG_TIDY=ON` or the `lint` preset).
- Language: C++23, warnings enforced (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`); Debug uses ASan/UBSan on non-Windows.
- Names: prefer self-descriptive identifiers over inline comments—types/classes in `PascalCase`, functions/methods in `lower_snake_case`, constants/macros in `SCREAMING_SNAKE_CASE`; keep code within the `c3` namespace.
- Comments: add only when they capture chess-engine specifics (e.g., search heuristics rationale, magic bitboard edge cases) that are not obvious from well-named code.

## Testing Guidelines

- Framework: GoogleTest; add cases under `tests/*_test.cpp` using `TEST(Suite, Case)`, and register the file in `tests/CMakeLists.txt`. Cases are picked up by `gtest_discover_tests`, so each one becomes its own ctest entry.
- Test first. Write the case that fails for the reason you are about to fix, watch it fail, then fix it—a bug that never had a failing test has no proof it is gone. Every `fix(...)` commit in this repo carries the test that pins the behaviour.
- Prefer relational assertions to pinned numbers. `tests/eval_test.cpp` asserts that a centralised knight outscores a rim knight, that rooks beat a queen at equal material, that a mirrored position scores identically—claims that survive a retune. Pinned node counts and centipawn scores belong in fixtures, not in the chess assertions.
- Fixtures are regression pins, not oracles. `tests/fixtures/zobrist.txt` and `eval.txt` say so in their own headers: they record what this build produces so an unintended change is caught, and they carry no authority about the "right" answer. Retune a weight and the eval fixture moves; that is the point. `perft.txt` is the exception—those node counts are ground truth from the Chess Programming Wiki.
- Python helpers are tested too: `Scripts.CommonUnitTests` and `Scripts.TexelTuneUnitTests` run `python3 -m unittest` over `tests/scripts/`, and `Fastchess.SummaryFixture` parses `tests/fixtures/fastchess_sample.pgn`. All three are registered only when CMake finds a Python 3 interpreter.
- Measuring a search heuristic is an A/B question, and `SearchContext` carries a test-only switch for each of the ones whose only effect is the size of the tree: `reductions_enabled`, `quiescence_pruning_enabled`, `reverse_futility_enabled`, `razoring_enabled`, `late_move_pruning_enabled`, `internal_iterative_reduction_enabled`, `check_extension_cap_enabled`, `null_move_enabled`, `futility_enabled`. The recipe is one sentence: search the same position twice from two contexts that differ only in the switch, and compare `Report::nodes`. Nothing on the UCI path writes any of them (the only UCI option is `Hash`), so they cost a branch that always answers "yes" in a real game.
- Those same switches can be flipped at COMPILE time, which is how a heuristic is priced in Elo rather than in nodes. Each default comes from a `C3_DISABLE_*` macro—`C3_DISABLE_REDUCTIONS`, `C3_DISABLE_QUIESCENCE_PRUNING`, `C3_DISABLE_REVERSE_FUTILITY`, `C3_DISABLE_RAZORING`, `C3_DISABLE_LATE_MOVE_PRUNING`, `C3_DISABLE_INTERNAL_ITERATIVE_REDUCTION`, `C3_DISABLE_CHECK_EXTENSION_CAP`, `C3_DISABLE_NULL_MOVE`, `C3_DISABLE_FUTILITY` in `include/c3/search.hpp`, plus `C3_DISABLE_MOBILITY` and `C3_DISABLE_KING_ATTACKERS` in `src/eval.cpp`—so one macro on the command line turns off exactly one rule and leaves the rest alone: `cmake -S . -B build-nomob -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DC3_DISABLE_MOBILITY -DC3_BUILD_TESTS=OFF`. The point is engine-versus-engine matches between two binaries from the same commit that differ in one heuristic, run through `scripts/run_fastchess_gauntlet.py` or `scripts/compare_branches.py`. These are experiment binaries, not shipping ones: the default build defines none of the macros and its `bench` is unchanged, and the UCI path still writes none of the switches—only the compiler invocation does. The two eval macros are separable in the score but not in the work, because mobility and the king-zone attacker count are read off the same attack sets; disabling one drops its term and keeps the cost, and only disabling both skips the pass.
- For a heuristic with no switch, or for strength rather than tree size: build both sides, compare `bench` node counts, and settle strength with `scripts/compare_branches.py`. A local `#if` around the heuristic is a fine scaffold—do not ship it.
- When adding behavior, accompany it with focused unit tests; ensure `ctest --preset tests` passes.

## Commit & Pull Request Guidelines

- Commit messages follow Conventional Commits: `type(scope): subject`, e.g., `feat(movegen): add knight mobility bonus`. Common types: feat, fix, docs, test, refactor, perf, build, ci, chore.
- Subjects are imperative, ≤72 chars; scope optional but encouraged.
- PRs should include: summary of what/why, key commands run (include `ctest --preset tests`; lint/format commands), linked issue if exists, and any relevant logs (PGN/fastchess stats for engine strength changes).
- Strength-affecting changes (search, evaluation, move ordering) should quote the `bench` node count before and after—including "unchanged at N" when the change is meant to be behaviour-neutral—and, where the machine time allows, an SPRT verdict from `scripts/compare_branches.py`.
- CI is `.github/workflows/test.yml`: a lint/format job, a build-and-test matrix (linux x64 clang with `-Werror`, linux arm64 cross-compiled with gcc under qemu and advisory-only, macOS x64/arm64, Windows clang-cl), and a release-tests job on linux x64. `.github/workflows/strength-test.yml` adds an advisory perft benchmark and an SPRT run on PRs touching `src/**` or `include/**`, posting both as PR comments.
- Pre-submit gate: `make can-release` (format, lint, test), or the three commands by hand.

## Release Checklist

- Build Release: `cmake --preset release && cmake --build --preset release`; verify `build-release/c3` exists.
- Regenerate magics if needed: `make magic`; commit the updated `include/c3/magic.hpp` when it changes.
- Quality gates: `clang-format` over touched files, lint build clean, `ctest --preset tests` and `ctest --preset release-tests` green.
- Deep perft: `ctest --preset slow-tests` green—move generation is the one thing nothing downstream can compensate for.
- Record the `bench` signature of the release build (`printf 'bench\nquit\n' | build-release/c3 | tail -2`) in the release notes, so the next release has something to diff against.
- Optional strength check: `scripts/run_fastchess_gauntlet.py` against a known baseline, or `scripts/compare_branches.py` against the previous tag; capture the summary in `Testing/fastchess/` and attach it to the release notes or PR.
