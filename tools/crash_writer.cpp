// crash_writer <db_path> [max_ops]
//
// Performs the numbered ops of crash_ops.h and prints each op number after the database has
// acknowledged it (stdout flushed every time). The process-crash test SIGKILLs it at a random
// moment and then checks that every printed op survived.

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "crash_ops.h"
#include "kv/engine.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <db_path> [max_ops]\n", argv[0]);
    return 2;
  }
  const uint64_t max_ops = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : UINT64_MAX;
  docstore::Options options;
  // Small memtables and files so flushes and compactions happen during the run.
  options.memtable_size = 64 << 10;
  options.target_file_size = 256 << 10;
  std::unique_ptr<docstore::kv::Engine> engine;
  docstore::Status s = docstore::kv::Engine::Open(options, argv[1], &engine);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  for (uint64_t i = 0; i < max_ops; i++) {
    docstore::crash::CrashOp op = docstore::crash::OpAt(i);
    docstore::WriteOptions wo{.sync = op.sync};
    s = op.value.empty() ? engine->Delete(wo, op.key) : engine->Put(wo, op.key, op.value);
    if (!s.ok()) {
      std::fprintf(stderr, "op %llu failed: %s\n", static_cast<unsigned long long>(i),
                   s.ToString().c_str());
      return 1;
    }
    std::printf("%llu\n", static_cast<unsigned long long>(i));
    std::fflush(stdout);  // one write() per line: the test sees exactly the acknowledged ops
  }
  return 0;
}
