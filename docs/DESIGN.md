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
| D8 | The fault model is the strict POSIX worst case: after power loss a file holds exactly its last-synced prefix, and a create/rename/delete survives only if its directory was synced. | Real ext4 is usually kinder (e.g. it often persists the directory entry with the file), so a design that passes this model also passes on real disks; the reverse is not true. |

## Deviations from the original requirements

None yet.

## Lock order

To be written.
