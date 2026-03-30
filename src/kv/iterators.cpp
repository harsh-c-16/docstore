#include "kv/iterators.h"

#include <algorithm>

#include "kv/table_reader.h"
#include "kv/version_set.h"

namespace docstore::kv {
namespace {

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }
  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    Rebuild();
  }
  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    Rebuild();
  }
  void Next() override {
    std::pop_heap(heap_.begin(), heap_.end(), Greater());
    Iterator* top = heap_.back();
    heap_.pop_back();
    top->Next();
    Push(top);
  }
  std::string_view key() const override { return heap_.front()->key(); }
  std::string_view value() const override { return heap_.front()->value(); }
  Status status() const override { return status_; }

 private:
  // std heap functions build a max-heap; "greater" turns it into a min-heap on internal keys.
  struct Greater {
    bool operator()(const Iterator* a, const Iterator* b) const {
      return CompareInternalKeys(a->key(), b->key()) > 0;
    }
  };

  void Rebuild() {
    heap_.clear();
    for (auto& c : children_) Push(c.get());
  }

  void Push(Iterator* child) {
    if (!status_.ok()) return;
    if (child->Valid()) {
      heap_.push_back(child);
      std::push_heap(heap_.begin(), heap_.end(), Greater());
    } else if (!child->status().ok()) {
      status_ = child->status();
      heap_.clear();  // stop: results without this child would be silently incomplete
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;
  Status status_;
};

class LevelIterator final : public Iterator {
 public:
  explicit LevelIterator(const std::vector<std::shared_ptr<const FileMeta>>* files)
      : files_(files) {}

  bool Valid() const override { return table_iter_ && table_iter_->Valid(); }
  void SeekToFirst() override {
    OpenFile(0);
    if (table_iter_) table_iter_->SeekToFirst();
    SkipExhaustedFiles();
  }
  void Seek(std::string_view target) override {
    // First file whose largest key >= target; earlier files end before it.
    auto it = std::lower_bound(
        files_->begin(), files_->end(), target,
        [](const auto& f, std::string_view t) { return CompareInternalKeys(f->largest, t) < 0; });
    OpenFile(static_cast<size_t>(it - files_->begin()));
    if (table_iter_) table_iter_->Seek(target);
    SkipExhaustedFiles();
  }
  void Next() override {
    table_iter_->Next();
    SkipExhaustedFiles();
  }
  std::string_view key() const override { return table_iter_->key(); }
  std::string_view value() const override { return table_iter_->value(); }
  Status status() const override { return status_; }

 private:
  void OpenFile(size_t i) {
    table_iter_.reset();
    index_ = i;
    if (i < files_->size()) table_iter_ = (*files_)[i]->table->reader().NewIterator();
  }

  void SkipExhaustedFiles() {
    while (table_iter_ && !table_iter_->Valid()) {
      if (!table_iter_->status().ok()) {
        status_ = table_iter_->status();
        table_iter_.reset();
        return;
      }
      OpenFile(index_ + 1);
      if (table_iter_) table_iter_->SeekToFirst();
    }
  }

  const std::vector<std::shared_ptr<const FileMeta>>* files_;
  size_t index_ = 0;
  std::unique_ptr<Iterator> table_iter_;
  Status status_;
};

class DBIterator final : public Iterator {
 public:
  DBIterator(std::unique_ptr<Iterator> internal, SequenceNumber snapshot, std::string lower,
             std::string upper, std::shared_ptr<const void> pin)
      : pin_(std::move(pin)),
        iter_(std::move(internal)),
        snapshot_(snapshot),
        lower_(std::move(lower)),
        upper_(std::move(upper)) {}

  bool Valid() const override { return valid_; }
  void SeekToFirst() override { Seek(lower_); }
  // target is a *user* key.
  void Seek(std::string_view target) override {
    std::string_view start = target < std::string_view(lower_) ? std::string_view(lower_) : target;
    iter_->Seek(MakeLookupKey(start, snapshot_));
    has_skip_ = false;
    FindNextUserEntry();
  }
  void Next() override {
    // skip_key_ holds the current key: all its older versions are passed over.
    iter_->Next();
    FindNextUserEntry();
  }
  std::string_view key() const override { return skip_key_; }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return status_.ok() ? iter_->status() : status_; }

 private:
  void FindNextUserEntry() {
    valid_ = false;
    for (; iter_->Valid(); iter_->Next()) {
      ParsedInternalKey p;
      if (!ParseInternalKey(iter_->key(), &p)) {
        status_ = Status::Corruption("bad internal key in scan");
        return;
      }
      if (p.sequence > snapshot_) continue;                // written after this snapshot
      if (has_skip_ && p.user_key == skip_key_) continue;  // older version of a decided key
      if (!upper_.empty() && p.user_key >= std::string_view(upper_)) return;
      skip_key_.assign(p.user_key);
      has_skip_ = true;
      if (p.type == ValueType::kValue) {
        valid_ = true;
        return;
      }
      // Tombstone: the key is deleted at this snapshot; its older versions are skipped above.
    }
  }

  std::shared_ptr<const void> pin_;  // declared first: destroyed after iter_
  std::unique_ptr<Iterator> iter_;
  const SequenceNumber snapshot_;
  const std::string lower_, upper_;
  std::string skip_key_;
  bool has_skip_ = false;
  bool valid_ = false;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<Iterator> NewLevelIterator(
    const std::vector<std::shared_ptr<const FileMeta>>* files) {
  return std::make_unique<LevelIterator>(files);
}

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal, SequenceNumber snapshot,
                                        std::string lower, std::string upper,
                                        std::shared_ptr<const void> pin) {
  return std::make_unique<DBIterator>(std::move(internal), snapshot, std::move(lower),
                                      std::move(upper), std::move(pin));
}

}  // namespace docstore::kv
