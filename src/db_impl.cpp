#include "db.h"

#include <map>

namespace minidb {

namespace {

class DBImpl : public DB {
public:
    explicit DBImpl(const Options& opts) : opts_(opts) {}
    ~DBImpl() override = default;

    bool Put(const std::string& key, const std::string& value) override {
        data_[key] = value;
        return true;
    }

    std::optional<std::string> Get(const std::string& key) override {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    bool Delete(const std::string& key) override {
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

private:
    Options opts_;
    std::map<std::string, std::string> data_;
};

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
    (void)dir;
    return std::make_unique<DBImpl>(opts);
}

}  // namespace minidb
