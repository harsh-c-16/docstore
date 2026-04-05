# DocStore

DocStore is a single-node document database, a tiny MongoDB, written from scratch in C++20 on
its own LSM-tree storage engine. JSON documents live in collections and are stored as BSON;
writes go through a CRC-checked write-ahead log into a lock-free skiplist memtable, are flushed
to SSTables with bloom filters, and are compacted from L0 into L1 in the background while reads
run against consistent snapshots. On top sit secondary indexes with an order-preserving key
encoding, a query planner with MongoDB-shaped `explain`, a CLI, a TCP server and a Python
client. Durability is not assumed but tested: a fault-injection filesystem cuts the power inside
flushes, compactions and recovery, and a harness `SIGKILL`s a writer and checks that no
acknowledged write is ever lost.

## Features

- **Documents**: collections (implicit or explicit), `Insert` (ObjectId `_id` if missing),
  `Upsert`, `Update` with `$set` / `$unset` / `$inc`, `Delete`, `Get`; MongoDB's document rules
  (16 MiB BSON, 100 levels, no `$`-prefixed or dotted field names).
- **Queries**: `$eq $gt $gte $lt $lte $in` on dotted field paths, ANDed; `limit`; explain with
  `COLLSCAN` / `IXSCAN` / `FETCH`, `indexBounds`, `totalKeysExamined`, `totalDocsExamined`.
- **Secondary indexes**: sparse single-field indexes, built from existing data, maintained in the
  same atomic batch as the document, resumed after a crash.
- **Storage engine**: WAL fdatasync'd per write when `sync=true` (MongoDB `j:true`), otherwise by a
  100 ms background syncer; skiplist memtable; SSTables with 4 KiB blocks, CRC32C everywhere, per-file
  bloom filters, a 16-shard LRU block cache; MANIFEST/CURRENT versioning; L0 -> L1 compaction
  with tombstone dropping, a compaction filter and write stalls; snapshot iterators.
- **Safety**: torn-write and corruption detection, read-only mode after any I/O error, a LOCK
  file against double opens, crash recovery that never deletes anything before the new state is
  durable.
- **Interfaces**: C++ library (`include/docstore/database.h`), `docstore_cli`, `docstore_server`
  (newline-delimited JSON over TCP), `clients/python/docstore_client.py`.
- **Tested**: unit, randomized model, concurrency stress, power-loss, process-crash, corruption,
  I/O-error and integration tests; clean under ASan + UBSan and TSan.

## Quick start

Requirements: Linux x86-64, GCC >= 11 or Clang >= 14, CMake >= 3.20, Ninja, Python 3 for the
demo. Dependencies (nlohmann/json 3.11.3, GoogleTest 1.15.2) are fetched by CMake.

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release -L fast          # fast suite
ctest --preset release -L long          # long suite (crash/power-loss/stress)
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan -L fast
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan -L fast
scripts/run_all_tests.sh                # all of the above + a benchmark smoke run, with a summary
```

### CLI session

```text
$ ./build/release/docstore_cli ./data
DocStore shell on ./data. Type 'help'.
docstore> insert users {"_id": "u1", "name": "Asha", "age": 21, "city": "Pune"}
docstore> insert users {"name": "Ben", "age": 34, "city": "Goa"}
docstore> index users age
docstore> find users {"age": {"$gte": 18, "$lt": 30}}
{
  "docs": [
    {
      "_id": "u1",
      "age": 21,
      "city": "Pune",
      "name": "Asha"
    }
  ],
  "ok": 1
}
docstore> explain users {"age": 21}
{
  "explain": {
    "executionStats": {"executionTimeMillis": 0, "nReturned": 1, "totalDocsExamined": 1, "totalKeysExamined": 1},
    "queryPlanner": {
      "winningPlan": {
        "filter": {"age": 21},
        "inputStage": {
          "indexBounds": {"age": ["[21, 21]"]},
          "indexName": "age_1",
          "keyPattern": {"age": 1},
          "stage": "IXSCAN"
        },
        "stage": "FETCH"
      }
    }
  },
  "ok": 1
}
docstore> update users u1 {"$inc": {"age": 1}}
docstore> stats
```

Each command prints the JSON response (`{"ok": 1, ...}` or `{"ok": 0, "code": ..., "errmsg": ...}`;
the `explain` above is shown with its inner objects joined onto single lines).
Raw requests work too: `{"op": "get", "coll": "users", "id": "u1"}`. `--exec "<command>"` runs a
single command and exits non-zero if it failed.

### Server and Python demo

```bash
./build/release/docstore_server --db ./data --port 7070      # Ctrl-C shuts down gracefully
python3 clients/python/demo.py 127.0.0.1 7070
```

Output of `demo.py` (5,000 random users, run on the benchmark machine):

```text
query: {'age': {'$gte': 30, '$lt': 32}, 'city': 'Pune'}  (5000 documents)

                     without index                            with index on age
plan                 COLLSCAN                                 FETCH <- IXSCAN age_1 {'age': ['[30, 32)']}
nReturned            22                                       22
totalKeysExamined    0                                        149
totalDocsExamined    5000                                     149

indexes on demo_users: ['age']
```

## Durability guarantees

- When a write returns OK, it has survived a **process crash** (it is in the OS page cache
  through the WAL).
- When a `sync=true` write returns OK, it also survives **power loss / OS crash**, because the WAL
  has been fdatasync'd (MongoDB `j:true`).
- `sync=false` writes are made durable by the syncer thread within `journal_commit_interval_ms`
  (default 100 ms, MongoDB's default commit interval).
- **Prefix property**: after any crash, the recovered state equals the state after some prefix of
  the acknowledged writes, and that prefix includes every `sync=true` write. There are never
  partial batches, and a later write is never present while an earlier one is missing.

A write that returns an error may or may not have survived (DESIGN.md D22).

## Architecture

```
            ┌──────────────── tools ────────────────┐
            │ docstore_cli   docstore_server   bench │
            └───────────────┬────────────────────────┘
                            │ JSON requests
                   ┌────────▼─────────┐
                   │  CommandHandler  │   one code path for CLI + server
                   └────────┬─────────┘
                   ┌────────▼─────────┐
   Document layer  │ Database         │  collections, catalog, BSON, ObjectId,
   (src/doc)       │ IndexManager     │  index maintenance, query matcher,
                   │ Planner/Executor │  planner, explain
                   └────────┬─────────┘
                            │ WriteBatch / Get / Iterator over byte keys
                   ┌────────▼─────────┐
   Storage engine  │ kv::Engine       │  WAL, memtable (skiplist), SSTables,
   (src/kv)        │                  │  bloom filters, block cache, versions,
                   │                  │  MANIFEST, flush, compaction, iterators
                   └────────┬─────────┘
                   ┌────────▼─────────┐
                   │ Env (filesystem) │  PosixEnv | MemEnv | FaultInjectionEnv
                   └──────────────────┘
```

Mermaid diagrams of the write path, read path, flush, compaction, recovery and file lifecycle are
in [docs/architecture.md](docs/architecture.md); byte layouts of every file are in
[docs/formats.md](docs/formats.md); design decisions, the lock order and deviations from the original requirements
are in [docs/DESIGN.md](docs/DESIGN.md).

## Benchmarks

Measured on this machine (from [bench/results/2026-11-09.md](bench/results/2026-11-09.md), which
has the full notes for every row: file layout, bloom and cache counters):

| | |
|---|---|
| CPU | AMD Ryzen 7 7840HS w/ Radeon 780M Graphics |
| Cores / threads | 8 cores, 16 threads |
| RAM | 14Gi total, 7.4Gi available at start |
| Disk | KINGSTON SNV2S250G (SSD/NVMe), ext4 on /dev/nvme1n1p4 |
| OS | Linux 7.0.0-42-generic |
| Compiler | c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0, CMake Release (-O3 -DNDEBUG), -std=c++20 |

Workloads use 1,000,000 keys/documents, 16-byte keys and 200-byte values, a 4 MiB memtable and an
8 MiB block cache; latencies are exact percentiles over every operation.

| benchmark | ops/s | MB/s | p50 µs | p99 µs | p99.9 µs | what |
|---|---|---|---|---|---|---|
| fillseq | 308533 | 63.6 | 2.22 | 6.27 | 10.66 | sequential keys, sync=0 |
| fillrandom | 119556 | 24.6 | 2.36 | 7.14 | 1070.57 | random keys, sync=0 |
| fillrandom (sync=1) | 640 | 0.1 | 1532.94 | 1891.28 | 2350.14 | random keys, fdatasync per write (num=10,000) |
| overwrite | 48824 | 10.1 | 2.38 | 1066.61 | 1079.55 | random overwrites of 1M existing keys |
| readrandom | 102154 | 21.0 | 9.86 | 14.11 | 17.90 | keys present; 8 MiB cache, 963,158 block-cache misses |
| readmissing | 3511899 | - | 0.15 | 0.41 | 10.47 | absent keys, bloom 10 bits/key |
| readmissing (bloom off) | 104510 | - | 9.68 | 13.83 | 15.92 | absent keys, bloom filters disabled |
| readwhilewriting | 86732 | 17.9 | 11.10 | 21.93 | 30.85 | 1 reader + 1 background writer |
| scan | 1584488 | 326.4 | - | - | - | full forward scan |
| doc_insert | 211009 | - | 3.44 | 7.84 | 12.79 | 1 document (~200 B BSON) per insert |
| find_collscan | 1 | - | 1628738.51 | 1634668.37 | 1634668.37 | `{zip: v}`, COLLSCAN, 1,000,000 docs examined |
| find_indexed | 836 | - | 1189.51 | 1523.45 | 1620.77 | `{zip: v}`, IXSCAN zip_1, 93 keys / 93 docs examined |

- **Bloom filters**: absent-key reads are 33.6x faster with filters (3,511,899 vs 104,510 reads/s); 991,654 of 999,960 filter checks skipped the disk read (0.83% false positives, theory 0.82%).
- **Indexes**: the indexed query examined 93 documents instead of 1,000,000 (10,753x fewer), 1.19 ms vs 1.63 s median.
- **Compaction**: write amplification 9.05 over fill + overwrite; on-disk size 262.0 MiB -> 219.8 MiB after `CompactAll`; 17.18 s of write stalls in the overwrite run (the cost of two-level compaction, see Limitations).

Reproduce with `scripts/run_benchmarks.sh` (writes `bench/results/<date>.md`), or individually, e.g.
`./build/release/docstore_bench --benchmarks=fillrandom,readrandom,readmissing --num=1000000 --db=<dir on a real disk>`
(the default `--db` is `/tmp/docstore_bench`, which is a RAM-backed tmpfs on many Linux machines).

## Tests

103 GoogleTest cases in one binary (`docstore_tests`), registered with ctest under the label
`fast`; the label `long` re-runs the scalable ones with more iterations (`DOCSTORE_LONG=1`).

| Kind | Where | What it proves |
|---|---|---|
| Unit | `tests/unit/` | every component: coding, CRC32C and xxHash64 against reference vectors, arena, Env + fault injection, skiplist (incl. 1 writer / 4 readers), memtable, WriteBatch, WAL (torn tail, mid-file corruption, oversized length), SSTable (100K keys, corruption), bloom filter (no false negatives, measured FP 0.83% vs 0.82% theory), LRU cache, key codec ordering, matcher, planner, update operators, ObjectId |
| Engine model | `tests/model/engine_model_test.cpp` | random Put/Delete/batch/Get/Scan/Flush/CompactAll/Reopen vs `std::map` with a 4 KiB memtable: 20 seeds x 10K ops (fast), 100 x 50K (long) |
| Document model | `tests/model/doc_model_test.cpp` | random document ops, index creates/drops and reopens; every query runs on a database with indexes and one without and is compared with an independent oracle |
| Concurrency stress | `tests/stress/stress_test.cpp` | 1 writer of atomic 1,000-key batches, 8 scanning readers, flushes and compactions throughout: every scan sees one whole batch, versions never go backwards |
| Power loss | `tests/crash/power_loss_test.cpp` | random mixed-sync workloads, power cut at random points including inside flush, compaction and recovery; the recovered state must be a prefix including every acknowledged `sync=true` write |
| Process crash | `tests/crash/process_crash_test.cpp` | `crash_writer` is `SIGKILL`ed after 50-2000 ms; every acknowledged op must survive, as an exact prefix |
| Corruption | `tests/crash/corruption_test.cpp` | random byte flips in WAL / SST / MANIFEST / CURRENT: always `Corruption` or a dropped WAL tail, never a wrong value |
| I/O errors | `tests/crash/io_error_test.cpp` | failing appends/syncs during flush, compaction and WAL writes: writes become `ReadOnly`, reads keep working, reopen recovers |
| Integration | `tests/integration/` | CommandHandler end to end, CLI `--exec`, TCP server with 16 concurrent clients x 1,000 verified ops, connection limit, oversized lines, graceful stop |

Results of the final run of `scripts/run_all_tests.sh` (2026-11-09, GCC 15.2):

| Suite | Result |
|---|---|
| fast, release | 103/103 passed (9 s) |
| fast, ASan + UBSan | 103/103 passed (74 s) |
| fast, TSan | 103/103 passed (52 s) |
| long, release (295 s) | **power loss: 500 iterations, 44,050 ops, 366 cuts inside flush/compaction, 0 violations**; **process crash: 200 SIGKILL runs, 3,996,074 acknowledged ops, 0 lost**; stress (30 s): 92,574 batches, 464,881 scans, 465,321 gets, 46,286 flushes, 11,571 compactions, 0 inconsistent reads; document model: 119,822 queries compared with and without indexes (24,582 used one), 0 mismatches; engine model: 100 seeds x 50K ops passed |

The power-loss test was checked to fail when any of four fsyncs is removed (directory sync after a
new WAL, directory sync after a new table, old-WAL sync at rotation, the `sync=true` fdatasync),
the crash test fails if WAL records stay in the user-space buffer, and the document model test
fails if the planner makes `$gte` exclusive or index inserts are skipped.

## Limitations

- One writer at a time; a `sync=true` write costs one fdatasync (no group commit), measured at
  640 writes/s on the benchmark machine.
- Two-level compaction rewrites the overlapping part of L1 on every compaction, so write
  amplification grows with database size (9.05 in the overwrite benchmark) and writes stall when
  compaction falls behind.
- Single-field, sparse indexes only: no compound or multikey (array) indexes, no index-backed sort,
  no `_id` fast path in the planner; the planner is rule-based, not cost-based.
- Index builds block writes for their whole duration.
- Numbers compare as doubles, so integers above 2^53 lose precision in comparisons and indexes.
- `nlohmann::json` keeps object keys sorted, so a generated `_id` is not necessarily the first
  field (DESIGN.md X2).
- No compression, no replication, no sharding, no transactions across documents, no
  authentication (the server binds to localhost by default), Linux only.
- Clang builds are configured in CI but were not run on the development machine (GCC 15 only).

## Future work

Group commit; leveled compaction; prefix compression with restart points; compound indexes and
index-backed sort; an oplog that tails the WAL to a read-only follower; an epoll server; user-visible
snapshots and optimistic multi-document transactions.
