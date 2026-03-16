#include "doc/matcher.h"

#include "doc/field_path.h"

namespace docstore::doc {
namespace {

using json = nlohmann::json;

bool IsScalar(const json& v) { return v.is_number() || v.is_string() || v.is_boolean(); }

bool ParseOp(const std::string& name, Op* op) {
  static const std::pair<const char*, Op> kOps[] = {{"$eq", Op::kEq}, {"$in", Op::kIn},
                                                    {"$gt", Op::kGt}, {"$gte", Op::kGte},
                                                    {"$lt", Op::kLt}, {"$lte", Op::kLte}};
  for (const auto& [n, o] : kOps) {
    if (name == n) {
      *op = o;
      return true;
    }
  }
  return false;
}

bool Kind(const json& v, int* kind) {
  if (v.is_number()) *kind = 0;
  else if (v.is_string()) *kind = 1;
  else if (v.is_boolean()) *kind = 2;
  else return false;
  return true;
}

bool ConditionHolds(const Condition& c, const json& value) {
  bool comparable = false;
  switch (c.op) {
    case Op::kEq:
      return CompareScalars(value, c.value, &comparable) == 0 && comparable;
    case Op::kIn:
      for (const auto& v : c.value) {
        if (CompareScalars(value, v, &comparable) == 0 && comparable) return true;
      }
      return false;
    default: {
      int r = CompareScalars(value, c.value, &comparable);
      if (!comparable) return false;  // range only matches values of the bound's type
      if (c.op == Op::kGt) return r > 0;
      if (c.op == Op::kGte) return r >= 0;
      if (c.op == Op::kLt) return r < 0;
      return r <= 0;
    }
  }
}

}  // namespace

int CompareScalars(const json& a, const json& b, bool* comparable) {
  int ka, kb;
  *comparable = Kind(a, &ka) && Kind(b, &kb) && ka == kb;
  if (!*comparable) return 0;
  if (ka == 0) {
    double x = a.get<double>(), y = b.get<double>();
    if (x != x || y != y) {  // NaN equals nothing and orders with nothing
      *comparable = false;
      return 0;
    }
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (ka == 1) {
    int r = a.get_ref<const std::string&>().compare(b.get_ref<const std::string&>());
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
  }
  return a.get<bool>() == b.get<bool>() ? 0 : (a.get<bool>() ? 1 : -1);
}

const FieldPredicate* Filter::Find(std::string_view path) const {
  for (const auto& f : fields) {
    if (f.path == path) return &f;
  }
  return nullptr;
}

Status ParseFilter(const json& filter, Filter* out) {
  out->fields.clear();
  if (!filter.is_object()) return Status::InvalidArgument("filter must be a JSON object");
  for (const auto& [path, value] : filter.items()) {
    std::vector<std::string> segments;
    Status s = SplitPath(path, &segments);  // also rejects top-level operators like $and
    if (!s.ok()) return s;
    FieldPredicate pred{path, {}};
    if (IsScalar(value)) {
      pred.conditions.push_back({Op::kEq, value});
    } else if (value.is_object() && !value.empty()) {
      for (const auto& [name, operand] : value.items()) {
        Op op;
        if (!ParseOp(name, &op)) {
          return Status::InvalidArgument("unsupported operator or embedded object for '" + path +
                                         "': " + name);
        }
        if (op == Op::kIn) {
          if (!operand.is_array()) return Status::InvalidArgument("$in needs an array");
          for (const auto& v : operand) {
            if (!IsScalar(v)) return Status::InvalidArgument("$in values must be scalars");
          }
        } else if (op == Op::kEq) {
          if (!IsScalar(operand)) return Status::InvalidArgument("$eq value must be a scalar");
        } else if (!operand.is_number() && !operand.is_string()) {
          return Status::InvalidArgument(name + " bound must be a number or a string");
        }
        pred.conditions.push_back({op, operand});
      }
    } else {
      return Status::InvalidArgument("unsupported filter value for '" + path +
                                     "' (null, arrays and embedded objects are not supported)");
    }
    out->fields.push_back(std::move(pred));
  }
  return Status::OK();
}

bool Matches(const Filter& filter, const json& doc) {
  for (const auto& pred : filter.fields) {
    const json* value = LookupPath(doc, pred.path);
    if (value == nullptr || !IsScalar(*value)) return false;
    for (const auto& c : pred.conditions) {
      if (!ConditionHolds(c, *value)) return false;
    }
  }
  return true;
}

}  // namespace docstore::doc
