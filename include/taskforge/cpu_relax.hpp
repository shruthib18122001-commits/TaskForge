// cpu_relax.hpp -- portable "I'm spinning, back off a little" hint.
//
// On a spin that is expected to succeed very soon (a few nanoseconds), issuing
// the architecture's pause/yield hint is better than std::this_thread::yield():
//   * x86:  PAUSE de-pipelines the spin, cuts the memory-order-violation
//           penalty when the lock is released, and frees the SMT sibling.
//   * ARM:  YIELD hints the core it may switch to the other SMT thread.
// Neither enters the kernel. std::this_thread::yield() may, and is the wrong
// tool for a 3-iteration spin. The thread pool escalates pause -> yield ->
// sleep; this is only the first rung.

#pragma once

namespace taskforge {

inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  // Unknown ISA: a compiler barrier is the safe minimum -- keeps the spin from
  // being hoisted/collapsed without claiming a HW hint we can't emit.
  __asm__ __volatile__("" ::: "memory");
#endif
}

}  // namespace taskforge
