# minidb

An LSM-tree key-value storage engine written in C++ from scratch.

It is not a SQL database. There is no query language, no transactions, no network server, and no secondary indexes. The API is four operations over byte strings.

## Status

M1 and M2 are complete: durable writes with crash-safe recovery (write-ahead log plus an in-memory skip list), and flushing the memtable to immutable on-disk SSTables once it crosses a size threshold, with reads and scans merging across the memtable and all flushed files.

Still to come are compaction (M3) and benchmarks against SQLite (M4).

## API

    static std::unique_ptr<DB> Open(const std::string& dir, const Options& opts);
    bool Put(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key);
    bool Delete(const std::string& key);
    std::vector<std::pair<std::string, std::string>> Scan(start, end);

`Get` returns an optional rather than a bool with an out-parameter, because an empty string is a valid stored value. With an out-parameter you cannot tell "the key holds an empty value" apart from "the key is not there," since the parameter is left empty either way. The optional carries both pieces of information in one return.

`Open` is a static factory rather than a constructor for a similar reason. Opening a database can fail: the directory may not exist, or the log may be corrupt. A constructor has no way to report that without throwing, so `Open` returns a null pointer instead.

`Scan` uses a half-open range, with `start` included and `end` excluded. An empty `end` means scan through to the last key, since byte-string keys have no natural maximum to compare against.

`Delete` returns true once the tombstone is recorded, not whether the key was there beforehand. Answering the second question would mean searching the memtable and every on-disk file before every delete, and that is read work on the write path that callers rarely want to pay for.

## Durability contract

`Put` returning true means the record is in the write-ahead log. What that is worth depends on the sync policy.

With `sync_every_write` set to true, every record is fsynced before the call returns. So true means the write survives a power cut happening immediately afterwards.

With it set to false, the record has reached the OS page cache but not necessarily the disk. It survives the process being killed, because the kernel still holds the data and will flush it on its own schedule. A power cut is different: the page cache is volatile, so up to `sync_interval - 1` acknowledged writes can be lost. It is one less than the interval because the write that triggers the sync makes everything before it durable.

The log is always written before the memtable is touched. If you updated memory first and crashed in between, you would have told the caller a write succeeded when nothing on disk records it.

## Crash recovery

A crash can only truncate the tail of an append-only file, never the middle. Recovery leans on that.

Damage at the end of the file is forgiven. A record cut short, or a length field that was only half written, both mean `Append` never finished, which means `Put` never returned true and the caller was never told anything. Dropping those bytes breaks no promise. And because both cases are unacknowledged writes, replay can treat them identically instead of trying to distinguish a real length from garbage.

Damage in the middle is different. If a bad record has valid records after it, a crash cannot explain it, because a crash would have taken everything after it too. That is real corruption, and ignoring it would silently discard writes that were acknowledged. So replay fails the open rather than continuing.

## WAL record format

| Field | Size | Notes |
|---|---|---|
| length | 4 bytes | byte count of the payload |
| crc32 | 4 bytes | over the payload only |
| type | 1 byte | 0 = put, 1 = delete |
| key_len | 4 bytes | |
| key | variable | |
| value_len | 4 bytes | 0 for a delete |
| value | variable | absent for a delete |

Integers are little-endian. The type byte exists because `Put(k, "")` and `Delete(k)` are otherwise identical on disk: both have a zero-length value.

## Flushing and SSTable format

Once the memtable's approximate size crosses `flush_threshold_bytes`, `Put`/`Delete` trigger a flush: the memtable's sorted contents are written to a new immutable SSTable, the file is renamed into place atomically, the memtable is cleared, and the WAL is reopened fresh since it now only needs to cover writes since the flush.

The WAL is only reset after the SSTable write is fsynced. If a crash lands between those two steps, the next `Open` replays the same WAL records into a new memtable — a harmless duplicate of what the SSTable already has, since `Put`/`Delete` are idempotent upserts, not data loss.

`Get` and `Scan` check the memtable first, then SSTables newest-to-oldest, so a later flush's value or tombstone always shadows an earlier one for the same key. Tombstones are kept in SSTables rather than dropped, for the same reason: discarding one early would let an older file's stale value for that key reappear. They are only safe to drop during compaction (M3), once every older file has been merged past them.

An SSTable file has three parts, in this order:

| Block | Contents |
|---|---|
| data | Sorted records: `type` (1 byte), `key_len`/`key`, `value_len`/`value` (0-length for a delete) |
| index | Sparse index, one entry per `kIndexInterval` records: `key_len`/`key`, 8-byte byte offset into the data block |
| footer | Fixed 24 bytes at end of file: `index_offset` (8), `index_count` (4), `data_crc32` (4) over the whole data block, `magic` (8) |

The footer is fixed-size and always the last 24 bytes, so `Open` seeks straight to it instead of scanning, then reads just the index it points to; the data block stays on disk and is read on demand. Integers are little-endian, matching the WAL. A missing, truncated, or CRC/magic-mismatched file fails `Open` outright — unlike the WAL, an SSTable has no tail to forgive, since it is written once and fsynced before anything ever opens it.

## Testing

The oracle test runs 500,000 randomized operations against both the engine and a `std::map` reference model, checking that the results match after every single operation rather than only at the end. Keys are drawn from a space of 1,000, which is deliberate: with random unique keys the overwrite and delete paths would almost never be exercised. The seed is fixed so a failure can be reproduced exactly.

The crash harness forks a writer, kills it with SIGKILL at a random point in the first 50 milliseconds, reopens the database, and checks that every acknowledged write is still there. The child writes each acknowledged key to a separate fsynced file first, so the test is comparing the database against a record of what it actually promised, rather than against what the test assumed it promised.

Over 1,000 iterations that came to 4,101,035 acknowledged writes with none lost.

Both suites were checked by breaking the implementation on purpose. Removing the WAL append from `Put` fails the crash test on the first iteration. A test that has never been seen to fail is not yet known to work.

### What these tests do not prove

SIGKILL kills the process, not the kernel. Anything already handed to the page cache survives regardless of whether fsync was ever called, so this harness proves that replay handles a torn log correctly. It does not prove that fsync is doing its job. A build with fsync stripped out entirely still passes it.

The timing measurements point the same way. Under WSL2 an fsync here costs about 4.4 microseconds, and turning fsync off for 999 out of every 1,000 writes only improved throughput from 227,807 to 261,017 writes per second, a ratio of 1.15. Real NVMe fsync costs somewhere between 50 and 200 microseconds. Numbers that fast, and a batching speedup that small, both suggest the sync is being absorbed by the virtual disk layer rather than reaching physical storage.

So the honest scope is: durability across process termination is verified. Durability across power loss is not, and verifying it would need hardware that honors fsync and a machine that can be hard-reset.

## Build

Needs a C++17 compiler, CMake 3.14 or newer, and zlib. GoogleTest is fetched during configure.

    sudo apt install -y build-essential cmake zlib1g-dev

    cmake -S . -B build
    cmake --build build -j4
    cd build && ctest --output-on-failure

The crash test can be run directly with a different iteration count:

    ./build/crash_test 1000

## Layout

    include/db.h        public API and the durability contract
    src/wal.{h,cpp}     write-ahead log: append, fsync policy, replay
    src/skiplist.h      memtable
    src/sstable.{h,cpp} immutable on-disk sorted file: write, open, get, scan
    src/db_impl.cpp     ties the WAL, memtable, and SSTables together; flush
    tests/              oracle test, WAL/SSTable unit tests, flush tests, crash harness

## Design notes

The memtable is a skip list rather than a balanced tree. Inserts never rotate or rebalance anything, they just splice a node into a few linked lists, which suits a structure taking a constant stream of writes. Level 0 links every node in sorted order, so `Scan` is a plain walk along it. This is also what LevelDB and RocksDB use for the same reason.

The overall design is an LSM tree rather than a B-tree, mostly because of how it fails. A B-tree is not useful until page splits are correct, so progress is invisible until it suddenly works. An LSM tree can be built in stages that each do something on their own: a durable log, then sorted files, then compaction. Being able to stop at a working intermediate point mattered more here than picking the more conventional structure.