// tf_verify.hpp -- a small assert-with-a-message helper for the verify/
// programs. NOT a test framework and NOT the deliverable test surface: the
// GoogleTest suite in tests/ is. This exists so the headers can be exercised
// (incl. under TSan/ASan) on a machine that has only a compiler + make.
//
// Each verify/ program has its own main(), runs a handful of checks, prints
// "<name>: OK", and returns 0. On failure it prints the location and aborts so
// a sanitizer or the shell sees a nonzero exit.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace tf_verify {

inline int& failure_count() {
  static int n = 0;
  return n;
}

[[noreturn]] inline void fail(const char* expr, const char* file, int line,
                              const std::string& extra) {
  ++failure_count();
  std::fprintf(stderr, "  CHECK FAILED  %s:%d\n    %s\n", file, line, expr);
  if (!extra.empty()) std::fprintf(stderr, "    %s\n", extra.c_str());
  std::fflush(stderr);
  // Abort rather than continue: keeps sanitizer output attributable and makes
  // the shell / CI see a hard failure.
  std::abort();
}

inline void check(bool ok, const char* expr, const char* file, int line,
                  const std::string& extra = {}) {
  if (!ok) fail(expr, file, line, extra);
}

// Rendered only when a comparison fails, and only for types printf can take.
template <class T>
std::string render(const T& v) {
  if constexpr (std::is_integral_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(static_cast<double>(v));
  } else if constexpr (std::is_pointer_v<T>) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(v));
    return buf;
  } else {
    return "<?>";
  }
}

// Evaluates a and b exactly once (they are function arguments). Works for any
// equality-comparable pair; no std::to_string requirement on T.
template <class A, class B>
void check_eq(const A& a, const B& b, const char* expr, const char* file,
              int line) {
  if (a == b) return;
  fail(expr, file, line, "lhs=" + render(a) + "  rhs=" + render(b));
}

}  // namespace tf_verify

#define TF_CHECK(cond) ::tf_verify::check((cond), #cond, __FILE__, __LINE__)

#define TF_CHECK_EQ(a, b) \
  ::tf_verify::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)

#define TF_CHECK_MSG(cond, msg) \
  ::tf_verify::check((cond), #cond, __FILE__, __LINE__, (msg))

#define TF_PASS(name)                                    \
  do {                                                   \
    std::printf("%s: OK\n", (name));                     \
    std::fflush(stdout);                                 \
    return ::tf_verify::failure_count() == 0 ? 0 : 1;    \
  } while (0)
