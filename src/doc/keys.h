#pragma once

// How documents and index entries map onto the engine's byte keys:
//   0x00 "catalog"                                        -> catalog JSON
//   0x01 coll_id(u32 BE) _id                              -> BSON document
//   0x02 coll_id(u32 BE) index_id(u32 BE) enc(value) _id  -> empty
// Big-endian ids keep each collection (and each index) contiguous, so a collection scan is a
// prefix scan.

#include <cstdint>
#include <string>
#include <string_view>

#include "util/coding.h"

namespace docstore::doc {

constexpr char kCatalogTag = 0x00;
constexpr char kDocTag = 0x01;
constexpr char kIndexTag = 0x02;

inline std::string CatalogKey() { return std::string(1, kCatalogTag) + "catalog"; }

inline std::string CollectionPrefix(uint32_t coll_id) {
  std::string k(1, kDocTag);
  PutBigEndian32(&k, coll_id);
  return k;
}

inline std::string DocKey(uint32_t coll_id, std::string_view id) {
  return CollectionPrefix(coll_id).append(id);
}

inline std::string IndexPrefix(uint32_t coll_id, uint32_t index_id) {
  std::string k(1, kIndexTag);
  PutBigEndian32(&k, coll_id);
  PutBigEndian32(&k, index_id);
  return k;
}

// The smallest key greater than every key that starts with prefix ("" = no upper bound).
// Trailing 0xFF bytes cannot be incremented, so they are dropped first: the successor of
// "\x01\x00\x00\x00\xff" is "\x01\x00\x00\x01".
inline std::string PrefixEnd(std::string prefix) {
  while (!prefix.empty() && static_cast<uint8_t>(prefix.back()) == 0xFF) prefix.pop_back();
  if (!prefix.empty()) prefix.back() = static_cast<char>(static_cast<uint8_t>(prefix.back()) + 1);
  return prefix;
}

}  // namespace docstore::doc
