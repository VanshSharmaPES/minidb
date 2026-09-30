#include "db.h"
#include "wal.h"
#include "skiplist.h"
#include "sstable.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <map>

namespace minidb {

namespace {

// Zero-padded so lexicographic and numeric directory order agree: the
// filename alone tells you flush order without opening anything.
std::string SSTableFileName(uint64_t seq) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%06llu.sst", static_cast<unsigned long long>(seq));
    return buf;
}

// Existing *.sst files in `dir`, oldest (lowest sequence number) first.
// Anything that doesn't parse as NNNNNN.sst is ignored: SSTable::Write always
// writes to a .tmp path and renames into place atomically, so a crash mid-
// flush leaves at most a stray .tmp file here, never a half-named .sst one.
std::vector<uint64_t> ListSSTableSeqs(const std::string& dir) {
    std::vector<uint64_t> seqs;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return seqs;
    while (dirent* entry = ::readdir(d)) {
        std::string name = entry->d_name;
        if (name.size() != 10 || name.substr(6) != ".sst") continue;
        if (name.find_first_not_of("0123456789", 0) != 6) continue;
        seqs.push_back(static_cast<uint64_t>(std::stoull(name.substr(0, 6))));
    }
    ::closedir(d);
    std::sort(seqs.begin(), seqs.end());
    return seqs;
}

}  // namespace

namespace {

class DBImpl : public DB {
public:
    DBImpl(const std::string& dir, const Options& opts, std::unique_ptr<WAL> wal)
        : dir_(dir), opts_(opts), wal_(std::move(wal)) {}

    ~DBImpl() override = default;

    // WAL first, memtable second. Reversing this would acknowledge a write
    // that a crash could erase.
    bool Put(const std::string& key, const std::string& value) override {
        if (!wal_->Append(RecordType::kPut, key, value)) return false;
        data_.Insert(key, value);
        MaybeFlush();
        return true;
    }

    std::optional<std::string> Get(const std::string& key) override {
        const SkipList::Node* n = data_.Find(key);
        if (n != nullptr) return n->is_delete ? std::nullopt : std::make_optional(n->value);

        // Not in the memtable: check SSTables newest first, since a later
        // flush's value or tombstone must shadow an earlier one for the
        // same key.
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            auto entry = (*it)->Get(key);
            if (entry.has_value()) {
                return entry->is_delete ? std::nullopt : std::make_optional(entry->value);
            }
        }
        return std::nullopt;
    }

    bool Delete(const std::string& key) override {
        if (!wal_->Append(RecordType::kDelete, key, "")) return false;
        data_.Erase(key);
        MaybeFlush();
        return true;
    }

    std::vector<std::pair<std::string, std::string>> Scan(
        const std::string& start, const std::string& end) override {
        // Merge oldest-to-newest so a later source's entry for a key always
        // overwrites an earlier one; the memtable is applied last since it is
        // always the newest data. A std::map is a simple correct choice here
        // and keeps results sorted for free -- Scan has no throughput
        // requirement yet to justify anything cleverer.
        std::map<std::string, std::pair<std::string, bool>> merged;
        for (auto& sst : sstables_) {
            for (auto& e : sst->Scan(start, end)) {
                merged[e.key] = {e.value, e.is_delete};
            }
        }
        for (const SkipList::Node* n = data_.LowerBound(start); n != nullptr;
             n = SkipList::Next(n)) {
            if (!end.empty() && n->key >= end) break;
            merged[n->key] = {n->value, n->is_delete};
        }

        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(merged.size());
        for (auto& [key, v] : merged) {
            if (!v.second) result.push_back({key, v.first});
        }
        return result;
    }

    SkipList& mutable_data() { return data_; }

    bool LoadSSTables() {
        for (uint64_t seq : ListSSTableSeqs(dir_)) {
            auto sst = SSTable::Open(dir_ + "/" + SSTableFileName(seq));
            if (!sst) return false;
            sstables_.push_back(std::move(sst));
            next_seq_ = std::max(next_seq_, seq + 1);
        }
        return true;
    }

private:
    // Writes the memtable out as a new immutable SSTable once it crosses
    // opts_.flush_threshold_bytes, then clears the memtable and starts a
    // fresh WAL. The WAL is only reset after the SSTable is fully durable
    // (SSTable::Write fsyncs before returning): if a crash lands between
    // those two steps, the next Open() replays the same records into a
    // fresh memtable, which is a harmless duplicate of what's already in the
    // SSTable, not data loss -- Put/Delete are idempotent upserts.
    void MaybeFlush() {
        if (opts_.flush_threshold_bytes == 0) return;
        if (data_.ApproxBytes() < opts_.flush_threshold_bytes) return;
        Flush();
    }

    bool Flush() {
        if (data_.size() == 0) return true;

        std::vector<SSTable::Entry> entries;
        entries.reserve(data_.size());
        for (const SkipList::Node* n = data_.LowerBound(""); n != nullptr;
             n = SkipList::Next(n)) {
            entries.push_back({n->key, n->value, n->is_delete});
        }

        std::string final_path = dir_ + "/" + SSTableFileName(next_seq_);
        std::string tmp_path = final_path + ".tmp";
        if (!SSTable::Write(tmp_path, entries)) return false;
        if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) return false;

        auto sst = SSTable::Open(final_path);
        if (!sst) return false;
        sstables_.push_back(std::move(sst));
        ++next_seq_;

        data_.Clear();

        // The WAL now only needs to cover writes since this flush. Reopening
        // it fresh (rather than truncating in place) matches how WAL::Open
        // always creates-if-missing, and avoids reasoning about truncate
        // interacting with an already-open fd.
        wal_.reset();
        std::string wal_path = dir_ + "/wal.log";
        if (::unlink(wal_path.c_str()) != 0 && errno != ENOENT) return false;
        wal_ = WAL::Open(wal_path, opts_);
        return wal_ != nullptr;
    }

    std::string dir_;
    Options opts_;
    std::unique_ptr<WAL> wal_;
    SkipList data_;
    std::vector<std::unique_ptr<SSTable>> sstables_;  // oldest first
    uint64_t next_seq_ = 0;
};

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return nullptr;

    const std::string wal_path = dir + "/wal.log";

    auto wal = WAL::Open(wal_path, opts);
    if (!wal) return nullptr;

    auto db = std::make_unique<DBImpl>(dir, opts, std::move(wal));

    if (!db->LoadSSTables()) return nullptr;

    // Replay directly into the memtable. SkipList is non-copyable, so there is
    // no temporary map to move in afterwards. Only writes since the last
    // flush are in the WAL at this point, since Flush resets it.
    bool ok = WAL::Replay(wal_path, [&](RecordType type, const std::string& key,
                                        const std::string& value) {
        if (type == RecordType::kPut) {
            db->mutable_data().Insert(key, value);
        } else {
            db->mutable_data().Erase(key);
        }
    });
    if (!ok) return nullptr;

    return db;
}

}  // namespace minidb
