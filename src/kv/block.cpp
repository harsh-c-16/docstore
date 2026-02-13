#include "kv/block.h"

#include "kv/dbformat.h"
#include "util/coding.h"

namespace docstore::kv {

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  PutVarint32(&buffer_, static_cast<uint32_t>(key.size()));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key);
  buffer_.append(value);
}

std::string BlockBuilder::Finish() {
  std::string out;
  out.swap(buffer_);
  return out;
}

namespace {

class BlockIterator final : public Iterator {
 public:
  explicit BlockIterator(std::shared_ptr<const Block> block)
      : block_(std::move(block)), data_(block_->data()) {}

  bool Valid() const override { return valid_; }
  void SeekToFirst() override {
    next_ = 0;
    ParseNext();
  }
  void Seek(std::string_view target) override {
    // Linear scan: blocks are small and entries have no restart points.
    for (SeekToFirst(); valid_ && CompareInternalKeys(key_, target) < 0; ParseNext()) {
    }
  }
  void Next() override { ParseNext(); }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  void ParseNext() {
    valid_ = false;
    if (!status_.ok() || next_ >= data_.size()) return;
    std::string_view in = data_.substr(next_);
    uint32_t klen, vlen;
    if (!GetVarint32(&in, &klen) || !GetVarint32(&in, &vlen) ||
        in.size() < static_cast<size_t>(klen) + vlen || klen < kInternalKeyTrailer) {
      status_ = Status::Corruption("bad entry in data block");
      return;
    }
    key_ = in.substr(0, klen);
    value_ = in.substr(klen, vlen);
    next_ = static_cast<size_t>(value_.data() + value_.size() - data_.data());
    valid_ = true;
  }

  std::shared_ptr<const Block> block_;
  std::string_view data_;
  size_t next_ = 0;
  bool valid_ = false;
  std::string_view key_, value_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> Block::NewIterator(std::shared_ptr<const Block> block) {
  return std::make_unique<BlockIterator>(std::move(block));
}

}  // namespace docstore::kv
