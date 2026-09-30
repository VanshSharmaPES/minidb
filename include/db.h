#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <utility>
#include <cstddef>

namespace minidb {

// Durability policy. This is a tradeoff knob, not a correctness switch:
// both modes are correct, they promise different things about crash recovery.
struct Options {
    // true:  every Put fsyncs before returning. Slowest, strongest guarantee.
    // false: fsync every sync_interval writes. Faster, bounded data loss.
    bool sync_every_write = true;

    // Only consulted when sync_every_write is false.
    // A power cut can lose at most (sync_interval - 1) acknowledged writes,
    // since the write that triggers the sync makes everything before it durable.
    size_t sync_interval = 100;

    // Approximate memtable size, in key+value bytes, that triggers a flush
    // to an immutable on-disk SSTable. Approximate because the running total
    // is updated per-write, not recomputed exactly; good enough for a size
    // trigger. 0 disables flushing, keeping the whole database in memory
    // (and in the WAL) as M1 did.
    size_t flush_threshold_bytes = 4 * 1024 * 1024;
};

class DB {
public:
    // Static factory rather than a constructor because opening a database can
    // fail: the directory may not exist, the WAL may be corrupt mid-file.
    // A constructor cannot signal failure without throwing, so Open returns
    // a null pointer instead. It also performs file and resource setup that
    // must succeed before a usable DB object exists.
    static std::unique_ptr<DB> Open(const std::string& dir, const Options& opts);

    virtual ~DB();

    // Returns true once the record is in the write-ahead log.
    //
    // With sync_every_write = true, true means the write survives a power cut
    // taking effect immediately after this call returns.
    //
    // With sync_every_write = false, true means the record reached the OS page
    // cache. It survives the process being killed, since the kernel still holds
    // and will flush the data, but a power cut can lose up to
    // (sync_interval - 1) acknowledged writes because the page cache is volatile.
    virtual bool Put(const std::string& key, const std::string& value) = 0;

    // Returns an optional because the key may not exist, and a missing key must
    // be distinguishable from a key whose stored value is the empty string.
    // A bool-plus-out-parameter signature cannot express that difference:
    // the out parameter is left empty in both cases.
    //
    // An empty string is a valid value. Put(k, "") followed by Get(k) returns
    // an engaged optional holding "", not nullopt. This is why WAL records
    // carry a type byte: a zero-length value and a delete are otherwise
    // identical on disk.
    //
    // Returns by value rather than by reference so callers never hold a pointer
    // into internal storage. Costs a copy or move, buys a safe lifetime contract.
    virtual std::optional<std::string> Get(const std::string& key) = 0;

    // Returns true when the tombstone is durably recorded, NOT whether the key
    // existed beforehand. Reporting prior existence would require a full lookup
    // through the memtable and every on-disk file, and callers rarely want to
    // pay that on the write path.
    //
    // After Delete, Get for that key returns nullopt.
    virtual bool Delete(const std::string& key) = 0;

    // Half-open range: start is inclusive, end is exclusive.
    // An empty end string means scan from start through the last key, since
    // byte-string keys have no natural maximum value to compare against.
    virtual std::vector<std::pair<std::string, std::string>> Scan(
        const std::string& start, const std::string& end) = 0;

    // A DB owns file descriptors. Copying would mean two objects closing the
    // same fd.
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

protected:
    DB() = default;
};

}  // namespace minidb
