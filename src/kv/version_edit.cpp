#include "kv/version_edit.h"

#include "util/coding.h"

namespace docstore::kv {
namespace {

enum Tag : uint32_t {
  kLogNumber = 1,
  kNextFileNumber = 2,
  kLastSequence = 3,
  kAddFile = 4,
  kDeleteFile = 5,
};

bool GetLevel(std::string_view* in, int* level) {
  uint32_t v;
  if (!GetVarint32(in, &v) || v >= kNumLevels) return false;
  *level = static_cast<int>(v);
  return true;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, *last_sequence);
  }
  for (const auto& [level, number] : deleted_files) {
    PutVarint32(dst, kDeleteFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files) {
    PutVarint32(dst, kAddFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  *this = VersionEdit();
  std::string_view in = src;
  while (!in.empty()) {
    uint32_t tag;
    uint64_t v;
    if (!GetVarint32(&in, &tag)) return Status::Corruption("bad version edit tag");
    switch (tag) {
      case kLogNumber:
        if (!GetVarint64(&in, &v)) return Status::Corruption("bad log number");
        log_number = v;
        break;
      case kNextFileNumber:
        if (!GetVarint64(&in, &v)) return Status::Corruption("bad next file number");
        next_file_number = v;
        break;
      case kLastSequence:
        if (!GetVarint64(&in, &v)) return Status::Corruption("bad last sequence");
        last_sequence = v;
        break;
      case kAddFile: {
        int level;
        FileMeta f;
        std::string_view smallest, largest;
        if (!GetLevel(&in, &level) || !GetVarint64(&in, &f.number) ||
            !GetVarint64(&in, &f.file_size) || !GetLengthPrefixed(&in, &smallest) ||
            !GetLengthPrefixed(&in, &largest) || smallest.size() < kInternalKeyTrailer ||
            largest.size() < kInternalKeyTrailer) {
          return Status::Corruption("bad add-file entry");
        }
        f.smallest.assign(smallest);
        f.largest.assign(largest);
        new_files.emplace_back(level, std::move(f));
        break;
      }
      case kDeleteFile: {
        int level;
        if (!GetLevel(&in, &level) || !GetVarint64(&in, &v)) {
          return Status::Corruption("bad delete-file entry");
        }
        deleted_files.emplace_back(level, v);
        break;
      }
      default:
        return Status::Corruption("unknown version edit tag " + std::to_string(tag));
    }
  }
  return Status::OK();
}

}  // namespace docstore::kv
