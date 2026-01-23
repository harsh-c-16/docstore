#include "util/coding.h"

namespace docstore {

void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, 4);
}

void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, 8);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 0x80) {
    *p++ = static_cast<unsigned char>(v | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}

void PutVarint32(std::string* dst, uint32_t v) { PutVarint64(dst, v); }

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[kMaxVarint64Bytes];
  char* end = EncodeVarint64(buf, v);
  dst->append(buf, end - buf);
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    len++;
  }
  return len;
}

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

bool GetFixed32(std::string_view* in, uint32_t* v) {
  if (in->size() < 4) return false;
  *v = DecodeFixed32(in->data());
  in->remove_prefix(4);
  return true;
}

bool GetFixed64(std::string_view* in, uint64_t* v) {
  if (in->size() < 8) return false;
  *v = DecodeFixed64(in->data());
  in->remove_prefix(8);
  return true;
}

bool GetVarint64(std::string_view* in, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0, i = 0; i < kMaxVarint64Bytes && i < static_cast<int>(in->size());
       i++, shift += 7) {
    uint64_t byte = static_cast<uint8_t>((*in)[i]);
    result |= (byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      // The 10th byte may only carry the single top bit of a 64-bit value.
      if (i == kMaxVarint64Bytes - 1 && byte > 1) return false;
      *v = result;
      in->remove_prefix(i + 1);
      return true;
    }
  }
  return false;
}

bool GetVarint32(std::string_view* in, uint32_t* v) {
  std::string_view copy = *in;
  uint64_t wide;
  if (!GetVarint64(&copy, &wide) || wide > UINT32_MAX) return false;
  *v = static_cast<uint32_t>(wide);
  *in = copy;
  return true;
}

bool GetLengthPrefixed(std::string_view* in, std::string_view* value) {
  uint32_t len;
  if (!GetVarint32(in, &len) || in->size() < len) return false;
  *value = in->substr(0, len);
  in->remove_prefix(len);
  return true;
}

void PutBigEndian32(std::string* dst, uint32_t v) {
  for (int i = 3; i >= 0; i--) dst->push_back(static_cast<char>(v >> (8 * i)));
}

uint32_t DecodeBigEndian32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

void PutBigEndian64(std::string* dst, uint64_t v) {
  for (int i = 7; i >= 0; i--) dst->push_back(static_cast<char>(v >> (8 * i)));
}

uint64_t DecodeBigEndian64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

}  // namespace docstore
