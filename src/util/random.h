#pragma once

// Small, fast, seedable PRNG (SplitMix64). Used for skiplist node heights and by tests
// that must be reproducible from a printed seed. Not cryptographic.

#include <cstdint>

namespace docstore {

class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed) {}

  uint64_t Next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // Uniform in [0, n). The modulo bias is negligible for the small n used here.
  uint64_t Uniform(uint64_t n) { return Next() % n; }
  bool OneIn(uint64_t n) { return Uniform(n) == 0; }

 private:
  uint64_t state_;
};

}  // namespace docstore
