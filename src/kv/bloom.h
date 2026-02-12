#pragma once

// Bloom filter over the user keys of one SSTable. "No" is always right (no false negatives);
// "maybe" is wrong with probability ~0.8% at 10 bits per key.
//
// Layout: bit array (bytes) | k (1 byte). k = clamp(round(bits_per_key * ln 2), 1, 30).
// Probes use double hashing from one 64-bit hash h: delta = rotr(h, 17),
// probe i = (h + i * delta) mod bits.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace docstore::kv {

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key);
  void AddKey(std::string_view user_key);
  // Serialized filter for all keys added so far.
  std::string Finish() const;
  size_t num_keys() const { return hashes_.size(); }

 private:
  int bits_per_key_;
  int k_;
  std::vector<uint64_t> hashes_;
};

// A malformed or empty filter answers "maybe" (it can never cause a false negative).
bool BloomKeyMayMatch(std::string_view filter, std::string_view user_key);

uint64_t BloomHash(std::string_view user_key);

}  // namespace docstore::kv
