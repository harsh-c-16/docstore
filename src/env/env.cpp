#include "env/env.h"

namespace docstore {

Status ReadFileToString(Env* env, const std::string& path, std::string* out) {
  out->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(path, &file);
  if (!s.ok()) return s;
  constexpr size_t kChunk = 1 << 20;
  std::string chunk;
  while (true) {
    s = file->Read(kChunk, &chunk);
    if (!s.ok()) return s;
    out->append(chunk);
    if (chunk.size() < kChunk) return Status::OK();
  }
}

Status WriteStringToFileSync(Env* env, std::string_view data, const std::string& path) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(path, &file);
  if (s.ok()) s = file->Append(data);
  if (s.ok()) s = file->Flush();
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  return s;
}

}  // namespace docstore
