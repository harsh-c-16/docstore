#pragma once

// Update operators applied to a copy of the stored document:
//   $set   {path: value}   creates intermediate objects as needed
//   $unset {path: ""}      removing a missing field is a no-op
//   $inc   {path: number}  the field must be numeric or missing (treated as 0)
// Two operators may not touch the same path or a path and its prefix ("a" and "a.b"), as in
// MongoDB, so the result never depends on the order the operators are applied in.

#include <nlohmann/json.hpp>

#include "docstore/status.h"

namespace docstore::doc {

Status ApplyUpdate(const nlohmann::json& update, nlohmann::json* doc);

}  // namespace docstore::doc
