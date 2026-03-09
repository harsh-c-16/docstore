#pragma once

// Documents are stored as BSON (nlohmann::json::to_bson / from_bson). This file also holds the
// document rules MongoDB enforces: an object, at most 16 MiB as BSON, at most 100 levels deep,
// field names not starting with '$' and not containing '.', and a string _id of 1-512 bytes.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "docstore/status.h"

namespace docstore::doc {

constexpr size_t kMaxDocumentBytes = 16 << 20;
constexpr int kMaxDepth = 100;
constexpr size_t kMaxIdBytes = 512;

// Checks shape, names, depth and _id (which must be present).
Status ValidateDocument(const nlohmann::json& doc);
Status ValidateId(const nlohmann::json& id);

// InvalidArgument if the document cannot be BSON-encoded or is larger than 16 MiB.
Status EncodeDocument(const nlohmann::json& doc, std::string* out);
// Corruption if the bytes are not valid BSON.
Status DecodeDocument(std::string_view bson, nlohmann::json* out);

}  // namespace docstore::doc
