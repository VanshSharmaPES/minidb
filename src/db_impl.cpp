#include "db.h"
#include "wal.h"
#include "skiplist.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>

namespace minidb {

namespace {

class DBImpl : public DB {
public:
    DBImpl(const Options& opts, std::unique_ptr<WAL> wal)
        : opts_(opts), wal_(std::move(wal)) {}

    ~DBImpl() override = default;

    // WAL first, memtable second. Reversing this would acknowledge a write
    // that a crash could erase.
    bool Put(const std::string& key, const std::string& value) override {
        if (!wal_->Append(RecordType::kPut, key, value)) return false;
        data_.Insert(key, value);
        return true;
    }

    std::optional<std::string> Get(const std::string& key) override {
        const SkipList::Node* n = data_.Find(key);
        if (n == nullptr) return std::nullopt;
        return n->value;
    }

    bool Delete(const std::string& key) override {
        if (!wal_->Append(RecordType::kDelete, key, "")) return false;
        data_.Erase(key);
        return true;
    }

    std::vector<std::pair<std::string, std::string>> Scan(
        const std::string& start, const std::string& end) override {
        std::vector<std::pair<std::string, std::string>> result;
        for (const SkipList::Node* n = data_.LowerBound(start); n != nullptr;
             n = SkipList::Next(n)) {
            if (!end.empty() && n->key >= end) break;
            result.push_back({n->key, n->value});
        }
        return result;
    }

    SkipList& mutable_data() { return data_; }

private:
    Options opts_;
    std::unique_ptr<WAL> wal_;
    SkipList data_;
};

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return nullptr;

    const std::string wal_path = dir + "/wal.log";

    auto wal = WAL::Open(wal_path, opts);
    if (!wal) return nullptr;

    auto db = std::make_unique<DBImpl>(opts, std::move(wal));

    // Replay directly into the memtable. SkipList is non-copyable, so there is
    // no temporary map to move in afterwards.
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