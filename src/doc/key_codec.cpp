#include "doc/key_codec.h"

#include <bit>
#include <cmath>
#include <cstdint>

#include "util/coding.h"

namespace docstore::doc {

bool EncodeIndexValue(const nlohmann::json& v, std::string* out) {
  out->clear();
  if (v.is_number()) {
    double d = v.get<double>();  // all numbers compare as doubles (precision limit: 2^53)
    if (std::isnan(d)) return false;
    if (d == 0) d = 0.0;  // -0.0 and 0.0 are equal, so they must encode the same
    uint64_t bits = std::bit_cast<uint64_t>(d);
    // Negative doubles sort backwards as raw bits, so flip all of them; positive ones only
    // need the sign bit set so they sort above every negative.
    bits = (bits & 0x8000000000000000ull) ? ~bits : bits ^ 0x8000000000000000ull;
    out->push_back(kNumberTag);
    PutBigEndian64(out, bits);
    return true;
  }
  if (v.is_string()) {
    out->push_back(kStringTag);
    for (char c : v.get_ref<const std::string&>()) {
      out->push_back(c);
      if (c == '\0') out->push_back('\xff');  // escape, so 0x00 0x01 can only be the end
    }
    out->push_back('\0');
    out->push_back('\x01');
    return true;
  }
  if (v.is_boolean()) {
    out->push_back(kBoolTag);
    out->push_back(v.get<bool>() ? '\x01' : '\x00');
    return true;
  }
  return false;
}

bool DecodeIndexValue(std::string_view* in, nlohmann::json* out) {
  if (in->empty()) return false;
  const char tag = (*in)[0];
  if (tag == kNumberTag) {
    if (in->size() < 9) return false;
    uint64_t bits = DecodeBigEndian64(in->data() + 1);
    bits = (bits & 0x8000000000000000ull) ? bits ^ 0x8000000000000000ull : ~bits;
    *out = std::bit_cast<double>(bits);
    in->remove_prefix(9);
    return true;
  }
  if (tag == kStringTag) {
    std::string s;
    for (size_t i = 1; i + 1 < in->size(); i++) {
      char c = (*in)[i];
      if (c != '\0') {
        s.push_back(c);
        continue;
      }
      char next = (*in)[i + 1];
      if (next == '\x01') {
        *out = std::move(s);
        in->remove_prefix(i + 2);
        return true;
      }
      if (next != '\xff') return false;
      s.push_back('\0');
      i++;
    }
    return false;
  }
  if (tag == kBoolTag) {
    if (in->size() < 2 || static_cast<uint8_t>((*in)[1]) > 1) return false;
    *out = (*in)[1] == '\x01';
    in->remove_prefix(2);
    return true;
  }
  return false;
}

}  // namespace docstore::doc
