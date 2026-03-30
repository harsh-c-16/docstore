#pragma once

// Query planner: picks an index scan or a collection scan.
// Candidates are *ready* indexes on fields that appear in the filter. Priority: a field with
// $eq, then a field with $in, then a field with a range; ties go to the smallest field name.
// The chosen index only has to return a superset of the matches: the executor applies the full
// filter to every fetched document (the residual filter).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc/catalog.h"
#include "doc/matcher.h"

namespace docstore::doc {

// One contiguous range of index keys [lower, upper) in engine key space, plus how to show it.
struct ScanRange {
  std::string lower;
  std::string upper;
  std::string display;  // "[18, 30)" as in MongoDB's explain
};

struct Plan {
  bool collscan = true;
  std::string field;  // indexed field (IXSCAN only)
  uint32_t index_id = 0;
  std::vector<ScanRange> ranges;  // in key order; $in gives one point range per value
};

Plan ChoosePlan(const CollectionInfo& coll, const Filter& filter);

// MongoDB-shaped "queryPlanner" section of explain.
nlohmann::json DescribePlan(const Plan& plan, const nlohmann::json& filter);

}  // namespace docstore::doc
