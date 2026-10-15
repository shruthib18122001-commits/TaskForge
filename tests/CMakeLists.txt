# GoogleTest suite.
#
# GoogleTest is pulled in with FetchContent (pinned tag). If a system install is
# already present we use that instead and skip the download.

include(FetchContent)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GoogleTest: fetching v1.15.2 via FetchContent")
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
else()
  message(STATUS "GoogleTest: using system package")
endif()

include(GoogleTest)

# Our targets compile with -Wconversion -Wsign-conversion -Wold-style-cast
# -Werror. GoogleTest's headers do not survive that. Mark its include dirs as
# SYSTEM so warnings originating inside <gtest/*.h> (incl. macro bodies) are
# suppressed, while warnings on OUR expressions passed to EXPECT_/ASSERT_ still
# fire.
foreach(_gt gtest gtest_main gmock gmock_main)
  if(TARGET ${_gt})
    get_target_property(_inc ${_gt} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${_gt} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endforeach()

add_library(taskforge_test_main INTERFACE)
target_link_libraries(taskforge_test_main INTERFACE
  taskforge_build_settings
  GTest::gtest GTest::gtest_main)

# tf_add_test(<name> [LABELS "a;b"] SOURCES a.cpp ...)
#   Registers a GoogleTest executable and discovers its cases into ctest.
#   Tests labelled "buggy-demo" are excluded from the default run in CI
#   (ctest --preset ... is invoked with -LE buggy-demo there); run them with
#   `ctest -L buggy-demo`.
function(tf_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LABELS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE taskforge_test_main)
  set(_labels "unit")
  if(ARG_LABELS)
    set(_labels "${ARG_LABELS}")
  endif()
  # Sanitizer runtime options are set by the caller's environment (the CI
  # workflow sets ASAN_OPTIONS=detect_leaks=1:... on Linux; macOS ASan does not
  # support detect_leaks and *aborts* if asked, so we must not bake it in here).
  gtest_discover_tests(${name}
    PROPERTIES
      TIMEOUT 600
      LABELS "${_labels}"
    DISCOVERY_TIMEOUT 60)
endfunction()

# ---- Stage 1 -----------------------------------------------------------------
tf_add_test(test_smoke SOURCES test_smoke.cpp)

# ---- Stage 2: Chase-Lev deque -------------------------------------------
tf_add_test(test_deque_single      SOURCES test_deque_single.cpp)
tf_add_test(test_deque_concurrent  SOURCES test_deque_concurrent.cpp)
tf_add_test(test_deque_stress      SOURCES test_deque_stress.cpp)

# ---- Stage 3: work-stealing thread pool ------------------------------
tf_add_test(test_thread_pool       SOURCES test_thread_pool.cpp)
tf_add_test(test_pool_shutdown     SOURCES test_pool_shutdown.cpp)
tf_add_test(test_exception_safety  SOURCES test_exception_safety.cpp)

# ---- Stage 4: baselines ------------------------------------------------------
tf_add_test(test_baselines         SOURCES test_baselines.cpp)

# ---- Stage 6: DAG resolver --------------------------------------------
tf_add_test(test_dag               SOURCES test_dag.cpp)
tf_add_test(test_dag_cycle         SOURCES test_dag_cycle.cpp)

# ---- Stage 7: steal-rate instrumentation ---------------------------
tf_add_test(test_steal_stats       SOURCES test_steal_stats.cpp)

# ---- Stage 8: false-sharing experiment ----------------------------
tf_add_test(test_false_sharing     SOURCES test_false_sharing.cpp)

# ---- Stage 9: the deliberately-wrong deque -------------------------
# A value-check stress harness for the relaxed variant. NOT expected to be
# sanitizer-clean; that is the demonstration. Labelled so the default CI run
# skips it (`-LE buggy-demo`); run explicitly with `ctest -L buggy-demo`.
tf_add_test(test_relaxed_deque_demo SOURCES test_relaxed_deque_demo.cpp
  LABELS "buggy-demo")

# ---- Phase 2 / P2-1: numerical core (rng.hpp, black_scholes.hpp) ----
tf_add_test(test_risk_black_scholes SOURCES test_risk_black_scholes.cpp)
tf_add_test(test_risk_rng           SOURCES test_risk_rng.cpp)
tf_add_test(test_risk_montecarlo    SOURCES test_risk_montecarlo.cpp)

# ---- Phase 2 / P2-2: serial reference engine (portfolio.hpp, engine.hpp) ----
tf_add_test(test_risk_portfolio     SOURCES test_risk_portfolio.cpp)
tf_add_test(test_risk_engine        SOURCES test_risk_engine.cpp)

# ---- Phase 2 / P2-3: parallel engine (ThreadPool, MutexQueuePool, AsyncPool) ----
tf_add_test(test_risk_engine_parallel SOURCES test_risk_engine_parallel.cpp)
