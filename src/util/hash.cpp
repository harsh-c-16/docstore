#include "util/hash.h"

#include <bit>
#include <cstring>

namespace docstore {
namespace {

constexpr uint64_t kP1 = 11400714785074694791ull;
constexpr uint64_t kP2 = 14029467366897019727ull;
constexpr uint64_t kP3 = 1609587929392839161ull;
constexpr uint64_t kP4 = 9650029242287828579ull;
constexpr uint64_t kP5 = 2870177450012600261ull;

// xxHash is defined on little-endian reads; the target platform is x86-64.
static_assert(std::endian::native == std::endian::little);

uint64_t Read64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t Read32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint64_t Round(uint64_t acc, uint64_t input) {
  acc += input * kP2;
  acc = std::rotl(acc, 31);
  return acc * kP1;
}

uint64_t MergeRound(uint64_t acc, uint64_t val) {
  acc ^= Round(0, val);
  return acc * kP1 + kP4;
}

}  // namespace

uint64_t Hash64(std::string_view s, uint64_t seed) {
  const size_t n = s.size();
  const char* p = s.data();
  const char* end = p + n;
  uint64_t h;
  if (n >= 32) {
    // Four independent lanes over 32-byte stripes.
    uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
    while (p + 32 <= end) {
      v1 = Round(v1, Read64(p));
      v2 = Round(v2, Read64(p + 8));
      v3 = Round(v3, Read64(p + 16));
      v4 = Round(v4, Read64(p + 24));
      p += 32;
    }
    h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    h = MergeRound(h, v1);
    h = MergeRound(h, v2);
    h = MergeRound(h, v3);
    h = MergeRound(h, v4);
  } else {
    h = seed + kP5;
  }
  h += static_cast<uint64_t>(n);
  for (; p + 8 <= end; p += 8) h = std::rotl(h ^ Round(0, Read64(p)), 27) * kP1 + kP4;
  if (p + 4 <= end) {
    h = std::rotl(h ^ (static_cast<uint64_t>(Read32(p)) * kP1), 23) * kP2 + kP3;
    p += 4;
  }
  for (; p < end; p++) h = std::rotl(h ^ (static_cast<uint8_t>(*p) * kP5), 11) * kP1;
  // Final avalanche so every input bit affects every output bit.
  h ^= h >> 33;
  h *= kP2;
  h ^= h >> 29;
  h *= kP3;
  h ^= h >> 32;
  return h;
}

}  // namespace docstore
