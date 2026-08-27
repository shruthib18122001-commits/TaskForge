# Benchmark harness. One executable, subcommand-dispatched (see bench_main.cpp).
# Emits CSV to stdout or --out FILE. No GoogleTest dependency.

add_executable(taskforge_bench
  bench_main.cpp
  workload.cpp)

target_link_libraries(taskforge_bench PRIVATE taskforge_build_settings)

# Release-grade optimisation for the harness even in a Debug tree: we are
# measuring the scheduler, not the benchmark scaffolding. Sanitizer builds keep
# their -O1 from taskforge_sanitize (added after, wins).
target_compile_options(taskforge_bench PRIVATE
  $<$<AND:$<CONFIG:Debug>,$<STREQUAL:${TASKFORGE_SANITIZER},none>>:-O2>)

# Linux-only thread pinning helper (pthread_setaffinity_np).
include(CheckCXXSymbolExists)
check_cxx_symbol_exists(pthread_setaffinity_np "pthread.h" TF_HAVE_SETAFFINITY)
if(TF_HAVE_SETAFFINITY)
  target_compile_definitions(taskforge_bench PRIVATE TF_HAVE_SETAFFINITY=1)
endif()
