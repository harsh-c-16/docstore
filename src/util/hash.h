#pragma once

// xxHash64 (Yann Collet's algorithm, reimplemented): fast 64-bit hash used by the bloom filter
// and for choosing block-cache shards. Not cryptographic.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace docstore {

// Only a string_view overload: a (const char*, size_t) overload would silently capture
// Hash64("abc", 42) as length 42 instead of seed 42.
uint64_t Hash64(std::string_view s, uint64_t seed = 0);

}  // namespace docstore
