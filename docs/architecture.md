# DocStore architecture

## Components

```mermaid
flowchart TD
  subgraph tools["tools / clients"]
    CLI["docstore_cli"]
    SRV["docstore_server<br/>(TcpServer, thread per connection)"]
    PY["Python client"]
    BENCH["docstore_bench"]
  end
  CH["CommandHandler<br/>JSON request -> JSON response"]
  subgraph doc["Document layer (src/doc)"]
    DB["Database<br/>catalog, write_mutex_"]
    IDX["index_manager<br/>maintenance, build, resume"]
    Q["matcher -> planner -> executor<br/>explain"]
    BSON["bson_codec, key_codec, objectid"]
  end
  subgraph kv["Storage engine (src/kv)"]
    ENG["Engine<br/>write path, snapshots, stats"]
    WAL["WAL<br/>LogWriter / LogReader"]
    MEM["MemTable<br/>arena + skiplist"]
    VS["VersionSet<br/>MANIFEST, CURRENT, Versions"]
    SST["SSTables<br/>TableBuilder / TableReader,<br/>bloom filter, block cache"]
    BG["background thread<br/>flush + compaction"]
    SYNC["syncer thread"]
  end
  ENV["Env<br/>PosixEnv | MemEnv | FaultInjectionEnv"]
  PY -- "newline JSON over TCP" --> SRV
  CLI --> CH
  SRV --> CH
  CH --> DB
  BENCH --> DB
  BENCH --> ENG
  DB --> IDX
  DB --> Q
  DB --> BSON
  DB -- "WriteBatch / Snapshot" --> ENG
  ENG --> WAL
  ENG --> MEM
  ENG --> VS
  BG --> SST
  BG --> VS
  SYNC --> WAL
  VS --> SST
  WAL --> ENV
  SST --> ENV
  VS --> ENV
```

## Write path

```mermaid
sequenceDiagram
  participant C as caller
  participant D as Database
  participant E as Engine (write_mutex_)
  participant W as WAL
  participant M as MemTable
  participant B as background thread
  C->>D: Update(coll, id, {$set: ...})
  D->>D: lock Database::write_mutex_
  D->>E: Get(old doc) (no lock: snapshot read)
  D->>D: apply operators, validate, compute index changes
  D->>E: Write(batch = doc key + index keys [+ catalog])
  E->>E: lock write_mutex_, MakeRoomForWrite (stall / rotate if full)
  E->>W: AddRecord(batch) = append + write()  (survives process crash)
  opt sync=true
    E->>W: Sync() = fdatasync              (survives power loss)
  end
  E->>M: insert every record (seq, seq+1, ...)
  E->>E: last_visible_sequence_.store(release)  (batch becomes visible atomically)
  E-->>D: OK
  D->>D: install new catalog (if any), unlock
  D-->>C: OK
  Note over E,B: when the memtable is full: sync old WAL, new WAL + dir sync,<br/>imm_ = mem_, wake the background thread
```

## Read path

```mermaid
flowchart LR
  G["Get / Find"] --> S["GetSnapshot():<br/>copy mem_, imm_, Version,<br/>last_visible_sequence_<br/>under mutex_"]
  S --> M1["memtable"]
  M1 -- "not found" --> M2["immutable memtable"]
  M2 -- "not found" --> L0["L0 tables, newest first<br/>(range check, bloom, index, 1 block)"]
  L0 -- "not found" --> L1["the one L1 table whose range<br/>holds the key (binary search,<br/>bloom, index, 1 block)"]
  L1 -- "not found" --> NF["NotFound"]
  M1 -- "value / tombstone" --> R["result"]
  M2 -- "value / tombstone" --> R
  L0 -- "value / tombstone" --> R
  L1 -- "value / tombstone" --> R
```

Scans merge the same sources with a `MergingIterator` (min-heap) under a `DBIterator` that hides
entries newer than the snapshot, keeps the newest version per key, skips tombstones and applies
the `[lower, upper)` bounds. `Find` runs its whole plan (index scan + document fetches) on one
snapshot.

## Flush

```mermaid
sequenceDiagram
  participant B as background thread
  participant F as Env (files)
  participant V as VersionSet
  B->>B: imm = imm_, log_number = current WAL number
  B->>F: write L0 table (newest entry per key, tombstones kept)
  B->>F: fdatasync table, close
  B->>F: fsync directory (the table's name is durable)
  B->>V: LogAndApply({add L0 file, log_number}): append + fsync MANIFEST
  V->>V: swap in new Version (under mutex_)
  B->>B: imm_ = null, wake stalled writers
  B->>F: delete WALs with number < log_number
```

## Compaction (L0 -> L1)

```mermaid
flowchart TD
  T["L0 files >= l0_compaction_trigger<br/>or CompactAll()"] --> P["PickCompaction: all L0 files +<br/>every overlapping L1 file<br/>(all files for CompactAll)"]
  P --> MG["MergingIterator over the inputs"]
  MG --> K{"for each entry"}
  K -- "older version of a key already seen" --> DROP["drop"]
  K -- "tombstone (L1 is the bottom)" --> DROP
  K -- "CompactionFilter says dead<br/>(dropped collection / index)" --> DROP
  K -- "newest live value" --> OUT["add to output; cut a new L1<br/>file at target_file_size"]
  K -. "every 256 entries" .-> Y{"memtable waiting?<br/>shutting down?"}
  Y -- "flush it inline (to L0)" --> K
  Y -- "shutdown: delete outputs, stop" --> STOP["abort"]
  OUT --> SY["fdatasync outputs, fsync directory"]
  SY --> LA["LogAndApply({delete inputs, add outputs})"]
  LA --> OB["mark inputs obsolete<br/>(deleted when the last Version<br/>holding them is released)"]
```

## Recovery (`Engine::Open`, then `Database::Open`)

```mermaid
sequenceDiagram
  participant O as Open
  participant F as Env
  participant V as VersionSet
  O->>F: CreateDir, LockFile(LOCK)
  alt CURRENT missing
    O->>O: new database (if create_if_missing)
  else CURRENT exists
    O->>V: Recover: read CURRENT -> MANIFEST, replay edits,<br/>open every table (footer + size checks)
  end
  O->>F: list files, raise next_file_number past every number on disk
  loop every WAL with number >= log_number, oldest first
    O->>O: replay records into a memtable (torn tail in the newest WAL: stop there)
    O->>F: flush to L0 whenever the memtable fills
  end
  O->>F: flush the rest to L0
  O->>F: create new WAL, fsync directory
  O->>V: WriteSnapshot: new MANIFEST (full state), CURRENT.tmp, fsync, rename, fsync directory
  O->>F: only now delete old WALs, old MANIFESTs, orphan tables, *.tmp
  O->>O: start background + syncer threads
  O->>O: Database: load catalog, rebuild any index left in state "building"
```

## Table file lifecycle

```mermaid
stateDiagram-v2
  [*] --> Writing: flush / compaction creates NNNNNN.sst
  Writing --> Orphan: crash before its MANIFEST edit is synced
  Orphan --> [*]: deleted at the next Open
  Writing --> Live: synced + dir synced + LogAndApply(add) durable
  Live --> Obsolete: LogAndApply(delete) durable (compaction input)
  Obsolete --> Obsolete: old Versions (scans, snapshots) still reference it
  Obsolete --> [*]: last Version released -> ~TableFile unlinks it
```
