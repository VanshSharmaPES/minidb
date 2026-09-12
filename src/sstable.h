#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

// Immutable, sorted, on-disk representation of a memtable flush.
//
// An SSTable is written once, in full, by Flush, and never modified again.
// That's what makes the read path simple: no locking against concurrent
// writers, and the sparse index can be built in one pass because keys arrive
// already sorted (the memtable iterates in order).
//
// A tombstone (Delete) is written like a Put with a type byte, not omitted:
// dropping it here would let an older SSTable's value for the same key
// reappear during a Get/Scan merge. Tombstones are only safe to discard
// during compaction (M3), once every older file has been merged past them.
//
// On-disk layout, in this exact order:
//
//   [data block]
//     A sorted sequence of records, one per key:
//       type       1 byte    0 = put, 1 = delete
//       key_len    4 bytes
//       key        key_len bytes
//       value_len  4 bytes   always 0 for a delete
//       value      value_len bytes
//
//   [index block]
//     A sparse index: one entry every kIndexInterval records, not every
//     record. Dense would cost one index entry per key for no benefit,
//     since a block scan from the nearest earlier entry is already O(1)
//     amortized for the block sizes this engine targets.
//       key_len    4 bytes
//       key        key_len bytes
//       offset     8 bytes   byte offset of this record within the data block
//     Repeated index_count times.
//
//   [footer]      fixed 24 bytes, always the last 24 bytes of the file
//     index_offset  8 bytes   byte offset where the index block starts
//     index_count   4 bytes   number of index entries
//     data_crc32    4 bytes   CRC32 over the entire data block
//     magic         8 bytes   fixed constant, sanity check that this is an
//                             SSTable and not truncated garbage
//
// The footer is fixed-size and at a fixed position (end of file), so Open
// can find it without scanning: seek to size - 24, read it, then read
// exactly the index block it points to. The data block is never read at
// open time; data_crc32 is only checked at open as a corruption guard.
//
// Integers are little-endian, matching wal.h.
class SSTable {
public:
    // Every record needed to reconstruct one flush, in the order the
    // memtable produced them (already sorted by key).
    struct Entry {
        std::string key;
        std::string value;
        bool is_delete;
    };

    // Writes a new, immutable SSTable file from a fully-sorted entry list.
    // Returns false on any I/O failure; a partially-written file on failure
    // is not fsynced and Open will refuse it as truncated garbage.
    static bool Write(const std::string& path, const std::vector<Entry>& entries);

    // Opens an existing SSTable for reads. Loads the footer and index into
    // memory; the data block stays on disk and is read on demand.
    // Returns nullptr if the file is missing, truncated, or the CRC/magic
    // checks fail -- the same "loudly fail on non-tail corruption" policy
    // as WAL::Replay, since an SSTable has no tail to forgive: it is written
    // once and fsynced before Open ever sees it.
    static std::unique_ptr<SSTable> Open(const std::string& path);

    ~SSTable();

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;

    // nullopt: key not present in this file. Present-with-value: still needs
    // the delete/put distinction one level up, at the DB merge, because a
    // tombstone found here must stop the search rather than fall through to
    // an older file -- so this returns the raw entry, not std::optional<value>.
    std::optional<Entry> Get(const std::string& key) const;

    // Every entry in the file with start <= key < end, in sorted order.
    // Same half-open convention as DB::Scan.
    std::vector<Entry> Scan(const std::string& start, const std::string& end) const;

private:
    struct IndexEntry {
        std::string key;
        uint64_t offset;
    };

    SSTable(int fd, std::vector<IndexEntry> index, uint64_t data_end)
        : fd_(fd), index_(std::move(index)), data_end_(data_end) {}

    int fd_;
    std::vector<IndexEntry> index_;
    uint64_t data_end_;
};

}  // namespace minidb
