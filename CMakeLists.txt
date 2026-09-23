# klattsch native engine.
#
# The product build: the library, the tools, and the stage verifiers as tests.
# Stage directories keep their own scratch scripts; this is what assembles the
# thing, from the same sources, on every platform it is built on.
#
# The exit test of stage 6 requires MSVC, clang-cl and gcc to produce identical
# samples, and that only works if it has been possible to run all three all
# along -- which is why this file exists at stage 1 rather than at the end.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure

cmake_minimum_required(VERSION 3.16)

project(klattsch VERSION 0.8.0 LANGUAGES C)

# C17: C11 with its defect reports fixed, and the newest dialect every target
# compiler actually has. MSVC 19.51 has no /std:c23 at all, and its `clatest`
# is missing constexpr, nullptr, the bool keyword and #embed -- see
# docs/REWRITE.md, "Which C", and tools/probe-c23/ for the measurement.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 64-bit only, per house rule 4 of the tree's CLAUDE.md. Fail loudly rather
# than quietly producing something nobody wants.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "32-bit builds are not produced here. Configure for x64 (or ARM64).")
endif()

# Warnings on, because a port is exactly where an implicit conversion or an
# uninitialised read hides.
if(MSVC)
  add_compile_options(/W4)
  # The dump tool writes binary to stdout and uses _setmode; MSVC's "secure
  # CRT" warnings about fwrite/fopen are noise for a build with no untrusted
  # input at all.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# --- the library ------------------------------------------------------------

add_library(klattsch STATIC
  csrc/kl_dsp.c
)
target_include_directories(klattsch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/csrc)

# libm, where it is a separate library.
include(CheckLibraryExists)
check_library_exists(m sin "" HAVE_LIB_M)
if(HAVE_LIB_M)
  target_link_libraries(klattsch PUBLIC m)
endif()

# --- stage verification tools ----------------------------------------------

add_executable(kl_dsp_dump csrc/tools/kl_dsp_dump.c)
target_link_libraries(kl_dsp_dump PRIVATE klattsch)

# --- tests ------------------------------------------------------------------

include(CTest)

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  # Stage 1: the DSP primitives against the JavaScript they came from.
  add_test(NAME stage1-dsp
    COMMAND ${NODE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-stage1.mjs
            $<TARGET_FILE:kl_dsp_dump>)
  set_tests_properties(stage1-dsp PROPERTIES LABELS "validation;stage1")

  # The goldens must match the engine for any stage comparison to mean
  # anything, so this is a precondition rather than a separate concern.
  add_test(NAME goldens-current
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/goldens.mjs --check)
  set_tests_properties(goldens-current PROPERTIES LABELS "quick")

  add_test(NAME banks-current
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build-banks.js --check)
  set_tests_properties(banks-current PROPERTIES LABELS "quick")
else()
  message(WARNING "node not found: the stage verifiers compare against the "
                  "JavaScript reference and will not be registered")
endif()
