#pragma once

// Dotted field paths ("address.city"). Field names may not contain '.', so a path is
// unambiguous. An array met along a path makes the field "not a scalar": lookups through it
// return nothing, so it is neither indexed nor matched.

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "docstore/status.h"

namespace docstore::doc {

// Splits and validates: no empty segment, no segment starting with '$'.
Status SplitPath(std::string_view path, std::vector<std::string>* segments);

// The value at path, or nullptr if missing or if an intermediate value is not an object.
const nlohmann::json* LookupPath(const nlohmann::json& doc, std::string_view path);

}  // namespace docstore::doc
