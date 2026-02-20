#pragma once

// Iterator building blocks for scans:
//  * MergingIterator: min-heap over child iterators of internal keys.
//  * LevelIterator: walks the sorted, disjoint L1 files, opening one table at a time.
//  * DBIterator: turns the merged stream of internal keys into user-visible key/values at one
//    snapshot: hides entries newer than the snapshot, keeps only the newest version of each
//    user key, skips tombstones, and honours [lower, upper) bounds.

#include <memory>
#include <string>
#include <vector>

#include "kv/dbformat.h"
#include "kv/iterator.h"
#include "kv/version_edit.h"

namespace docstore::kv {

// If a child stops because of an error, the merged iterator stops too and reports it, rather
// than silently continuing without that child's keys.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

// files must be sorted and disjoint (L1), and must outlive the iterator.
std::unique_ptr<Iterator> NewLevelIterator(
    const std::vector<std::shared_ptr<const FileMeta>>* files);

// `pin` keeps whatever the internal iterator reads from (memtables, Version) alive.
// An empty upper bound means unbounded.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal,
                                        SequenceNumber snapshot, std::string lower,
                                        std::string upper, std::shared_ptr<const void> pin);

}  // namespace docstore::kv
