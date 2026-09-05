cmake_minimum_required(VERSION 3.20)

project(c3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(C3_ENABLE_CLANG_TIDY "Run clang-tidy during builds" OFF)
option(C3_BUILD_TESTS "Build the GoogleTest unit test suite" ON)
option(C3_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Some Debug invariants are too expensive to check on every node (for example
# rebuilding the incremental evaluation totals after each make/unmake). They
# are compiled in only when this is ON, so the everyday sanitizer build stays
# quick while a nightly or a suspicious bug hunt can still turn them on.
option(C3_EXPENSIVE_ASSERTS "Enable slow Debug-only consistency checks" OFF)

# ASan/UBSan are incompatible with MSVC debug runtime (-MDd) on Windows.
# Disable sanitizers by default on Windows to avoid the conflict.
if(WIN32)
  option(C3_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
  option(C3_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" OFF)
else()
  option(C3_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" ON)
  option(C3_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" ON)
endif()

if(C3_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "clang-tidy requested but clang-tidy not found on PATH")
  endif()
  set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
endif()

set(SANITIZER_FLAGS "")
if(C3_ENABLE_ASAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=address)
endif()
if(C3_ENABLE_UBSAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=undefined)
endif()
if(C3_ENABLE_ASAN OR C3_ENABLE_UBSAN)
  list(APPEND SANITIZER_FLAGS -fno-omit-frame-pointer)
endif()
if(MSVC)
  set(RELEASE_COMPILE_FLAGS /O2 /GL)
  set(RELEASE_LINK_FLAGS /LTCG)
  set(WARNING_FLAGS /W4)
  # Warnings are only fatal when asked for: contributors run a wide range of
  # compiler versions, so -Werror is opt-in locally and forced on in CI.
  if(C3_WARNINGS_AS_ERRORS)
    list(APPEND WARNING_FLAGS /WX)
  else()
    list(APPEND WARNING_FLAGS /WX-)
  endif()
else()
  set(RELEASE_COMPILE_FLAGS -O3 -flto)
  set(RELEASE_LINK_FLAGS -flto)
  set(WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(C3_WARNINGS_AS_ERRORS)
    list(APPEND WARNING_FLAGS -Werror)
  endif()
endif()

function(c3_apply_standard_settings target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  if(C3_EXPENSIVE_ASSERTS)
    target_compile_definitions(${target} PRIVATE C3_EXPENSIVE_ASSERTS)
  endif()
  target_compile_options(${target} PRIVATE
    ${WARNING_FLAGS}
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
    $<$<CONFIG:Release>:${RELEASE_COMPILE_FLAGS}>
  )
  target_link_options(${target} PRIVATE
    $<$<CONFIG:Debug>:${SANITIZER_FLAGS}>
    $<$<CONFIG:Release>:${RELEASE_LINK_FLAGS}>
  )
endfunction()

set(MAGIC_OUTPUT ${PROJECT_SOURCE_DIR}/include/c3/magic.hpp)

option(C3_REGENERATE_MAGIC "Regenerate magic.hpp" OFF)

if(C3_REGENERATE_MAGIC)
  add_executable(c3_magic_bitboard_generator
    src/magic_bitboard_generator.cpp
  )
  target_include_directories(c3_magic_bitboard_generator PRIVATE ${PROJECT_SOURCE_DIR}/include)
  c3_apply_standard_settings(c3_magic_bitboard_generator)

  add_custom_target(generate_magic
    COMMAND c3_magic_bitboard_generator ${MAGIC_OUTPUT}
    DEPENDS c3_magic_bitboard_generator
    COMMENT "Generating magic.hpp"
    VERBATIM
  )
else()
  add_custom_target(generate_magic)
endif()

# Absolute paths so the library can also be created from tests/ (see below).
set(C3_CORE_SOURCES
  ${PROJECT_SOURCE_DIR}/src/about.cpp
  ${PROJECT_SOURCE_DIR}/src/eval.cpp
  ${PROJECT_SOURCE_DIR}/src/fen.cpp
  ${PROJECT_SOURCE_DIR}/src/position.cpp
  ${PROJECT_SOURCE_DIR}/src/movegen.cpp
  ${PROJECT_SOURCE_DIR}/src/engine.cpp
  ${PROJECT_SOURCE_DIR}/src/uci.cpp
  ${PROJECT_SOURCE_DIR}/src/search.cpp
)

# The engine sources are compiled twice: once as `c3_core` for the shipped
# binary and once as `c3_core_testing` (in tests/) with C3_TESTING defined.
# Test-only helpers such as c3::uci::run_script_for_test live behind that
# define, so building them separately keeps them out of the released engine
# while the unit tests still get a library that exposes them.
# Side effect: compile_commands.json then holds two entries per source file,
# and an editor that picks the c3_core one greys out the C3_TESTING blocks as
# inactive code. That is the shipped configuration, so it is the honest view.
function(c3_add_core_library target)
  add_library(${target} STATIC ${C3_CORE_SOURCES})
  add_dependencies(${target} generate_magic)
  target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/include)
  c3_apply_standard_settings(${target})
endfunction()

c3_add_core_library(c3_core)

add_executable(c3
  src/main.cpp
)
target_link_libraries(c3 PRIVATE c3_core)
c3_apply_standard_settings(c3)

if(C3_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
