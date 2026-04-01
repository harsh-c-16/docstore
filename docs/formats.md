# DocStore on-disk formats

All integers are little-endian unless marked **BE** (big-endian). `varint` is the usual base-128
encoding (7 bits per byte, high bit = "more bytes follow"); `lp(x)` means `varint len(x) | x`.
Every checksum is CRC32C (Castagnoli, polynomial `0x82F63B78`, standard pre/post inversion).
Code: `src/util/coding.h`, `src/util/crc32c.h`.

## Files in a database directory

| Name | Contents |
|---|---|
| `NNNNNN.log` | write-ahead log (record log of WriteBatches) |
| `NNNNNN.sst` | SSTable |
| `MANIFEST-NNNNNN` | record log of VersionEdits |
| `CURRENT` | the live MANIFEST's file name plus `\n`, e.g. `MANIFEST-000012\n` |
| `CURRENT.tmp` | written, synced and renamed over `CURRENT` to switch manifests |
| `LOCK` | empty; held with `flock(LOCK_EX)` while the database is open |

All `NNNNNN` numbers come from one counter and are never reused (zero-padded to at least 6 digits).

## Internal key (`src/kv/dbformat.h`)

```
user_key bytes | fixed64( (sequence << 8) | type )
type: 0 = deletion (tombstone), 1 = value; sequence: 56 bits
```
Order: user key ascending (bytewise), then the 8-byte tag **descending** (newest first).

## Record log: WAL and MANIFEST (`src/kv/log_writer.h`)

```
+----------------+----------------+---------------------+
| crc32c  u32    | length  u32    | payload (length B)  |   repeated until end of file
+----------------+----------------+---------------------+
crc32c covers the 4 length bytes followed by the payload.
length > 64 MiB is treated as corruption.
```
A WAL payload is one WriteBatch; a MANIFEST payload is one VersionEdit.

## WriteBatch (`src/kv/write_batch.h`)

```
sequence u64 | count u32 | record * count
record (put)    = 0x01 | lp(key) | lp(value)
record (delete) = 0x00 | lp(key)
```
Record `i` gets sequence `sequence + i`.

## Memtable entry (in the arena, `src/kv/memtable.cpp`)

```
varint ikey_len | internal key | varint value_len | value
```

## SSTable (`src/kv/table_format.h`)

```
[data block | crc32c u32] ... [filter block | crc32c u32] [index block | crc32c u32] [footer 48 B]
```

**Data block** (~`block_size` = 4 KiB of entries, sorted by internal key, no restart points):
```
entry = varint klen | varint vlen | internal key (klen B) | value (vlen B)
```

**Filter block**: the bloom filter over the file's user keys (empty if `bloom_bits_per_key = 0`):
```
bit array (ceil(max(64, n * bits_per_key) / 8) bytes) | k u8
probe i (0 <= i < k) = (h + i * rotr(h, 17)) mod (8 * bit_array_bytes), h = xxHash64(user_key, seed 0xb100f11e)
```

**Index block**: one entry per data block, in order:
```
lp(last internal key of the block) | offset u64 | size u32      (size excludes the block's CRC)
```

**Footer** (last 48 bytes of the file):
```
offset  0: index_offset   u64
offset  8: index_size     u64   (without CRC)
offset 16: filter_offset  u64
offset 24: filter_size    u64   (without CRC)
offset 32: format_version u32   (= 1)
offset 36: crc32c         u32   of bytes 0..35
offset 40: magic          u64   = 0x444F4353544F5245 ("DOCSTORE" read as big-endian ASCII)
```
`TableReader::Open` checks: magic, footer CRC, version, that filter and index are contiguous and
end at the footer, the filter and index CRCs, and that the index entries tile the data region.

## VersionEdit (MANIFEST payload, `src/kv/version_edit.h`)

A sequence of tagged fields (`tag` is a varint):

| Tag | Field | Encoding |
|---|---|---|
| 1 | log_number | varint64: WALs below this number are obsolete |
| 2 | next_file_number | varint64 |
| 3 | last_sequence | varint64 |
| 4 | add_file | varint32 level, varint64 number, varint64 file_size, lp(smallest internal key), lp(largest internal key) |
| 5 | delete_file | varint32 level, varint64 number |

The first record of every MANIFEST is a full snapshot (log_number, next_file_number,
last_sequence and an add_file for every live table); later records are deltas from flushes and
compactions.

## Document keyspace (`src/doc/keys.h`)

| Key | Value |
|---|---|
| `0x00` `"catalog"` | catalog JSON (below) |
| `0x01` coll_id (u32 **BE**) `_id` bytes | the document as BSON (`nlohmann::json::to_bson`) |
| `0x02` coll_id (u32 **BE**) index_id (u32 **BE**) `enc(value)` `_id` bytes | empty |

Catalog JSON:
```json
{"next_coll_id": 3, "next_index_id": 5,
 "collections": [{"name": "users", "id": 1,
   "indexes": [{"field": "age", "id": 2, "state": "ready"}]}]}
```
`state` is `"ready"` or `"building"`.

## Index value encoding `enc(v)` (`src/doc/key_codec.h`)

| Type | Bytes |
|---|---|
| number (compared as double; NaN not indexed) | `0x10` then 8 bytes **BE** of: `bits = bit_cast<u64>(d)` (with `-0.0` normalized to `0.0`); if the sign bit is set `bits = ~bits`, else `bits ^= 0x8000000000000000` |
| string | `0x20`, the bytes with every `0x00` written as `0x00 0xFF`, then `0x00 0x01` |
| boolean | `0x30` then `0x00` (false) or `0x01` (true) |

For two values of the same type, `enc(a) < enc(b)` bytewise exactly when `a < b`; across types,
numbers < strings < booleans. No encoding is a prefix of another.
