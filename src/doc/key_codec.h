#pragma once

// Order-preserving, self-delimiting encoding of index values, like MongoDB's KeyString.
// For two values of the same type, bytewise order of enc(a), enc(b) equals the order of a, b.
//   number : 0x10, then 8 bytes big-endian of the double's sortable bit pattern
//   string : 0x20, bytes with each 0x00 escaped as 0x00 0xFF, terminated by 0x00 0x01
//   boolean: 0x30, then 0x00 or 0x01
// Type order numbers < strings < booleans follows MongoDB's BSON comparison order.
// No encoding is a prefix of another, so the _id can simply follow it in an index key.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace docstore::doc {

constexpr char kNumberTag = 0x10;
constexpr char kStringTag = 0x20;
constexpr char kBoolTag = 0x30;

// Only numbers (not NaN), strings and booleans are indexable; returns false for anything else.
bool EncodeIndexValue(const nlohmann::json& v, std::string* out);
// Decodes one value from the front of *in and advances past it. False if malformed.
bool DecodeIndexValue(std::string_view* in, nlohmann::json* out);

}  // namespace docstore::doc
