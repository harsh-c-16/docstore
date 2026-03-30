// docstore_bench --benchmarks=a,b,c [--num=1000000] [--value_size=200] [--sync=0]
//                [--bloom_bits=10] [--cache_mb=8] [--db=/tmp/docstore_bench] [--seed=301]
//                [--reads=N]
//
// Engine workloads: fillseq fillrandom overwrite readrandom readmissing readwhilewriting scan
// Document workloads: doc_insert find_indexed find_collscan
//
// fill* and doc_insert start from an empty database; the others run on whatever the previous
// benchmark left behind (they fill sequentially first if the database is empty). Every
// operation's latency is recorded and percentiles are exact (sorted), after an unmeasured
// warm-up. Each benchmark prints one Markdown table row.

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "docstore/database.h"
#include "kv/engine.h"
#include "util/random.h"

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

struct Flags {
  std::vector<std::string> benchmarks;
  uint64_t num = 1000000;
  uint64_t reads = 0;  // 0 = num
  int value_size = 200;
  bool sync = false;
  int bloom_bits = 10;
  int cache_mb = 8;
  std::string db = "/tmp/docstore_bench";
  uint64_t seed = 301;
};

std::string Key(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "key%013llu", static_cast<unsigned long long>(i));
  return buf;  // 16 bytes, so key + 200-byte value makes documents of about 200 bytes
}

uint64_t DirBytes(const std::string& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec)) total += e.file_size(ec);
  }
  return total;
}

double Mib(uint64_t bytes) { return static_cast<double>(bytes) / (1 << 20); }

struct Result {
  uint64_t ops = 0;
  double seconds = 0;
  uint64_t bytes = 0;  // payload bytes moved, for MB/s
  std::vector<uint64_t> latencies_ns;
  std::string notes;
};

uint64_t Percentile(std::vector<uint64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
  return sorted[std::min(sorted.size() - 1, rank == 0 ? 0 : rank - 1)];
}

void Print(const std::string& name, Result r) {
  std::sort(r.latencies_ns.begin(), r.latencies_ns.end());
  auto us = [&](double p) {
    if (r.latencies_ns.empty()) return std::string("-");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", Percentile(r.latencies_ns, p) / 1000.0);
    return std::string(buf);
  };
  char mbs[32] = "-";
  if (r.bytes > 0) std::snprintf(mbs, sizeof(mbs), "%.1f", Mib(r.bytes) / r.seconds);
  std::printf("| %s | %llu | %.0f | %s | %s | %s | %s | %s |\n", name.c_str(),
              static_cast<unsigned long long>(r.ops), r.ops / r.seconds, mbs, us(0.50).c_str(),
              us(0.99).c_str(), us(0.999).c_str(), r.notes.c_str());
  std::fflush(stdout);
}

class Bench {
 public:
  explicit Bench(Flags f) : flags_(std::move(f)), rnd_(flags_.seed) {
    value_.resize(flags_.value_size);
    for (auto& c : value_) c = static_cast<char>('a' + rnd_.Uniform(26));
  }

  docstore::Options EngineOptions() const {
    docstore::Options o;
    o.bloom_bits_per_key = flags_.bloom_bits;
    o.block_cache_bytes = static_cast<size_t>(flags_.cache_mb) << 20;
    return o;
  }

  void Fresh() {
    engine_.reset();
    db_.reset();
    std::filesystem::remove_all(flags_.db);
  }

  docstore::kv::Engine* Engine() {
    if (!engine_) {
      db_.reset();
      docstore::Status s = docstore::kv::Engine::Open(EngineOptions(), flags_.db, &engine_);
      if (!s.ok()) Die(s);
    }
    return engine_.get();
  }

  docstore::Database* Db() {
    if (!db_) {
      engine_.reset();
      docstore::Status s = docstore::Database::Open(EngineOptions(), flags_.db, &db_);
      if (!s.ok()) Die(s);
    }
    return db_.get();
  }

  static void Die(const docstore::Status& s) {
    std::fprintf(stderr, "error: %s\n", s.ToString().c_str());
    std::exit(1);
  }

  // Writes n keys (sequential or uniformly random) and measures each write.
  Result Write(uint64_t n, bool random, bool measure) {
    Result r;
    if (measure) r.latencies_ns.reserve(n);
    docstore::WriteOptions wo{.sync = flags_.sync};
    auto* e = Engine();
    auto start = Clock::now();
    for (uint64_t i = 0; i < n; i++) {
      std::string k = Key(random ? rnd_.Uniform(flags_.num) : i);
      auto t0 = Clock::now();
      docstore::Status s = e->Put(wo, k, value_);
      if (!s.ok()) Die(s);
      if (measure) r.latencies_ns.push_back((Clock::now() - t0).count());
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.ops = n;
    r.bytes = n * (16 + value_.size());
    return r;
  }

  void WarmUpWrites() {
    // A throwaway database, so the measured fill starts from empty with warm caches/CPU.
    std::string real = flags_.db;
    flags_.db = real + "_warmup";
    std::filesystem::remove_all(flags_.db);
    Write(std::min<uint64_t>(flags_.num / 10, 50000), true, false);
    engine_.reset();
    std::filesystem::remove_all(flags_.db);
    flags_.db = real;
  }

  void EnsureFilled() {
    auto* e = Engine();
    if (e->Stats()["last_sequence"].get<uint64_t>() == 0) Write(flags_.num, false, false);
    docstore::Status s = e->Flush();  // reads hit tables, not the memtable
    if (!s.ok()) Die(s);
  }

  std::string Layout() {
    json st = Engine()->Stats();
    return "L0=" + std::to_string(st["l0_files"].get<int>()) +
           " L1=" + std::to_string(st["l1_files"].get<int>()) + " files";
  }

  // n point reads; missing=true reads keys that sort between existing ones.
  Result Read(uint64_t n, bool missing) {
    auto* e = Engine();
    std::string v;
    for (uint64_t i = 0; i < std::min<uint64_t>(n / 10, 100000); i++) {  // warm-up
      (void)e->Get(Key(rnd_.Uniform(flags_.num)) + (missing ? "." : ""), &v);
    }
    json before = e->Stats();
    Result r;
    r.latencies_ns.reserve(n);
    uint64_t found = 0;
    auto start = Clock::now();
    for (uint64_t i = 0; i < n; i++) {
      std::string k = Key(rnd_.Uniform(flags_.num)) + (missing ? "." : "");
      auto t0 = Clock::now();
      docstore::Status s = e->Get(k, &v);
      r.latencies_ns.push_back((Clock::now() - t0).count());
      if (s.ok())
        found++;
      else if (!s.IsNotFound())
        Die(s);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.ops = n;
    r.bytes = found * (16 + value_.size());
    json after = e->Stats();
    auto delta = [&](const char* f) {
      return after[f].get<uint64_t>() - before[f].get<uint64_t>();
    };
    std::ostringstream notes;
    notes << "found " << found << "/" << n << "; " << Layout() << "; bloom checks "
          << delta("bloom_checks") << ", bloom-avoided reads " << delta("bloom_useful")
          << "; cache hits " << delta("block_cache_hits") << ", misses "
          << delta("block_cache_misses") << "; bloom_bits=" << flags_.bloom_bits;
    r.notes = notes.str();
    return r;
  }

  Result ReadWhileWriting(uint64_t n) {
    auto* e = Engine();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes{0};
    std::thread writer([&] {
      docstore::Random wr(flags_.seed + 1);
      while (!stop) {
        docstore::Status s = e->Put(docstore::WriteOptions(), Key(wr.Uniform(flags_.num)), value_);
        if (!s.ok()) Die(s);
        writes++;
      }
    });
    Result r = Read(n, false);
    stop = true;
    writer.join();
    r.notes = std::to_string(writes.load()) + " concurrent writes by 1 writer; " + r.notes;
    return r;
  }

  Result Scan() {
    auto* e = Engine();
    {
      auto warm = e->NewIterator({});
      for (int i = 0; warm->Valid() && i < 100000; i++) warm->Next();
    }
    Result r;
    auto start = Clock::now();
    auto it = e->NewIterator({});
    for (; it->Valid(); it->Next()) {
      r.ops++;
      r.bytes += it->key().size() + it->value().size();
    }
    if (!it->status().ok()) Die(it->status());
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.notes = "full forward scan; " + Layout();
    return r;
  }

  Result Overwrite() {
    EnsureFilled();
    Result r = Write(flags_.num, true, true);
    auto* e = Engine();
    docstore::Status s = e->Flush();
    if (!s.ok()) Die(s);
    json st = e->Stats();
    uint64_t disk_before = DirBytes(flags_.db);
    auto c0 = Clock::now();
    s = e->CompactAll();
    if (!s.ok()) Die(s);
    double compact_s = std::chrono::duration<double>(Clock::now() - c0).count();
    uint64_t disk_after = DirBytes(flags_.db);
    char notes[512];
    std::snprintf(
        notes, sizeof(notes),
        "write amplification %.2f (fill + overwrite: SST bytes flushed %.0f MiB + "
        "compacted %.0f MiB / user bytes %.0f MiB); %d compactions; stall %.2f s; "
        "disk %.1f MiB before CompactAll -> %.1f MiB after (%.1f s)",
        st["write_amplification"].get<double>(), Mib(st["flush_bytes_written"].get<uint64_t>()),
        Mib(st["compaction_bytes_written"].get<uint64_t>()),
        Mib(st["user_bytes_written"].get<uint64_t>()), st["compactions"].get<int>(),
        st["stall_micros"].get<uint64_t>() / 1e6, Mib(disk_before), Mib(disk_after), compact_s);
    r.notes = notes;
    return r;
  }

  // ---- document layer
  json RandomDoc(uint64_t i) {
    char id[32];
    std::snprintf(id, sizeof(id), "user%08llu", static_cast<unsigned long long>(i));
    json d{{"_id", id},
           {"age", rnd_.Uniform(100)},
           {"zip", rnd_.Uniform(10000)},  // high cardinality: ~num/10000 docs per value
           {"city", "city" + std::to_string(rnd_.Uniform(100))},
           {"name", value_.substr(0, 20)}};
    d["pad"] = value_.substr(0, 110);  // about 200 bytes of BSON per document
    return d;
  }

  Result DocInsert() {
    Fresh();
    auto* db = Db();
    for (uint64_t i = 0; i < std::min<uint64_t>(flags_.num / 10, 20000); i++) {  // warm-up
      docstore::Status s = db->Insert("warmup", RandomDoc(i), {}, nullptr);
      if (!s.ok()) Die(s);
    }
    Result r;
    r.latencies_ns.reserve(flags_.num);
    auto start = Clock::now();
    for (uint64_t i = 0; i < flags_.num; i++) {
      json d = RandomDoc(i);
      auto t0 = Clock::now();
      docstore::Status s = db->Insert("users", std::move(d), {.sync = flags_.sync}, nullptr);
      r.latencies_ns.push_back((Clock::now() - t0).count());
      if (!s.ok()) Die(s);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.ops = flags_.num;
    r.notes = "1 document per insert, ~200 B BSON each";
    return r;
  }

  Result Find(bool indexed) {
    auto* db = Db();
    if (indexed) {
      auto c0 = Clock::now();
      docstore::Status s = db->CreateIndex("users", "zip");
      if (!s.ok()) Die(s);
      build_seconds_ = std::chrono::duration<double>(Clock::now() - c0).count();
    } else {
      (void)db->DropIndex("users", "zip");
    }
    const uint64_t queries = indexed ? 1000 : 5;
    Result r;
    json explain;
    std::vector<json> out;
    (void)db->Find("users", json{{"zip", 4242}}, {}, &out, &explain);  // warm-up
    auto start = Clock::now();
    for (uint64_t q = 0; q < queries; q++) {
      json filter{{"zip", static_cast<int>((4242 + q * 7919) % 10000)}};
      auto t0 = Clock::now();
      docstore::Status s = db->Find("users", filter, {}, &out, &explain);
      r.latencies_ns.push_back((Clock::now() - t0).count());
      if (!s.ok()) Die(s);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.ops = queries;
    const json& es = explain["executionStats"];
    std::ostringstream notes;
    notes << "filter {zip: v} over " << flags_.num << " docs; plan "
          << (indexed ? "IXSCAN zip_1" : "COLLSCAN") << "; last query nReturned " << es["nReturned"]
          << ", totalKeysExamined " << es["totalKeysExamined"] << ", totalDocsExamined "
          << es["totalDocsExamined"];
    if (indexed) notes << "; index build " << build_seconds_ << " s";
    r.notes = notes.str();
    return r;
  }

  void Run() {
    std::printf("| benchmark | ops | ops/s | MB/s | p50 us | p99 us | p99.9 us | notes |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    const uint64_t reads = flags_.reads ? flags_.reads : flags_.num;
    for (const auto& b : flags_.benchmarks) {
      Result r;
      if (b == "fillseq" || b == "fillrandom") {
        WarmUpWrites();
        Fresh();
        r = Write(flags_.num, b == "fillrandom", true);
        r.notes = std::string("sync=") + (flags_.sync ? "1" : "0") + "; " + Layout();
      } else if (b == "overwrite") {
        r = Overwrite();
      } else if (b == "readrandom" || b == "readmissing") {
        EnsureFilled();
        r = Read(reads, b == "readmissing");
      } else if (b == "readwhilewriting") {
        EnsureFilled();
        r = ReadWhileWriting(reads);
      } else if (b == "scan") {
        EnsureFilled();
        r = Scan();
      } else if (b == "doc_insert") {
        r = DocInsert();
      } else if (b == "find_indexed" || b == "find_collscan") {
        r = Find(b == "find_indexed");
      } else {
        std::fprintf(stderr, "unknown benchmark %s\n", b.c_str());
        std::exit(2);
      }
      Print(b, std::move(r));
    }
  }

 private:
  Flags flags_;
  docstore::Random rnd_;
  std::string value_;
  std::unique_ptr<docstore::kv::Engine> engine_;
  std::unique_ptr<docstore::Database> db_;
  double build_seconds_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Flags f;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&](const char* name) -> const char* {
      size_t n = std::strlen(name);
      return a.compare(0, n, name) == 0 ? a.c_str() + n : nullptr;
    };
    if (const char* v = value("--benchmarks=")) {
      std::stringstream ss(v);
      for (std::string b; std::getline(ss, b, ',');) f.benchmarks.push_back(b);
    } else if (const char* v = value("--num="))
      f.num = std::strtoull(v, nullptr, 10);
    else if (const char* v = value("--reads="))
      f.reads = std::strtoull(v, nullptr, 10);
    else if (const char* v = value("--value_size="))
      f.value_size = std::atoi(v);
    else if (const char* v = value("--sync="))
      f.sync = std::atoi(v) != 0;
    else if (const char* v = value("--bloom_bits="))
      f.bloom_bits = std::atoi(v);
    else if (const char* v = value("--cache_mb="))
      f.cache_mb = std::atoi(v);
    else if (const char* v = value("--db="))
      f.db = v;
    else if (const char* v = value("--seed="))
      f.seed = std::strtoull(v, nullptr, 10);
    else {
      std::fprintf(stderr, "unknown flag %s\n", a.c_str());
      return 2;
    }
  }
  if (f.benchmarks.empty()) f.benchmarks = {"fillseq", "fillrandom", "readrandom", "readmissing"};
  Bench(f).Run();
  return 0;
}
