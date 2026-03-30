#include "doc/field_path.h"

namespace docstore::doc {

Status SplitPath(std::string_view path, std::vector<std::string>* segments) {
  segments->clear();
  size_t start = 0;
  while (true) {
    size_t dot = path.find('.', start);
    std::string_view seg = path.substr(start, dot == std::string_view::npos ? dot : dot - start);
    if (seg.empty())
      return Status::InvalidArgument("empty segment in field path '" + std::string(path) + "'");
    if (seg[0] == '$')
      return Status::InvalidArgument("field path segment starts with '$': " + std::string(path));
    segments->emplace_back(seg);
    if (dot == std::string_view::npos) return Status::OK();
    start = dot + 1;
  }
}

const nlohmann::json* LookupPath(const nlohmann::json& doc, std::string_view path) {
  const nlohmann::json* cur = &doc;
  size_t start = 0;
  while (true) {
    if (!cur->is_object()) return nullptr;  // includes arrays met along the path
    size_t dot = path.find('.', start);
    std::string key(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
    auto it = cur->find(key);
    if (it == cur->end()) return nullptr;
    cur = &*it;
    if (dot == std::string_view::npos) return cur;
    start = dot + 1;
  }
}

}  // namespace docstore::doc
