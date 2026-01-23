#pragma once

// Byte encodings shared by every on-disk format: fixed-width little-endian integers, varints,
// length-prefixed strings, and big-endian integers (used where bytewise order must match
// numeric order, e.g. collection ids inside keys).

#include <cstdint>
#include <string>
#include <string_view>

namespace docstore {

constexpr int kMaxVarint32Bytes = 5;
constexpr int kMaxVarint64Bytes = 10;

inline void EncodeFixed32(char* dst, uint32_t v) {
  for (int i = 0; i < 4; i++) dst[i] = static_cast<char>(v >> (8 * i));
}
inline void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; i++) dst[i] = static_cast<char>(v >> (8 * i));
}
inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}
inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

void PutFixed32(std::string* dst, uint32_t v);
void PutFixed64(std::string* dst, uint64_t v);
void PutVarint32(std::string* dst, uint32_t v);
void PutVarint64(std::string* dst, uint64_t v);
void PutLengthPrefixed(std::string* dst, std::string_view value);

// Writes a varint at dst (which must have room) and returns the byte after it.
char* EncodeVarint64(char* dst, uint64_t v);
int VarintLength(uint64_t v);

// The Get* functions consume bytes from the front of *in. They return false (and leave *in in an
// unspecified state) if the input is truncated or malformed.
bool GetFixed32(std::string_view* in, uint32_t* v);
bool GetFixed64(std::string_view* in, uint64_t* v);
bool GetVarint32(std::string_view* in, uint32_t* v);
bool GetVarint64(std::string_view* in, uint64_t* v);
bool GetLengthPrefixed(std::string_view* in, std::string_view* value);

void PutBigEndian32(std::string* dst, uint32_t v);
uint32_t DecodeBigEndian32(const char* p);
void PutBigEndian64(std::string* dst, uint64_t v);
uint64_t DecodeBigEndian64(const char* p);

}  // namespace docstore
