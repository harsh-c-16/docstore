#pragma once

// Query filters. A filter is an object; {} matches everything. Each key is a
// field path whose value is a scalar (implicit $eq) or an object of operators from
// $eq $gt $gte $lt $lte $in. Everything is ANDed.
//
// Matching rules, which are exactly what makes index results identical to collection-scan
// results: the document's value at the path must exist and be a scalar (number, string or
// boolean). Numbers compare as doubles, strings bytewise, booleans by equality only, and a range
// comparison only matches values of the bound's own type.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docstore/status.h"

namespace docstore::doc {

enum class Op { kEq, kIn, kGt, kGte, kLt, kLte };

struct Condition {
  Op op;
  nlohmann::json value;  // a scalar, or for $in an array of scalars
};

struct FieldPredicate {
  std::string path;
  std::vector<Condition> conditions;
};

struct Filter {
  std::vector<FieldPredicate> fields;  // sorted by path (JSON object order)
  const FieldPredicate* Find(std::string_view path) const;
};

Status ParseFilter(const nlohmann::json& filter, Filter* out);
bool Matches(const Filter& filter, const nlohmann::json& doc);

// Three-way comparison of two scalars of the same kind (both numbers, both strings or both
// booleans); returns false in *comparable otherwise.
int CompareScalars(const nlohmann::json& a, const nlohmann::json& b, bool* comparable);

}  // namespace docstore::doc
