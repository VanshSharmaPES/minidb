#pragma once

#include "db.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace minidb {

// The type byte is what lets replay tell Put(k, "") apart from Delete(k):
// both have a zero-length value.
enum class RecordType : uint8_t {
    kPut = 0,
    kDelete = 1,
};

// Append-only write-ahead log.
//
// On-disk record layout, in this exact order:
//   length      4 bytes   byte count of the payload (everything after the CRC)
//   crc32       4 bytes   computed over the payload only
//   type        1 byte    0 = put, 1 = delete        \
//   key_len     4 bytes                               |
//   key         key_len bytes                         |- payload
//   value_len   4 bytes   always 0 for a delete       |
//   value       value_len bytes                      /
//
// Integers are little-endian, which is what x86 writes natively.
class WAL {
public:
    static std::unique_ptr<WAL> Open(const std::string& path, const Options& opts);

    ~WAL();

    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    // Returns true only once the record is on disk to the extent the sync
    // policy promises. The caller must not touch the memtable until this
    // returns true: updating memory first would acknowledge a write that
    // does not exist on disk.
    bool Append(RecordType type, const std::string& key, const std::string& value);

    using ApplyFn = std::function<void(RecordType, const std::string&, const std::string&)>;

    // Rebuilds state from an existing log. Reader only; writes nothing.
    //
    // A truncated or CRC-failing record AT THE END of the file is the normal
    // signature of a crash. Append never returned true for it, so the write was
    // never acknowledged and discarding it breaks no promise. Replay stops
    // there and reports success.
    //
    // A bad record with valid records after it cannot come from a crash, since
    // a crash only ever truncates the tail. That is real corruption, and
    // ignoring it would silently drop writes that WERE acknowledged, so
    // Replay fails.
    //
    // Returns false only on corruption or an unreadable file. A missing file
    // is a fresh database and returns true.
    static bool Replay(const std::string& path, const ApplyFn& apply);

private:
    WAL(int fd, const Options& opts) : fd_(fd), opts_(opts) {}

    int fd_;
    Options opts_;
    size_t writes_since_sync_ = 0;
};

}  // namespace minidb