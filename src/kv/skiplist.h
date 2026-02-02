#pragma once

// LevelDB-style skiplist: one writer, any number of lock-free readers.
//
// Concurrency contract (DESIGN.md D9):
//  * Insert() must be externally serialized (the engine's write mutex guarantees one writer).
//  * Readers need no lock. Nodes are never removed while the list is alive, so a reader can
//    never follow a pointer to freed memory.
//  * A new node's own next pointers are set with relaxed stores, then the node is published by
//    a release store into its predecessor. A reader's acquire load of that pointer therefore
//    also sees the node's key and next pointers fully initialised.

#include <atomic>
#include <cassert>
#include <new>

#include "util/arena.h"
#include "util/random.h"

namespace docstore::kv {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  SkipList(Comparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), rnd_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; i++) head_->SetNext(i, nullptr);
  }
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Precondition: no equal key is already present (internal keys are unique by sequence).
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || compare_(key, x->key) != 0);
    (void)x;

    int height = RandomHeight();
    int max_height = GetMaxHeight();
    if (height > max_height) {
      for (int i = max_height; i < height; i++) prev[i] = head_;
      // Relaxed is enough: a reader that sees the new height before the node is linked just
      // finds nullptr at the new levels of head_ and drops down a level.
      max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; i++) {
      // Not yet reachable by readers, so no ordering needed for the node's own pointers.
      x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      // Publish: release store makes the initialised node visible to acquire readers.
      prev[i]->SetNext(i, x);
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && compare_(key, x->key) == 0;
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;  // each level holds ~1/4 of the nodes below it

  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;

    Node* Next(int n) const { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) const { return next_[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

    // Variable length: a node of height h is allocated with h slots (see NewNode).
    std::atomic<Node*> next_[1];
  };

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
    Node* node = new (mem) Node(key);
    for (int i = 1; i < height; i++) new (&node->next_[i]) std::atomic<Node*>(nullptr);
    return node;
  }

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && rnd_.OneIn(kBranching)) height++;
    return height;
  }

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }

  // First node with key >= `key`. If prev is non-null, fills prev[level] with the last node
  // before that position on every level (the insertion points).
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (KeyIsAfterNode(key, next)) {
        x = next;  // keep moving right on this level
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        level--;  // drop down a level
      }
    }
  }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  Random rnd_;  // only touched by the single writer
};

}  // namespace docstore::kv
