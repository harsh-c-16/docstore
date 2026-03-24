# DocStore design notes

This file records every design decision, the alternatives that were rejected, the lock order,
and every place the implementation deviates from its original requirements. It grows with the project.

## Decisions

| # | Decision | Alternatives / reason |
|---|---|---|
| D1 | Dependencies are pulled as release tarballs with a SHA-256 (`URL_HASH`) instead of `GIT_TAG`. | Same pinned versions (json v3.11.3, googletest v1.15.2); the hash pins the exact bytes and a tarball download is faster than a clone. |
| D2 | One test binary (`docstore_tests`); every gtest case is registered with ctest under the `fast` label. The `long` label re-runs the scalable tests with `DOCSTORE_LONG=1`. | A binary per component links slower and duplicates setup; one binary still lets `ctest -j` run cases in parallel. |
| D3 | Tests keep scratch files in `build/<preset>/test_tmp` (via `TEST_TMPDIR`), not `/tmp`. | `/tmp` is a tmpfs on many machines, which would hide fsync behaviour and fill RAM. |
| D4 | FaultInjectionEnv *extends* MemEnv (inherits its inode map and adds a durable namespace) rather than wrapping a MemEnv instance. | Power loss must swap inode pointers between the visible and durable namespaces, which needs MemEnv's internals; a wrapper would have to duplicate the whole file map. |
| D5 | In MemEnv/FaultInjectionEnv, directories are durable as soon as they are created; only files inside a directory follow the SyncDir rules. | The database directory is created once; the crash bugs worth catching are about files (WAL, SST, MANIFEST, CURRENT) inside it. |
| D6 | `WritableFile::Sync()` only fdatasyncs data already handed over by `Flush()`; it never touches the user-space buffer. | If Sync also flushed the buffer, the WAL syncer thread (calling Sync without the WAL mutex) would race with writers appending to that buffer. Single-threaded users call Flush() then Sync(). |
| D7 | The memtable arena is single-threaded for allocation. | Only the writer (holding the engine's write mutex) inserts; readers never allocate. MemoryUsage() is an atomic so Stats can read it from any thread. |
| D9 | The skiplist is LevelDB's design: one writer, lock-free readers, nodes never deleted, publication by release store / acquire load. It has no `Prev()`. | A mutex-protected `std::map` would make every read take a lock. A lock-free multi-writer list (CAS on insert) is unnecessary because writes are serialized anyway. Reverse iteration is not used by any query. |
| D10 | `LogReader` parses a log that was read fully into memory instead of streaming from a `SequentialFile`. | WAL files are bounded by `memtable_size` plus one batch, and MANIFESTs are tiny, so memory is not a concern; parsing a buffer makes torn-tail detection (which looks ahead) simple. |
| D11 | MANIFEST recovery accepts a bad final record only if it was cut off by end of file, its CRC does not verify at the length implied by end of file, and no valid record exists after it (`LogReader::BadRecordIsTornTail`). Anything else is `Corruption`. | A plain "stop at the first bad record" rule would turn a flipped length byte in the middle of the MANIFEST into silent data loss (later edits ignored, their WALs already deleted). LevelDB's rule (an incomplete trailing record is EOF) has the same weakness. The WAL keeps the simpler rule: a bad record in the newest WAL is a torn tail. |
| D12 | Table files are deleted by `~TableFile` once obsolete and unreferenced (RAII), so the unlink can run on whichever thread drops the last Version, including a reader. | A reference-counted "pending deletes" list processed by the background thread would keep unlinks off reader threads, at the cost of a second lifecycle mechanism. An unlink is cheap; simplicity wins. |
| D13 | `Open` always writes a fresh MANIFEST snapshot and switches CURRENT; at runtime the MANIFEST only grows by one small edit per flush/compaction and is never rolled over. | LevelDB rolls the MANIFEST when it gets large. Ours stays tiny (about 100 bytes per edit) and is compacted at every open, so rollover code would never matter in practice. |
| D14 | Flush keeps only the newest version of each key (tombstones included); compaction keeps only the newest version and drops tombstones. | Safe because there are no long-lived user snapshots: a reader pins `{mem, imm, Version, sequence}` together, and every reader that can see a new table took its snapshot after the table was installed, so it is newer than every entry in it. User-visible snapshots (future work) would need a snapshot list to keep older versions. |
| D15 | `next_file_number` is raised past every numbered file found in the directory at open, in addition to the MANIFEST value. | WAL rotation allocates numbers without writing the MANIFEST, so after a crash the MANIFEST's counter can be behind a WAL that exists on disk. |
| D16 | Two-level LSM: every compaction takes *all* of L0 plus the overlapping L1 files and writes new L1 files. | **Leveled** (L1..Ln, size ratio 10, LevelDB/RocksDB) bounds write amplification at ~10 per level and keeps space amplification near 1.1, but needs per-level score picking and file-by-file compactions. **Size-tiered** (Cassandra) has low write amplification but up to 2x space and many overlapping runs to read. Two levels is the simplest design where tombstones can be dropped (L1 is the bottom) and reads check a bounded number of files (L0 <= stop trigger, plus one L1 file). Its cost: each compaction rewrites the overlapping part of L1, so write amplification grows with database size. Leveled compaction is future work. |
| D17 | Write stalls follow LevelDB's order: (1) if L0 >= `l0_slowdown_trigger`, sleep 1 ms once per write; (2) if the memtable has room, proceed; (3) if a flush is pending or L0 >= `l0_stop_trigger`, wait; (4) otherwise rotate. | The requirements list the same three conditions without an order. Checking the stop trigger only when the memtable is full means writes keep flowing into memory while L0 is crowded; they only block when they would actually add another L0 file. |
| D18 | A long compaction polls every 256 entries and, if a memtable is waiting, flushes it inline before continuing. | With one background thread, "flush before compaction" alone would let a multi-second compaction block writers that filled the new memtable. The inline flush goes to L0, which is not a compaction input, so it cannot interfere with the merge. |
| D19 | `CompactAll()` flushes first and then compacts *every* file (L0 and all of L1). | Merging only L0 + overlapping L1 would leave L1 files untouched by the CompactionFilter; a full compaction is what reclaims a dropped collection everywhere. |
| D20 | Options are sanitized at open so that `l0_compaction_trigger <= l0_slowdown_trigger <= l0_stop_trigger` (each raised to at least the previous one). | Otherwise writers can wait at the stop trigger for a compaction that will never be scheduled (a real deadlock found by the I/O error test). RocksDB applies the same rule. |
| D21 | Any failed fsync (WAL syncer, `sync=true` write, rotation, flush, compaction, MANIFEST) sets `bg_error_`; the database becomes read-only until reopened. | After a failed fsync Linux may already have dropped the dirty pages and cleared the error, so a retry can "succeed" without the data being on disk (PostgreSQL's 2018 "fsyncgate"). Refusing further writes and recovering from the WAL on reopen is the only safe response. |
| D22 | A write that returns an error may or may not survive a crash; tests accept recovered states that include the single in-flight failed write. | The WAL append can reach the kernel (or even the disk, via the syncer) before the error is returned. This is the same contract as any database: an error means "unknown", OK means "durable". |
| D23 | `DropCollection`/`DropIndex` only remove the catalog entry (O(1)); the data is reclaimed later by compaction through `CatalogCompactionFilter`. Drops are written with `sync=true`, and the in-memory catalog changes only after the batch commits. | Deleting every key would cost O(collection size) of tombstones and block writers. Dropping by catalog is safe because ids are never reused (so the keys can never be written again, the filter contract) and because the filter only sees a drop once it is durable; otherwise compaction could durably erase data whose drop a power cut later undid. |
| D24 | Documents are stored with `nlohmann::json::to_bson`/`from_bson`, and JSON numbers keep their JSON type (int64 or double) in BSON. | Writing our own BSON encoder adds code with no benefit; BSON gives typed numbers and a length-prefixed binary layout like MongoDB's. Values BSON cannot hold (uint64 above INT64_MAX, keys containing NUL) are rejected with InvalidArgument at the API boundary. |
| D25 | The document layer has its own `Database::write_mutex_`, taken before the engine's `write_mutex_`. | Index maintenance needs read-modify-write across the old document and its index keys; the engine's mutex only covers a single `Write`. One extra uncontended lock per write is cheaper than exposing the engine mutex. |
| D26 | Indexes are **sparse**: only number, string and boolean values are indexed; missing, null, array and object values get no entry. | Arrays would need multikey indexes (one entry per element) and different matching semantics; nulls/missing would need a "null" bracket. Sparse indexes plus the matcher rule "the value must exist and be a scalar" make index and collection-scan results provably identical. |
| D27 | The planner is rule-based (eq > in > range, ties by field name), uses one index per query, and has no `_id` fast path. | MongoDB runs candidate plans in a trial ("plan racing"); with single-field indexes and this rule the choice is predictable and easy to test. An `_id` equality lookup could be a direct `Get`; it is listed as future work. |
| D28 | `CreateIndex` runs under the document write mutex, so writes block for the whole build. The "building" catalog entry is synced; the final flip to "ready" is synced. | A background build that tolerates concurrent writes (MongoDB's hybrid build with a side table) is much more complex. Syncing the "building" marker guarantees the build resumes after any crash; an unsynced marker could vanish in a power cut and leave no trace that a build was in progress. |
| D29 | `Find` takes one engine Snapshot for both the index scan and the document fetches. | With separate snapshots a concurrent update could move a document out of the scanned range after the index read and into it after the fetch, so it would be missing from both views. One snapshot makes every result a consistent point-in-time answer. |
| D30 | The TCP server uses one thread per connection (capped by `--max-conns`), blocking sockets, and newline-delimited JSON. | An epoll event loop with a worker pool scales to many more idle connections, but needs non-blocking I/O, per-connection buffers and a state machine; the database itself serializes writes anyway. Thread-per-connection is a few dozen lines that are easy to reason about; epoll is future work. JSON lines are trivial to use from any language (the Python client is ~100 lines) at the cost of parsing overhead compared to MongoDB's binary wire protocol. |
| D31 | The server blocks SIGINT/SIGTERM in all threads and handles them with `sigwait` in `main`. | A classic signal handler runs in an arbitrary thread and may only call async-signal-safe functions; `sigwait` turns the signal into an ordinary event so shutdown code (stop server, close database) can run normally. |
| D8 | The fault model is the strict POSIX worst case: after power loss a file holds exactly its last-synced prefix, and a create/rename/delete survives only if its directory was synced. | Real ext4 is usually kinder (e.g. it often persists the directory entry with the file), so a design that passes this model also passes on real disks; the reverse is not true. |

## Deviations from the original requirements

| # | Requirement | What we do | Why |
|---|---|---|---|
| X2 | a generated `_id` is "inserted as the first field". | The public API uses `nlohmann::json`, whose objects keep keys sorted, so `_id` sits wherever it sorts (after upper-case names, before lower-case ones). | The requirements also fix the API type as `nlohmann::json`; field order is not observable in queries, and switching to `ordered_json` everywhere would change every signature. |
| X1 | a torn tail in the newest WAL is truncated at the last good record. | Recovery stops at the last good record and logs a warning, but does not truncate the file in place. | Open always flushes the recovered data to L0, writes a new MANIFEST and then deletes every replayed WAL, so the torn file is never appended to or read again. If Open crashes first, the next Open replays the same prefix. Truncation would be dead code. |

## Concurrency model and lock order

**Threads.** Any number of caller threads (readers and writers), one background thread (flushes,
then compactions), and one WAL syncer thread (every `journal_commit_interval_ms`).

| Lock | Protects | Rules |
|---|---|---|
| `Engine::write_mutex_` | the whole engine write path: sequence assignment, WAL append/sync, memtable insert, memtable rotation; also `Flush`, `CompactAll`, `Close` | One writer at a time. Held across write stalls (writers queue behind it). Readers and the background threads never take it. |
| `Engine::mutex_` | `mem_`, `imm_`, `log_`, `log_number_`, the current Version pointer (swapped by `VersionSet::LogAndApply`), `bg_error_`, `manual_compaction_`, `closed_` | Held only to copy or swap pointers and flags, **never during disk I/O**. Condition variables `bg_work_cv_`, `bg_done_cv_`, `syncer_cv_` wait on it. |
| `LogWriter::mu_` | the WAL file's user-space buffer and byte count | `AddRecord` appends + flushes under it; `Sync` flushes under it and fdatasyncs **outside** it. |
| `BlockCache::Shard::mu` (x16) | one LRU shard | Leaf lock: nothing is acquired while holding it. |
| `MemEnv::mu_` -> `FileState::mu` | test filesystems only | Namespace lock before file lock. |
| `Database::write_mutex_` | document-level read-modify-write (old doc, index keys, catalog) | One document writer at a time; taken before the engine's `write_mutex_` (D25). |
| `CatalogHolder::mu_` (shared_mutex) | the `shared_ptr<const Catalog>` | Readers and the compaction filter take it shared to copy the pointer; the writer takes it exclusive to swap in a new catalog after commit. |

**Lock order:** `Database::write_mutex_` -> `CatalogHolder` shared_mutex -> `Engine::write_mutex_`
-> `Engine::mutex_` -> `LogWriter::mu_` -> cache shard. A thread never
acquires a lock earlier in this list while holding a later one. In practice the write path holds
`write_mutex_`, takes `mutex_` briefly for rotation, releases it, and only then does WAL I/O.
The catalog `shared_mutex` is only held to copy or swap one `shared_ptr<const Catalog>`, never
across an engine call, so in practice it never nests with the engine locks. The compaction filter
(background thread, no engine lock held) takes it in shared mode.

**Lock-free paths.**
- Readers: `GetSnapshot()` copies `{mem_, imm_, Version, last_visible_sequence_}` under `mutex_`
  and then reads with no lock at all. Memtables are read through the skiplist's acquire loads;
  tables are immutable files read with `pread`.
- `last_visible_sequence_`: stored with `memory_order_release` by the writer after the memtable
  insert, loaded with `memory_order_acquire` by readers. This is what makes a batch visible
  atomically.
- `shutting_down_` and `has_imm_` are atomics so the compaction loop can poll them every 256
  entries without the mutex. Statistics counters are relaxed atomics.

**Why the background threads never deadlock with writers.** Writers wait on `bg_done_cv_` for
exactly two things: a pending immutable memtable to be flushed, or L0 to drop below the stop
trigger. The background thread never takes `write_mutex_`, flushes before compacting, flushes a
waiting memtable even in the middle of a long compaction (D18), and options are sanitized so a
compaction is always scheduled before the stop trigger (D20).

**Shutdown.** `Close()` takes `write_mutex_` (waits for an in-flight write), sets `closed_` and
`shutting_down_` under `mutex_`, notifies all condition variables, joins the background and syncer
threads, then syncs the WAL. A compaction in progress sees `shutting_down_` within 256 entries,
deletes its unfinished outputs and exits; an unflushed `imm_` is simply replayed from its WAL at
the next open.
