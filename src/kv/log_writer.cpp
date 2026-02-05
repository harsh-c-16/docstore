#include "kv/log_writer.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace docstore::kv {

Status LogWriter::AddRecord(std::string_view payload) {
  if (payload.size() > kMaxLogRecordSize) return Status::InvalidArgument("log record too large");
  char header[kLogHeaderSize];
  EncodeFixed32(header + 4, static_cast<uint32_t>(payload.size()));
  // CRC over length + payload, computed with Extend so the two never need to be concatenated.
  uint32_t crc = crc32c::Value(header + 4, 4);
  crc = crc32c::Extend(crc, payload.data(), payload.size());
  EncodeFixed32(header, crc);

  std::lock_guard<std::mutex> l(mu_);
  Status s = file_->Append(std::string_view(header, kLogHeaderSize));
  if (s.ok()) s = file_->Append(payload);
  if (s.ok()) s = file_->Flush();
  if (s.ok()) bytes_written_ += kLogHeaderSize + payload.size();
  return s;
}

Status LogWriter::Sync() {
  {
    std::lock_guard<std::mutex> l(mu_);
    Status s = file_->Flush();  // normally a no-op: AddRecord already flushed
    if (!s.ok()) return s;
  }
  return file_->Sync();
}

uint64_t LogWriter::bytes_written() const {
  std::lock_guard<std::mutex> l(mu_);
  return bytes_written_;
}

}  // namespace docstore::kv
