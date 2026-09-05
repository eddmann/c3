.DEFAULT_GOAL := help

.PHONY: *

help: ## Display this help message
	@awk 'BEGIN {FS = ":.*##"; printf "\nUsage:\n  make \033[36m<target>\033[0m\n"} /^[a-zA-Z\/_%-]+:.*?##/ { printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2 } /^##@/ { printf "\n\033[1m%s\033[0m\n", substr($$0, 5) } ' $(MAKEFILE_LIST)

##@ Development

build: ## Build debug binary (with sanitizers)
	cmake --preset debug
	cmake --build --preset debug

release: ## Build release binary (with LTO)
	cmake --preset release
	cmake --build --preset release

profile: ## Build -O2 binary with debug info for profilers (no LTO)
	cmake --preset relwithdebinfo
	cmake --build --preset relwithdebinfo

run: build ## Run debug binary
	./build/c3

run-release: release ## Run release binary
	./build-release/c3

fmt: ## Format code with clang-format
	clang-format -i $$(git ls-files '*.cpp' '*.hpp')

##@ Testing/Linting

test: build ## Run unit tests (debug tree, sanitizers)
	ctest --preset tests

test-release: ## Run unit tests against the optimized build
	cmake --preset release-tests
	cmake --build --preset release-tests
	ctest --preset release-tests

lint: ## Build with clang-tidy enabled
	cmake --preset lint
	cmake --build --preset lint

can-release: fmt lint test ## Run all CI checks (format, lint, test)

##@ Gauntlet Testing

gauntlet: release ## Run gauntlet vs opponent (OPPONENT=/path/to/engine GAMES=200)
	python3 scripts/run_fastchess_gauntlet.py --opponent $(OPPONENT) --games $(or $(GAMES),200) --concurrency 4

compare: release ## SPRT HEAD vs origin/main (GAMES=2000 DEPTH=8 ELO0=0 ELO1=5)
	python3 scripts/compare_branches.py --base origin/main --test HEAD --max-games $(or $(GAMES),2000) --depth $(or $(DEPTH),8) --elo0 $(or $(ELO0),0) --elo1 $(or $(ELO1),5) --openings tests/fixtures/openings.epd --concurrency 4

##@ Maintenance

clean: ## Clean all build directories
	rm -rf build build-release build-release-tests build-relwithdebinfo build-tidy

magic: ## Regenerate magic bitboard tables
	cmake --preset release -DC3_REGENERATE_MAGIC=ON
	cmake --build --preset release --target generate_magic
	cmake --preset release -DC3_REGENERATE_MAGIC=OFF
