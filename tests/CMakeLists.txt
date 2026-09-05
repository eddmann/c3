include(FetchContent)
find_package(Python3 COMPONENTS Interpreter)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# Reuse an installed GoogleTest when one is available (CMake 3.24+ can hand the
# request to find_package first), so packaged/offline builds do not need the
# network at all. FETCHCONTENT_SOURCE_DIR_GOOGLETEST always wins over both.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(C3_GTEST_FIND_PACKAGE_ARGS FIND_PACKAGE_ARGS NAMES GTest)
else()
  set(C3_GTEST_FIND_PACKAGE_ARGS "")
endif()

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  # v1.14.0, pinned by commit rather than by tag, since a tag can be moved.
  # URL + URL_HASH on the release archive would pin the bytes exactly and cost
  # a ~1 MB download instead of a full clone, and is the better option whenever
  # the hash can be obtained; the archive was unreachable from the environment
  # this was changed in, so its hash could not be computed or verified, and a
  # commit sha is the strongest pin git offers. Do not add GIT_SHALLOW here: a
  # shallow fetch wants a branch or tag, not a commit sha.
  GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571
  ${C3_GTEST_FIND_PACKAGE_ARGS}
)

FetchContent_MakeAvailable(googletest)

if(CMAKE_CXX_CLANG_TIDY AND TARGET gtest)
  # Skip clang-tidy on third-party gtest to avoid noisy external diagnostics.
  set_target_properties(gtest PROPERTIES CXX_CLANG_TIDY "")
  set_target_properties(gtest_main PROPERTIES CXX_CLANG_TIDY "")
endif()

# Second copy of the engine sources, this time with C3_TESTING defined so the
# test-only helpers (e.g. c3::uci::run_script_for_test) are compiled. The define
# is PUBLIC so the test sources see the same declarations the library was built
# with; c3_core itself stays free of it and the shipped binary never carries the
# test scaffolding.
c3_add_core_library(c3_core_testing)
target_compile_definitions(c3_core_testing PUBLIC C3_TESTING)

if(CMAKE_CXX_CLANG_TIDY)
  # C3_TESTING only adds code, so linting this twin covers every translation
  # unit c3_core has plus the test-only helpers. Skipping the duplicate pass
  # halves the lint time and stops every diagnostic being reported twice.
  set_target_properties(c3_core PROPERTIES CXX_CLANG_TIDY "")
endif()

add_executable(c3_tests
  about_test.cpp
  board_test.cpp
  castling_test.cpp
  engine_test.cpp
  fen_test.cpp
  fixtures.cpp
  fixtures_test.cpp
  integration_test.cpp
  magic_test.cpp
  eval_test.cpp
  position_test.cpp
  primitives_test.cpp
  rng_test.cpp
  move_list_test.cpp
  movegen_test.cpp
  search_test.cpp
  uci_test.cpp
  zobrist_test.cpp
)
target_link_libraries(c3_tests PRIVATE c3_core_testing GTest::gtest_main)
target_compile_definitions(c3_tests PRIVATE C3_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
c3_apply_standard_settings(c3_tests)
add_dependencies(c3_tests generate_magic)

include(GoogleTest)
gtest_discover_tests(c3_tests)

# The deep perft fixtures skip themselves unless C3_SLOW_PERFT is set; this
# entry supplies it, so they can be run with `ctest -L slow`.
add_test(NAME Perft.SlowOptIn COMMAND c3_tests --gtest_filter=Perft.SlowFixturesMatch)
set_tests_properties(Perft.SlowOptIn PROPERTIES
  ENVIRONMENT "C3_SLOW_PERFT=1" LABELS slow TIMEOUT 900)

if(Python3_Interpreter_FOUND)
  add_test(NAME Fastchess.SummaryFixture
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run_fastchess_gauntlet.py
            --summarize-only ${PROJECT_SOURCE_DIR}/tests/fixtures/fastchess_sample.pgn
            --summary ${CMAKE_BINARY_DIR}/Testing/fastchess/sample_summary.txt)

  # Unit tests for the Python helpers behind the gauntlet/SPRT scripts.
  add_test(NAME Scripts.CommonUnitTests
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${PROJECT_SOURCE_DIR}/tests/scripts -t ${PROJECT_SOURCE_DIR}/tests/scripts -v)
endif()
