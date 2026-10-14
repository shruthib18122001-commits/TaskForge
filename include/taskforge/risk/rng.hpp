// rng.hpp -- deterministic, chunk-seeded random numbers for the risk engine.
// =============================================================================
// Three pieces:
//   * SplitMix64   a tiny, fully-specified 64-bit uniform generator.
//   * chunk_seed() derives a chunk's seed from (global_seed, chunk_index) only
//                  -- never thread id, never time -- so which worker happens
//                  to run a chunk, or how many workers there are, cannot
//                  change what that chunk computes.
//   * RngStream    wraps SplitMix64 and layers a hand-written Box-Muller
//                  uniform-to-normal transform on top.
//
// Why not std::mt19937 + std::normal_distribution, which C++17 already gives
// us for free:
//
//   std::mt19937's OWN algorithm is fully specified by the standard (the
//   recurrence and tempering constants are spelled out in [rand.eng.mers]),
//   so it genuinely is bit-identical across conforming implementations. It is
//   not the problem.
//
//   std::normal_distribution is the problem. The standard only constrains its
//   *output distribution*, not the algorithm that produces it from a uniform
//   source ([rand.dist.norm.normal] gives the density function, not an
//   implementation). libstdc++ and libc++ use different transforms (and have
//   changed them across versions), so the same std::mt19937 bit stream feeds
//   into std::normal_distribution and comes out as DIFFERENT normal variates
//   depending on which standard library you linked -- silently. That breaks
//   the one property this engine is built around: same seed, same answer,
//   forever, on any pool, any thread count, any day. So the normal transform
//   is hand-written here (Box-Muller) instead, where every operation is
//   something *we* wrote and can reason about.
//
//   SplitMix64 is used in place of std::mt19937 anyway, not because mt19937
//   is wrong, but because this engine constructs one RNG stream PER CHUNK --
//   potentially millions of them over a run -- and mt19937's ~2.5 KB of state
//   is wasteful to construct and discard at that rate, where SplitMix64's 8
//   bytes is not. It is a well-known, good-quality generator (Steele, Lea &
//   Flood 2014; this is Vigna's public-domain reference form) and, like
//   mt19937, defined purely by fixed-width unsigned integer arithmetic --
//   unsigned overflow wraps modulo 2^64 by definition ([basic.fundamental]),
//   so there is no implementation-defined or undefined behaviour anywhere in
//   it to diverge on.
//
// What is NOT claimed to be bit-identical across platforms: the Box-Muller
// transform below calls std::log/std::sqrt/std::sin/std::cos, and the C++
// standard does not require <cmath> transcendental functions to be
// correctly-rounded, so a ULP-level difference between two libm
// implementations (glibc vs Apple's libm, say) is possible in the last bit.
// This is a universal, accepted property of floating point -- Black-Scholes
// itself leans on std::exp/std::log/std::erfc in black_scholes.hpp, so
// nothing in this engine could be fully libm-independent without hand-writing
// correctly-rounded transcendental functions, which is out of scope. What
// determinism.md (DESIGN.md, Phase 2 section) actually promises and the tests
// actually assert is the property the project needs: on one machine, with one
// compiler and one libm, the same seed gives bit-identical results regardless
// of thread count or which of the three pools ran it.
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>

namespace taskforge::risk {

// ---------------------------------------------------------------------------
// SplitMix64 (Steele, Lea & Flood 2014 / Vigna public-domain reference impl).
// 8 bytes of state, one add + three xor-multiply-shift rounds per output.
// Full period 2^64; this engine never draws remotely close to that many
// values from one stream, so period is a non-issue here.
// ---------------------------------------------------------------------------
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next_u64() {
    state_ += 0x9E3779B97F4A7C15ULL;
    return avalanche(state_);
  }

  // Uniform double in [0, 1), using the full 53-bit double mantissa (the
  // standard "take the top 53 bits of a 64-bit integer, scale by 2^-53"
  // construction -- unbiased, and every representable output is equally
  // likely up to the generator's own quality).
  double next_uniform() {
    constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
    return static_cast<double>(next_u64() >> 11) * kScale;
  }

  // The mixing step alone, exposed so chunk_seed() (below) can reuse it as a
  // general-purpose avalanche without needing a live SplitMix64 instance.
  static std::uint64_t avalanche(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Derive a chunk's seed from (global_seed, chunk_index) alone.
//
// This is a PURE function: no loop, no shared state, same output no matter
// which thread calls it, no matter when, no matter how many times it has
// already been called for other chunk indices. That purity is exactly what
// makes the chunking scheme's determinism work -- chunk 7 computes the same
// thing whether it happens to run on worker 0 of a 1-thread pool or worker 5
// of an 8-thread work-stealing pool, because "which RNG stream chunk 7 gets"
// never depended on anything about the run except the chunk index itself.
//
// Each input is avalanched separately first (so chunk_index's sequential
// structure -- 0, 1, 2, 3, ... -- does not leak into low-order bits of the
// combined seed), then the two are combined and avalanched once more. This is
// a standard, non-cryptographic hash-combine; it is not trying to resist an
// adversary, only to turn two small/structured integers into two
// well-separated 64-bit seeds.
// ---------------------------------------------------------------------------
inline std::uint64_t chunk_seed(std::uint64_t global_seed, std::uint64_t chunk_index) {
  const std::uint64_t a = SplitMix64::avalanche(global_seed ^ 0x9E3779B97F4A7C15ULL);
  const std::uint64_t b = SplitMix64::avalanche(chunk_index + 0xBF58476D1CE4E5B9ULL);
  return SplitMix64::avalanche(a ^ (b + 0x9E3779B97F4A7C15ULL));
}

// ---------------------------------------------------------------------------
// RngStream: a SplitMix64 uniform source plus a hand-written Box-Muller
// uniform-to-normal transform. One instance per chunk; constructed with
// chunk_seed(global_seed, chunk_index), used linearly within that chunk's
// single thread, then discarded. Not thread-safe to share across threads --
// it is never meant to be, each chunk owns exactly one.
// ---------------------------------------------------------------------------
class RngStream {
 public:
  explicit RngStream(std::uint64_t seed) : uniform_(seed) {}

  // Uniform in [0, 1).
  double next_uniform() { return uniform_.next_uniform(); }

  // Standard normal N(0, 1), via the classic (non-polar) Box-Muller
  // transform: two independent uniforms u1, u2 in (0, 1] x [0, 1) produce two
  // independent standard normals
  //     z0 = sqrt(-2 ln u1) * cos(2*pi*u2)
  //     z1 = sqrt(-2 ln u1) * sin(2*pi*u2)
  // One call to next_standard_normal() either returns a cached z1 from the
  // previous pair (O(1), no transcendental calls) or computes a fresh pair
  // (consumes exactly 2 uniform draws, 1 sqrt + 1 log + 1 sin + 1 cos) and
  // caches the second value for the following call.
  double next_standard_normal() {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    // u1 must be in (0, 1], not [0, 1): log(0) is -inf. SplitMix64's 53-bit
    // construction can land exactly on 0.0 with probability 2^-53 -- reroll
    // on the rare chance rather than ever propagate a NaN.
    double u1 = next_uniform();
    while (u1 <= 0.0) u1 = next_uniform();
    const double u2 = next_uniform();

    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = kTwoPi * u2;
    spare_ = r * std::sin(theta);
    has_spare_ = true;
    return r * std::cos(theta);
  }

 private:
  static constexpr double kTwoPi = 6.283185307179586476925286766559;

  SplitMix64 uniform_;
  double spare_ = 0.0;
  bool has_spare_ = false;
};

}  // namespace taskforge::risk
