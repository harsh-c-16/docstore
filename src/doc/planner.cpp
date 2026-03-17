#include "doc/planner.h"

#include <algorithm>
#include <optional>

#include "doc/key_codec.h"
#include "doc/keys.h"

namespace docstore::doc {
namespace {

using json = nlohmann::json;

struct Bound {
  json value;
  bool inclusive;
};

bool Has(const FieldPredicate& p, Op op) {
  return std::any_of(p.conditions.begin(), p.conditions.end(),
                     [op](const Condition& c) { return c.op == op; });
}

std::string Show(const json& v) { return v.dump(); }

ScanRange PointRange(const std::string& prefix, const json& v) {
  std::string enc;
  EncodeIndexValue(v, &enc);
  // Keys for value v are exactly the keys starting with prefix+enc(v) (encodings are
  // prefix-free), followed by the _id.
  return {prefix + enc, PrefixEnd(prefix + enc), "[" + Show(v) + ", " + Show(v) + "]"};
}

// Range over one type: the tightest lower and upper bounds of that type among the field's
// conditions. A document matching every condition must have that type, so this is a superset.
ScanRange RangeFor(const std::string& prefix, const FieldPredicate& pred) {
  int kind = -1;  // 0 number, 1 string: the type of the first range condition
  std::optional<Bound> lo, hi;
  for (const auto& c : pred.conditions) {
    if (c.op == Op::kEq || c.op == Op::kIn) continue;
    int k = c.value.is_number() ? 0 : 1;
    if (kind == -1) kind = k;
    if (k != kind) continue;
    bool comparable;
    if (c.op == Op::kGt || c.op == Op::kGte) {
      bool incl = c.op == Op::kGte;
      int r = lo ? CompareScalars(c.value, lo->value, &comparable) : 1;
      if (r > 0 || (r == 0 && !incl)) lo = Bound{c.value, incl};
    } else {
      bool incl = c.op == Op::kLte;
      int r = hi ? CompareScalars(c.value, hi->value, &comparable) : -1;
      if (r < 0 || (r == 0 && !incl)) hi = Bound{c.value, incl};
    }
  }
  const char tag = kind == 0 ? kNumberTag : kStringTag;
  ScanRange range;
  std::string enc;
  if (lo) {
    EncodeIndexValue(lo->value, &enc);
    range.lower = lo->inclusive ? prefix + enc : PrefixEnd(prefix + enc);
  } else {
    range.lower = prefix + tag;  // the smallest encoding of this type
  }
  if (hi) {
    EncodeIndexValue(hi->value, &enc);
    range.upper = hi->inclusive ? PrefixEnd(prefix + enc) : prefix + enc;
  } else {
    range.upper = prefix + static_cast<char>(tag + 1);  // past every encoding of this type
  }
  const char* min = kind == 0 ? "-inf.0" : "\"\"";
  const char* max = kind == 0 ? "inf.0" : "{}";
  range.display = std::string(lo ? (lo->inclusive ? "[" : "(") : "[") +
                  (lo ? Show(lo->value) : min) + ", " + (hi ? Show(hi->value) : max) +
                  (hi ? (hi->inclusive ? "]" : ")") : "]");
  return range;
}

}  // namespace

Plan ChoosePlan(const CollectionInfo& coll, const Filter& filter) {
  // Rank: 0 = has $eq, 1 = has $in, 2 = range only. Then field name.
  const IndexInfo* best = nullptr;
  int best_rank = 3;
  const FieldPredicate* best_pred = nullptr;
  for (const auto& idx : coll.indexes) {
    if (!idx.ready) continue;
    const FieldPredicate* pred = filter.Find(idx.field);
    if (pred == nullptr) continue;
    int rank = Has(*pred, Op::kEq) ? 0 : Has(*pred, Op::kIn) ? 1 : 2;
    if (rank < best_rank || (rank == best_rank && idx.field < best->field)) {
      best = &idx;
      best_rank = rank;
      best_pred = pred;
    }
  }
  Plan plan;
  if (best == nullptr) return plan;
  plan.collscan = false;
  plan.field = best->field;
  plan.index_id = best->id;
  const std::string prefix = IndexPrefix(coll.id, best->id);
  if (best_rank == 0) {
    for (const auto& c : best_pred->conditions) {
      if (c.op == Op::kEq) {
        plan.ranges.push_back(PointRange(prefix, c.value));
        break;
      }
    }
  } else if (best_rank == 1) {
    for (const auto& c : best_pred->conditions) {
      if (c.op != Op::kIn) continue;
      for (const auto& v : c.value) plan.ranges.push_back(PointRange(prefix, v));
      break;
    }
    // One scan per distinct value, in key order, so results come out in (value, _id) order.
    std::sort(plan.ranges.begin(), plan.ranges.end(),
              [](const ScanRange& a, const ScanRange& b) { return a.lower < b.lower; });
    plan.ranges.erase(std::unique(plan.ranges.begin(), plan.ranges.end(),
                                  [](const ScanRange& a, const ScanRange& b) {
                                    return a.lower == b.lower;
                                  }),
                      plan.ranges.end());
  } else {
    plan.ranges.push_back(RangeFor(prefix, *best_pred));
  }
  return plan;
}

json DescribePlan(const Plan& plan, const json& filter) {
  json stage;
  if (plan.collscan) {
    stage = {{"stage", "COLLSCAN"}, {"filter", filter}, {"direction", "forward"}};
  } else {
    json bounds = json::array();
    for (const auto& r : plan.ranges) bounds.push_back(r.display);
    stage = {{"stage", "FETCH"},
             {"filter", filter},
             {"inputStage",
              {{"stage", "IXSCAN"},
               {"indexName", plan.field + "_1"},
               {"keyPattern", {{plan.field, 1}}},
               {"indexBounds", {{plan.field, bounds}}}}}};
  }
  return {{"winningPlan", stage}};
}

}  // namespace docstore::doc
