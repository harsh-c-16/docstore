#include "kv/dbformat.h"

#include "util/coding.h"

namespace docstore::kv {

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                       ValueType type) {
  dst->append(user_key);
  PutFixed64(dst, PackSequenceAndType(seq, type));
}

bool ParseInternalKey(std::string_view ikey, ParsedInternalKey* out) {
  if (ikey.size() < kInternalKeyTrailer) return false;
  uint64_t tag = DecodeFixed64(ikey.data() + ikey.size() - kInternalKeyTrailer);
  uint8_t type = tag & 0xFF;
  if (type > static_cast<uint8_t>(ValueType::kValue)) return false;
  out->user_key = ExtractUserKey(ikey);
  out->sequence = tag >> 8;
  out->type = static_cast<ValueType>(type);
  return true;
}

int CompareInternalKeys(std::string_view a, std::string_view b) {
  int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r;
  uint64_t ta = DecodeFixed64(a.data() + a.size() - kInternalKeyTrailer);
  uint64_t tb = DecodeFixed64(b.data() + b.size() - kInternalKeyTrailer);
  // Larger tag (newer sequence) sorts first.
  if (ta > tb) return -1;
  if (ta < tb) return 1;
  return 0;
}

std::string MakeLookupKey(std::string_view user_key, SequenceNumber snapshot) {
  std::string key;
  // kValue is the larger type, so this is the largest tag a visible entry can have.
  AppendInternalKey(&key, user_key, snapshot, ValueType::kValue);
  return key;
}

}  // namespace docstore::kv
