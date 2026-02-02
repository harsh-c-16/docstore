#pragma once

// Internal keys: every version of a user key carries a sequence number and a type.
//   internal_key = user_key + fixed64((sequence << 8) | type)
// Ordering: user key ascending (bytewise), then sequence DESCENDING, so a seek lands on the
// newest version first.

#include <cstdint>
#include <string>
#include <string_view>

namespace docstore::kv {

using SequenceNumber = uint64_t;

// Sequence numbers use 56 bits so (seq << 8) | type fits in a u64.
constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;
constexpr size_t kInternalKeyTrailer = 8;

enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  return (seq << 8) | static_cast<uint8_t>(type);
}

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                       ValueType type);

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

// False if ikey is shorter than the trailer or has an unknown type byte.
bool ParseInternalKey(std::string_view ikey, ParsedInternalKey* out);

// Precondition: ikey.size() >= kInternalKeyTrailer (checked by callers that read from disk).
inline std::string_view ExtractUserKey(std::string_view ikey) {
  return ikey.substr(0, ikey.size() - kInternalKeyTrailer);
}

// <0, 0, >0 like memcmp. Precondition: both keys have a trailer.
int CompareInternalKeys(std::string_view a, std::string_view b);

// The internal key a lookup at `snapshot` seeks to: it sorts before every version of user_key
// with sequence <= snapshot and after every newer version.
std::string MakeLookupKey(std::string_view user_key, SequenceNumber snapshot);

}  // namespace docstore::kv
