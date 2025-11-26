cmake_minimum_required(VERSION 3.20)

project(c3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(C3_ENABLE_CLANG_TIDY "Run clang-tidy during builds" OFF)

if(C3_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "clang-tidy requested but clang-tidy not found on PATH")
  endif()
  set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
endif()

set(SANITIZER_FLAGS -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
set(RELEASE_COMPILE_FLAGS -O3 -flto)
set(RELEASE_LINK_FLAGS -flto)
set(WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

function(c3_apply_standard_settings target)
  target_compile_features(${target} PUBLIC cxx_std_23)
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

add_library(c3_core STATIC
  src/about.cpp
  src/eval.cpp
  src/fen.cpp
  src/position.cpp
  src/movegen.cpp
  src/engine.cpp
  src/uci.cpp
  src/search.cpp
)
target_compile_definitions(c3_core PUBLIC C3_TESTING)
add_dependencies(c3_core generate_magic)
target_include_directories(c3_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
c3_apply_standard_settings(c3_core)

add_executable(c3
  src/main.cpp
)
target_link_libraries(c3 PRIVATE c3_core)
c3_apply_standard_settings(c3)

enable_testing()
add_subdirectory(tests)
