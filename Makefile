# TaskForge convenience Makefile
# =============================================================================
# CMake (see CMakeLists.txt / CMakePresets.json) is the canonical build and the
# one CI uses. This Makefile exists so the project is buildable on a machine
# with only a C++17 compiler + make -- no cmake, no network, no GoogleTest.
#
# It builds:
#   * the benchmark harness           (bench/,  no test-framework dependency)
#   * the "verify/" programs          (assert-based, framework-free checks that
#                                      exercise the same include/taskforge/*.hpp
#                                      code the GoogleTest suite covers)
#
# It does NOT build tests/ -- those need GoogleTest; use CMake for them.
#
# Targets:
#   make              build everything, Debug-ish (-O2 -g), no sanitizer
#   make verify       build + RUN every verify/ program
#   make verify-tsan  same, ThreadSanitizer   (the important one on ARM)
#   make verify-asan  same, AddressSanitizer + UBSan
#   make bench        build the benchmark binary only
#   make buggy-demo   build verify_buggy_deque under TSan (expected to REPORT)
#   make clean
# =============================================================================

CXX      ?= c++
STD      := -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Wshadow
BASEOPT  := -O2 -g -pthread
INCLUDES := -Iinclude

TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g -O1 -pthread
ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1 -pthread

BUILD    := build
D_DEBUG  := $(BUILD)/debug
D_TSAN   := $(BUILD)/tsan
D_ASAN   := $(BUILD)/asan

VERIFY_SRCS := $(wildcard verify/verify_*.cpp)
VERIFY_DEBUG := $(patsubst verify/%.cpp,$(D_DEBUG)/%,$(VERIFY_SRCS))
VERIFY_TSAN  := $(patsubst verify/%.cpp,$(D_TSAN)/%,$(VERIFY_SRCS))
VERIFY_ASAN  := $(patsubst verify/%.cpp,$(D_ASAN)/%,$(VERIFY_SRCS))

BENCH_SRCS := $(wildcard bench/*.cpp)

.PHONY: all verify verify-tsan verify-asan bench buggy-demo clean dirs

all: $(VERIFY_DEBUG) $(D_DEBUG)/taskforge_bench

dirs:
	@mkdir -p $(D_DEBUG) $(D_TSAN) $(D_ASAN)

# ---- Debug-ish (no sanitizer) -------------------------------------------------
$(D_DEBUG)/%: verify/%.cpp | dirs
	$(CXX) $(STD) $(WARN) $(BASEOPT) $(INCLUDES) $< -o $@

$(D_DEBUG)/taskforge_bench: $(BENCH_SRCS) | dirs
	$(CXX) $(STD) $(WARN) $(BASEOPT) $(INCLUDES) $(BENCH_SRCS) -o $@

# ---- ThreadSanitizer --------------------------------------------------------
$(D_TSAN)/%: verify/%.cpp | dirs
	$(CXX) $(STD) $(WARN) $(TSAN_FLAGS) $(INCLUDES) $< -o $@

# ---- AddressSanitizer + UBSan ---------------------------------------------
$(D_ASAN)/%: verify/%.cpp | dirs
	$(CXX) $(STD) $(WARN) $(ASAN_FLAGS) $(INCLUDES) $< -o $@

# ---- run helpers ----------------------------------------------------------
verify: $(VERIFY_DEBUG)
	@echo "=== running verify/ (no sanitizer) ==="; \
	rc=0; for b in $(VERIFY_DEBUG); do \
	  echo "--- $$b"; "$$b" || rc=$$?; \
	done; \
	if [ $$rc -eq 0 ]; then echo "ALL VERIFY OK"; else echo "VERIFY FAILURES (rc=$$rc)"; fi; \
	exit $$rc

verify-tsan: $(VERIFY_TSAN)
	@echo "=== running verify/ under ThreadSanitizer ==="; \
	rc=0; for b in $(VERIFY_TSAN); do \
	  case "$$b" in *verify_buggy_deque) echo "--- $$b (SKIPPED here; see 'make buggy-demo')"; continue;; esac; \
	  echo "--- $$b"; TSAN_OPTIONS="halt_on_error=1" "$$b" || rc=$$?; \
	done; \
	if [ $$rc -eq 0 ]; then echo "ALL VERIFY-TSAN OK"; else echo "VERIFY-TSAN FAILURES (rc=$$rc)"; fi; \
	exit $$rc

verify-asan: $(VERIFY_ASAN)
	@echo "=== running verify/ under AddressSanitizer + UBSan ==="; \
	rc=0; for b in $(VERIFY_ASAN); do \
	  case "$$b" in *verify_buggy_deque) echo "--- $$b (SKIPPED here)"; continue;; esac; \
	  echo "--- $$b"; ASAN_OPTIONS="detect_leaks=1" "$$b" || rc=$$?; \
	done; \
	if [ $$rc -eq 0 ]; then echo "ALL VERIFY-ASAN OK"; else echo "VERIFY-ASAN FAILURES (rc=$$rc)"; fi; \
	exit $$rc

bench: $(D_DEBUG)/taskforge_bench
	@echo "built $<"

# Stage 9: this is SUPPOSED to trip TSan (and often to fail its value check on
# ARM). A clean run here would mean the demo is not demonstrating anything.
buggy-demo: $(D_TSAN)/verify_buggy_deque
	@echo "=== deliberately-wrong-ordering demo under TSan ==="
	@echo "Expect: a ThreadSanitizer data-race report, and/or a value-check abort."
	-TSAN_OPTIONS="halt_on_error=1" $(D_TSAN)/verify_buggy_deque
	@echo "(nonzero exit / race report above is the expected outcome)"

clean:
	rm -rf $(BUILD)
