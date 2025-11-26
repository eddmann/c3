# Repository Guidelines

## Project Structure & Module Organization

- `src/` holds engine sources (UCI glue, search, move generation, evaluation); headers live in `include/c3/` under the `c3` namespace.
- `tests/` contains GoogleTest units plus fixtures in `tests/fixtures/`; `Testing/` is an output dir created by test/gauntlet runs.
- `scripts/` provides helpers such as `run_fastchess_gauntlet.py`.
- Generated file: `include/c3/magic.hpp` is produced by the `generate_magic` target—never hand-edit.
- Build trees: `build/` (Debug + sanitizers), `build-release/` (Release + LTO), `build-tidy/` (Debug + clang-tidy).

## Build, Test, and Development Commands

- Configure & build Debug (sanitizers on): `cmake --preset debug && cmake --build --preset debug`.
- Release build: `cmake --preset release && cmake --build --preset release` → binary at `build-release/c3`.
- Lint build with clang-tidy: `cmake --preset lint && cmake --build --preset lint`.
- Unit tests (uses Debug tree): `ctest --preset tests`.
- Run engine locally: `build/c3` (Debug) or `build-release/c3` (Release).
- Gauntlet/regression: `python3 scripts/run_fastchess_gauntlet.py --opponent /path/to/engine --games 200 --concurrency 4`; add `--summarize-only <pgn>` to aggregate an existing run.

## Coding Style & Naming Conventions

- Formatting: `.clang-format` (LLVM base, 2-space indent, 100-col limit, sorted includes). Apply with `clang-format -i $(git ls-files '*.cpp' '*.hpp')`.
- Tooling: optional `.clang-tidy` (enable via `C3_ENABLE_CLANG_TIDY=ON` or the `lint` preset).
- Language: C++23, warnings enforced (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`); Debug uses ASan/UBSan.
- Names: prefer self-descriptive identifiers over inline comments—types/classes in `PascalCase`, functions/methods in `lower_snake_case`, constants/macros in `SCREAMING_SNAKE_CASE`; keep code within the `c3` namespace.
- Comments: add only when they capture chess-engine specifics (e.g., search heuristics rationale, magic bitboard edge cases) that are not obvious from well-named code.

## Testing Guidelines

- Framework: GoogleTest; add cases under `tests/*_test.cpp` using `TEST(Suite, Case)`.
- Keep fixtures in `tests/fixtures/`; prefer perft/PGN data for reproducibility.
- When adding behavior, accompany with focused unit tests; ensure `ctest --preset tests` passes.
- For movegen/search performance changes, capture a fastchess summary (PGN + stats) and reference it in the PR.

## Commit & Pull Request Guidelines

- Commit messages follow Conventional Commits: `type(scope): subject`, e.g., `feat(movegen): add knight mobility bonus`. Common types: feat, fix, docs, test, refactor, perf, build, ci, chore.
- Subjects are imperative, ≤72 chars; scope optional but encouraged.
- PRs should include: summary of what/why, key commands run (include `ctest --preset tests`; lint/format commands), linked issue if exists, and any relevant logs or screenshots (PGN/fastchess stats for engine strength changes).
- Pre-submit gate: format (`clang-format`), lint if applicable (`cmake --preset lint && cmake --build --preset lint`), and ensure tests pass (`ctest --preset tests`).

## Release Checklist

- Build Release: `cmake --preset release && cmake --build --preset release`; verify `build-release/c3` exists.
- Regenerate magics if needed: rebuilding will run `generate_magic`; commit the updated `include/c3/magic.hpp` when it changes.
- Quality gates: `clang-format` over touched files, lint build clean, `ctest --preset tests` green.
- Optional strength check: run `scripts/run_fastchess_gauntlet.py` against a known baseline and capture the summary in `Testing/fastchess/`; attach the PGN/summary to the release notes or PR.
