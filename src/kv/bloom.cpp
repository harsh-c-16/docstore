#include "kv/bloom.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "util/hash.h"

namespace docstore::kv {

uint64_t BloomHash(std::string_view user_key) { return Hash64(user_key, 0xb100f11e); }

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key)
    : bits_per_key_(bits_per_key),
      k_(std::clamp(static_cast<int>(std::lround(bits_per_key * 0.69314718056)), 1, 30)) {}

void BloomFilterBuilder::AddKey(std::string_view user_key) {
  hashes_.push_back(BloomHash(user_key));
}

std::string BloomFilterBuilder::Finish() const {
  // A minimum of 64 bits keeps the false-positive rate sane for tiny tables.
  size_t bits = std::max<size_t>(64, hashes_.size() * static_cast<size_t>(bits_per_key_));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string filter(bytes, '\0');
  for (uint64_t h : hashes_) {
    const uint64_t delta = std::rotr(h, 17);
    for (int i = 0; i < k_; i++) {
      uint64_t bit = (h + static_cast<uint64_t>(i) * delta) % bits;
      filter[bit / 8] = static_cast<char>(static_cast<uint8_t>(filter[bit / 8]) | (1u << (bit % 8)));
    }
  }
  filter.push_back(static_cast<char>(k_));
  return filter;
}

bool BloomKeyMayMatch(std::string_view filter, std::string_view user_key) {
  if (filter.size() < 2) return true;
  const int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > 30) return true;  // unknown encoding: never claim absence
  const uint64_t bits = (filter.size() - 1) * 8;
  const uint64_t h = BloomHash(user_key);
  const uint64_t delta = std::rotr(h, 17);
  for (int i = 0; i < k; i++) {
    uint64_t bit = (h + static_cast<uint64_t>(i) * delta) % bits;
    if ((static_cast<uint8_t>(filter[bit / 8]) & (1u << (bit % 8))) == 0) return false;
  }
  return true;
}

}  // namespace docstore::kv
