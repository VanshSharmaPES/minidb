#include "db.h"
#include "wal.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <map>

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
        data_[key] = value;
        return true;
    }

    std::optional<std::string> Get(const std::string& key) override {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    bool Delete(const std::string& key) override {
        if (!wal_->Append(RecordType::kDelete, key, "")) return false;
        data_.erase(key);
        return true;
    }

    std::vector<std::pair<std::string, std::string>> Scan(
        const std::string& start, const std::string& end) override {
        std::vector<std::pair<std::string, std::string>> result;
        for (auto it = data_.lower_bound(start); it != data_.end(); ++it) {
            if (!end.empty() && it->first >= end) break;
            result.push_back({it->first, it->second});
        }
        return result;
    }

    std::map<std::string, std::string>& mutable_data() { return data_; }

private:
    Options opts_;
    std::unique_ptr<WAL> wal_;
    std::map<std::string, std::string> data_;
};

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return nullptr;

    const std::string wal_path = dir + "/wal.log";

    // Replay before opening for writing, so a corrupt log fails the open
    // instead of getting appended to.
    std::map<std::string, std::string> recovered;
    bool ok = WAL::Replay(wal_path, [&](RecordType type, const std::string& key,
                                        const std::string& value) {
        if (type == RecordType::kPut) {
            recovered[key] = value;
        } else {
            recovered.erase(key);
        }
    });
    if (!ok) return nullptr;

    auto wal = WAL::Open(wal_path, opts);
    if (!wal) return nullptr;

    auto db = std::make_unique<DBImpl>(opts, std::move(wal));
    db->mutable_data() = std::move(recovered);
    return db;
}

}  // namespace minidb