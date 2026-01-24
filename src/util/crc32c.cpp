#include "util/crc32c.h"

#include <array>

namespace docstore::crc32c {
namespace {

constexpr uint32_t kPolynomial = 0x82F63B78;  // reflected form of 0x1EDC6F41

// table[b] = CRC contribution of byte b, computed once at compile time.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? kPolynomial : 0);
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  // The standard pre/post inversion makes leading zero bytes change the checksum.
  uint32_t c = crc ^ 0xFFFFFFFFu;
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace docstore::crc32c
