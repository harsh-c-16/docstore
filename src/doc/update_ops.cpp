#include "doc/update_ops.h"

#include <limits>
#include <string>
#include <vector>

#include "doc/field_path.h"

namespace docstore::doc {
namespace {

using json = nlohmann::json;

bool IsPrefixPath(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.size() > b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// Walks to the parent object of the last segment. With create=true, missing intermediates
// become empty objects; an existing non-object intermediate is an error.
Status Parent(json* doc, const std::vector<std::string>& path, bool create, json** parent) {
  json* cur = doc;
  for (size_t i = 0; i + 1 < path.size(); i++) {
    auto it = cur->find(path[i]);
    if (it == cur->end()) {
      if (!create) {
        *parent = nullptr;
        return Status::OK();
      }
      cur = &(*cur)[path[i]];
      *cur = json::object();
    } else if (it->is_object()) {
      cur = &*it;
    } else {
      if (!create) {
        *parent = nullptr;
        return Status::OK();
      }
      return Status::InvalidArgument("cannot create field '" + path[i + 1] + "' inside non-object '" +
                                     path[i] + "'");
    }
  }
  *parent = cur;
  return Status::OK();
}

Status Increment(json* target, const json& by) {
  if (target->is_null()) {
    *target = by;
    return Status::OK();
  }
  if (!target->is_number()) return Status::InvalidArgument("$inc on a non-numeric field");
  if (target->is_number_integer() && by.is_number_integer()) {
    int64_t a = target->get<int64_t>(), b = by.get<int64_t>(), sum;
    if (!__builtin_add_overflow(a, b, &sum)) {
      *target = sum;
      return Status::OK();
    }
  }
  *target = target->get<double>() + by.get<double>();
  return Status::OK();
}

}  // namespace

Status ApplyUpdate(const json& update, json* doc) {
  if (!update.is_object() || update.empty()) {
    return Status::InvalidArgument("update must be a non-empty object of operators");
  }
  std::vector<std::vector<std::string>> seen;
  for (const auto& [op, fields] : update.items()) {
    if (op != "$set" && op != "$unset" && op != "$inc") {
      return Status::InvalidArgument("unknown update operator '" + op + "'");
    }
    if (!fields.is_object() || fields.empty()) {
      return Status::InvalidArgument(op + " needs a non-empty object of fields");
    }
    for (const auto& [path_str, value] : fields.items()) {
      std::vector<std::string> path;
      Status s = SplitPath(path_str, &path);
      if (!s.ok()) return s;
      for (const auto& other : seen) {
        if (IsPrefixPath(other, path) || IsPrefixPath(path, other)) {
          return Status::InvalidArgument("update paths conflict at '" + path_str + "'");
        }
      }
      seen.push_back(path);
      if (op == "$inc" && !value.is_number()) {
        return Status::InvalidArgument("$inc value must be a number");
      }
      json* parent = nullptr;
      s = Parent(doc, path, /*create=*/op != "$unset", &parent);
      if (!s.ok()) return s;
      if (op == "$set") {
        (*parent)[path.back()] = value;
      } else if (op == "$unset") {
        if (parent != nullptr) parent->erase(path.back());
      } else {
        s = Increment(&(*parent)[path.back()], value);
        if (!s.ok()) return s;
      }
    }
  }
  return Status::OK();
}

}  // namespace docstore::doc
