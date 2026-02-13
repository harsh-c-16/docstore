#pragma once

// SSTable layout (docs/formats.md has the byte-level description):
//   [data block + crc]... [filter block + crc] [index block + crc] [footer: 48 bytes]
// Footer: index_offset u64 | index_size u64 | filter_offset u64 | filter_size u64 |
//         format_version u32 | crc32c u32 (of the previous 36 bytes) | magic u64
// Block sizes recorded in the index and footer exclude the 4-byte CRC that follows each block.
// Index entry: varint klen | last internal key of the block | offset u64 | size u32

#include <cstdint>

namespace docstore::kv {

constexpr uint64_t kTableMagic = 0x444F4353544F5245ull;  // "DOCSTORE"
constexpr uint32_t kTableFormatVersion = 1;
constexpr size_t kFooterSize = 48;
constexpr size_t kBlockTrailerSize = 4;  // crc32c

}  // namespace docstore::kv
