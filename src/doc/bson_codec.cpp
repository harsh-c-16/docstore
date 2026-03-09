#include "doc/bson_codec.h"

#include <vector>

namespace docstore::doc {
namespace {

Status CheckValue(const nlohmann::json& v, int depth) {
  if (depth > kMaxDepth) return Status::InvalidArgument("document nested deeper than 100 levels");
  if (v.is_object()) {
    for (const auto& [name, child] : v.items()) {
      if (!name.empty() && name[0] == '$') {
        return Status::InvalidArgument("field name may not start with '$': " + name);
      }
      if (name.find('.') != std::string::npos) {
        return Status::InvalidArgument("field name may not contain '.': " + name);
      }
      Status s = CheckValue(child, depth + 1);
      if (!s.ok()) return s;
    }
  } else if (v.is_array()) {
    for (const auto& child : v) {
      Status s = CheckValue(child, depth + 1);
      if (!s.ok()) return s;
    }
  }
  return Status::OK();
}

}  // namespace

Status ValidateId(const nlohmann::json& id) {
  if (!id.is_string()) return Status::InvalidArgument("_id must be a string");
  const auto& s = id.get_ref<const std::string&>();
  if (s.empty() || s.size() > kMaxIdBytes) {
    return Status::InvalidArgument("_id must be 1 to 512 bytes long");
  }
  return Status::OK();
}

Status ValidateDocument(const nlohmann::json& doc) {
  if (!doc.is_object()) return Status::InvalidArgument("document must be a JSON object");
  auto id = doc.find("_id");
  if (id == doc.end()) return Status::InvalidArgument("document has no _id");
  Status s = ValidateId(*id);
  if (!s.ok()) return s;
  return CheckValue(doc, 1);
}

Status EncodeDocument(const nlohmann::json& doc, std::string* out) {
  std::vector<uint8_t> bytes;
  try {
    bytes = nlohmann::json::to_bson(doc);
  } catch (const nlohmann::json::exception& e) {
    // e.g. integers above INT64_MAX or field names containing NUL have no BSON form.
    return Status::InvalidArgument(std::string("cannot encode document as BSON: ") + e.what());
  }
  if (bytes.size() > kMaxDocumentBytes) {
    return Status::InvalidArgument("document is larger than 16 MiB as BSON");
  }
  out->assign(bytes.begin(), bytes.end());
  return Status::OK();
}

Status DecodeDocument(std::string_view bson, nlohmann::json* out) {
  try {
    *out = nlohmann::json::from_bson(bson.begin(), bson.end());
  } catch (const nlohmann::json::exception& e) {
    return Status::Corruption(std::string("stored document is not valid BSON: ") + e.what());
  }
  return Status::OK();
}

}  // namespace docstore::doc
