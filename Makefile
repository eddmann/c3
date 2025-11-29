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

run: build ## Run debug binary
	./build/c3

run-release: release ## Run release binary
	./build-release/c3

fmt: ## Format code with clang-format
	clang-format -i $$(git ls-files '*.cpp' '*.hpp')

##@ Testing/Linting

test: build ## Run unit tests
	ctest --preset tests

lint: ## Build with clang-tidy enabled
	cmake --preset lint
	cmake --build --preset lint

can-release: fmt lint test ## Run all CI checks (format, lint, test)

##@ Gauntlet Testing

gauntlet: release ## Run gauntlet vs opponent (OPPONENT=/path/to/engine GAMES=200)
	python3 scripts/run_fastchess_gauntlet.py --opponent $(OPPONENT) --games $(or $(GAMES),200) --concurrency 4

compare: release ## Compare HEAD vs origin/main (GAMES=500 DEPTH=8)
	python3 scripts/compare_branches.py --base origin/main --test HEAD --games $(or $(GAMES),500) --depth $(or $(DEPTH),8) --openings tests/fixtures/openings.epd --concurrency 4

##@ Maintenance

clean: ## Clean all build directories
	rm -rf build build-release build-tidy

magic: ## Regenerate magic bitboard tables
	cmake --preset release -DC3_REGENERATE_MAGIC=ON
	cmake --build --preset release --target generate_magic
	cmake --preset release -DC3_REGENERATE_MAGIC=OFF
