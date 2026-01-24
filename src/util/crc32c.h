#pragma once

// CRC32C (Castagnoli, reflected polynomial 0x82F63B78): the checksum on every WAL record,
// SSTable block, MANIFEST record and footer. Software, table-driven: one table lookup per byte.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace docstore::crc32c {

// Returns the CRC of concat(A, data[0, n)) where crc is the CRC of A.
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(std::string_view s) { return Extend(0, s.data(), s.size()); }

}  // namespace docstore::crc32c
