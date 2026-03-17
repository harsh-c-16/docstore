#pragma once

// Runs a Plan against one engine Snapshot, so the index entries and the documents they point
// to come from the same consistent state.
//   COLLSCAN: iterate the collection prefix in _id order, apply the filter.
//   IXSCAN + FETCH: for each range, iterate index keys in (value, _id) order, fetch each _id,
//                   then apply the full filter to the document (the residual filter).

#include <vector>

#include <nlohmann/json.hpp>

#include "doc/matcher.h"
#include "doc/planner.h"
#include "docstore/status.h"
#include "kv/snapshot.h"

namespace docstore::doc {

struct ExecutionStats {
  uint64_t returned = 0;
  uint64_t keys_examined = 0;
  uint64_t docs_examined = 0;
};

Status Execute(const kv::Snapshot& snapshot, uint32_t coll_id, const Filter& filter,
               const Plan& plan, size_t limit, std::vector<nlohmann::json>* out,
               ExecutionStats* stats);

}  // namespace docstore::doc
