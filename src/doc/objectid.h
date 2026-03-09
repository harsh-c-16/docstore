#pragma once

// MongoDB-style ObjectId: 4-byte big-endian Unix seconds | 5-byte per-process random value |
// 3-byte counter, rendered as 24 lowercase hex characters. Ids from one process increase
// (time first, then the counter), so later inserts sort after earlier ones.

#include <string>

namespace docstore::doc {

// Thread-safe.
std::string GenerateObjectId();

}  // namespace docstore::doc
