cmake_minimum_required(VERSION 3.16)

project(TaskForge
  VERSION 0.1.0
  DESCRIPTION "Lock-free work-stealing task scheduler with a DAG resolver"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Guard against in-source builds.
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not allowed. Run:\n"
    "  cmake --preset debug   (or release / asan / tsan)\n"
    "from the project root, which builds into ./build/<preset>.")
endif()

# ---------------------------------------------------------------------------
# Standard / options
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++17, never -std=gnu++17

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

option(TASKFORGE_BUILD_TESTS "Build the GoogleTest suite" ON)
option(TASKFORGE_BUILD_BENCH "Build the benchmark harness" ON)
option(TASKFORGE_WERROR      "Treat warnings as errors (CI turns this ON)" OFF)

# none | asan | tsan  -- see cmake/Sanitizers.cmake. Presets set this.
set(TASKFORGE_SANITIZER "none" CACHE STRING "Sanitizer to build with: none|asan|tsan")
set_property(CACHE TASKFORGE_SANITIZER PROPERTY STRINGS none asan tsan)

include(GNUInstallDirs)
include(CTest)                         # defines BUILD_TESTING (ON by default)

# ---------------------------------------------------------------------------
# Warnings: one INTERFACE target every TaskForge target links.
# Kept strict and identical for GCC and Clang. -Werror is opt-in so a local
# dev build is not blocked by a pedantic warning; CI passes -DTASKFORGE_WERROR=ON.
# ---------------------------------------------------------------------------
add_library(taskforge_warnings INTERFACE)
target_compile_options(taskforge_warnings INTERFACE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion
    $<$<BOOL:${TASKFORGE_WERROR}>:-Werror>
  >)

# ---------------------------------------------------------------------------
# Sanitizer flags -> taskforge_sanitize INTERFACE target
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)

# ---------------------------------------------------------------------------
# The library itself is header-only: an INTERFACE target carrying the include
# path plus the Threads dependency. Everything lives in include/taskforge/.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(taskforge INTERFACE)
add_library(TaskForge::taskforge ALIAS taskforge)
target_include_directories(taskforge INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(taskforge INTERFACE Threads::Threads)
target_compile_features(taskforge INTERFACE cxx_std_17)

# Convenience: an object-free "all flags" target the sub-projects link.
add_library(taskforge_build_settings INTERFACE)
target_link_libraries(taskforge_build_settings INTERFACE
  taskforge taskforge_warnings taskforge_sanitize)

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
if(TASKFORGE_BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(TASKFORGE_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Status summary
# ---------------------------------------------------------------------------
message(STATUS "TaskForge configuration")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  sanitizer    : ${TASKFORGE_SANITIZER}")
message(STATUS "  tests        : ${TASKFORGE_BUILD_TESTS}")
message(STATUS "  benchmarks   : ${TASKFORGE_BUILD_BENCH}")
message(STATUS "  -Werror      : ${TASKFORGE_WERROR}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
