include(FetchContent)
find_package(Python3 COMPONENTS Interpreter)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)

if(CMAKE_CXX_CLANG_TIDY)
  # Skip clang-tidy on third-party gtest to avoid noisy external diagnostics.
  set_target_properties(gtest PROPERTIES CXX_CLANG_TIDY "")
  set_target_properties(gtest_main PROPERTIES CXX_CLANG_TIDY "")
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
  movegen_test.cpp
  search_test.cpp
  uci_test.cpp
  zobrist_test.cpp
)
target_link_libraries(c3_tests PRIVATE c3_core GTest::gtest_main)
target_compile_definitions(c3_tests PRIVATE C3_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
target_compile_definitions(c3_tests PRIVATE C3_TESTING)
c3_apply_standard_settings(c3_tests)
add_dependencies(c3_tests generate_magic)

include(GoogleTest)
gtest_discover_tests(c3_tests)

if(Python3_Interpreter_FOUND)
  add_test(NAME Fastchess.SummaryFixture
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run_fastchess_gauntlet.py
            --summarize-only ${PROJECT_SOURCE_DIR}/tests/fixtures/fastchess_sample.pgn
            --summary ${CMAKE_BINARY_DIR}/Testing/fastchess/sample_summary.txt)
endif()
