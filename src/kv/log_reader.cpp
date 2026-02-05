#include "kv/log_reader.h"

#include "kv/log_writer.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace docstore::kv {
namespace {

uint32_t RecordCrc(const char* length_field, std::string_view payload) {
  return crc32c::Extend(crc32c::Value(length_field, 4), payload.data(), payload.size());
}

}  // namespace

bool LogReader::ReadRecord(std::string_view* record) {
  if (problem_ != Problem::kNone) return false;
  if (pos_ == data_.size()) return false;  // clean end
  if (data_.size() - pos_ < kLogHeaderSize) {
    problem_ = Problem::kIncomplete;
    return false;
  }
  const char* header = data_.data() + pos_;
  uint32_t crc = DecodeFixed32(header);
  uint32_t length = DecodeFixed32(header + 4);
  if (length > kMaxLogRecordSize) {
    problem_ = Problem::kOversized;
    return false;
  }
  if (data_.size() - pos_ - kLogHeaderSize < length) {
    problem_ = Problem::kIncomplete;
    return false;
  }
  std::string_view payload = data_.substr(pos_ + kLogHeaderSize, length);
  if (RecordCrc(header + 4, payload) != crc) {
    problem_ = Problem::kChecksum;
    return false;
  }
  pos_ += kLogHeaderSize + length;
  valid_end_ = pos_;
  *record = payload;
  return true;
}

Status LogReader::status() const {
  switch (problem_) {
    case Problem::kNone:
      return Status::OK();
    case Problem::kIncomplete:
      return Status::Corruption("truncated log record at offset " + std::to_string(pos_));
    case Problem::kChecksum:
      return Status::Corruption("log record checksum mismatch at offset " + std::to_string(pos_));
    case Problem::kOversized:
      return Status::Corruption("log record length too large at offset " + std::to_string(pos_));
  }
  return Status::Corruption("unknown log problem");
}

bool LogReader::ValidRecordAt(size_t offset, size_t* end) const {
  if (data_.size() - offset < kLogHeaderSize) return false;
  const char* header = data_.data() + offset;
  uint32_t length = DecodeFixed32(header + 4);
  if (length > kMaxLogRecordSize || data_.size() - offset - kLogHeaderSize < length) return false;
  if (RecordCrc(header + 4, data_.substr(offset + kLogHeaderSize, length)) !=
      DecodeFixed32(header)) {
    return false;
  }
  *end = offset + kLogHeaderSize + length;
  return true;
}

bool LogReader::BadRecordIsTornTail() const {
  if (problem_ != Problem::kIncomplete) return false;
  size_t remaining = data_.size() - pos_;
  if (remaining >= kLogHeaderSize) {
    // If the CRC matches with the length the record would have had if it ended exactly at end
    // of file, the record is complete and its length field was damaged.
    char length_field[4];
    EncodeFixed32(length_field, static_cast<uint32_t>(remaining - kLogHeaderSize));
    std::string_view payload = data_.substr(pos_ + kLogHeaderSize);
    if (RecordCrc(length_field, payload) == DecodeFixed32(data_.data() + pos_)) return false;
  }
  // A damaged length can make a middle record look cut off; then later records still verify.
  // An interrupted append is the last thing in the file, so nothing valid follows it.
  for (size_t off = pos_ + 1; off + kLogHeaderSize <= data_.size(); off++) {
    size_t end;
    if (ValidRecordAt(off, &end)) return false;
  }
  return true;
}

}  // namespace docstore::kv
