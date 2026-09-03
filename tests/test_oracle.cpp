#include <gtest/gtest.h>
#include "db.h"
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

std::string MakeKey(int n) {
    return "key" + std::to_string(n);
}

// Every 7th value is empty on purpose. Empty values are legal in your design
// and the WAL will have to tell them apart from deletes later.
std::string MakeValue(int n) {
    if (n % 7 == 0) return "";
    return "val" + std::to_string(n);
}

std::vector<std::pair<std::string, std::string>> ExpectedScan(
    const std::map<std::string, std::string>& model,
    const std::string& start, const std::string& end) {
    std::vector<std::pair<std::string, std::string>> expected;
    for (auto it = model.lower_bound(start); it != model.end(); ++it) {
        if (!end.empty() && it->first >= end) break;
        expected.push_back({it->first, it->second});
    }
    return expected;
}

void RunOracle(int iterations) {
    (void)system("rm -rf /tmp/minidb_oracle");
    minidb::Options opts;
    opts.sync_every_write = false;
    opts.sync_interval = 1000;
    auto db = minidb::DB::Open("/tmp/minidb_oracle", opts);
    ASSERT_NE(db, nullptr);

    std::map<std::string, std::string> model;
    std::mt19937 rng(42);

    const int kKeySpace = 1000;

    for (int i = 0; i < iterations; ++i) {
        int op = rng() % 100;
        int k = rng() % kKeySpace;
        std::string key = MakeKey(k);

        if (op < 40) {
            std::string value = MakeValue(rng() % kKeySpace);
            EXPECT_TRUE(db->Put(key, value)) << "Put failed at iteration " << i;
            model[key] = value;
        } else if (op < 80) {
            auto got = db->Get(key);
            auto it = model.find(key);
            if (it == model.end()) {
                EXPECT_FALSE(got.has_value())
                    << "Get returned a value for a missing key at iteration " << i
                    << " key=" << key;
            } else {
                ASSERT_TRUE(got.has_value())
                    << "Get returned nullopt for an existing key at iteration " << i
                    << " key=" << key;
                EXPECT_EQ(*got, it->second)
                    << "Get returned the wrong value at iteration " << i
                    << " key=" << key;
            }
        } else if (op < 95) {
            EXPECT_TRUE(db->Delete(key)) << "Delete failed at iteration " << i;
            model.erase(key);
        } else {
            std::string a = MakeKey(rng() % kKeySpace);
            std::string b = MakeKey(rng() % kKeySpace);
            if (b < a) std::swap(a, b);
            auto got = db->Scan(a, b);
            auto expected = ExpectedScan(model, a, b);
            EXPECT_EQ(got, expected)
                << "Scan mismatch at iteration " << i
                << " range [" << a << ", " << b << ")";
        }
    }

    auto full = db->Scan("", "");
    std::vector<std::pair<std::string, std::string>> expected_full(model.begin(), model.end());
    EXPECT_EQ(full, expected_full) << "Full scan did not match the model";
    EXPECT_EQ(full.size(), model.size());
}

}  // namespace

TEST(Oracle, MatchesStdMapSmall) {
    RunOracle(1000);
}

TEST(Oracle, MatchesStdMapLarge) {
    RunOracle(500000);
}

TEST(Oracle, EmptyValueIsDistinctFromMissingKey) {
    minidb::Options opts;
    (void)system("rm -rf /tmp/minidb_empty");
    auto db = minidb::DB::Open("/tmp/minidb_empty", opts);
    ASSERT_NE(db, nullptr);

    EXPECT_FALSE(db->Get("absent").has_value());

    ASSERT_TRUE(db->Put("present", ""));
    auto got = db->Get("present");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "");

    ASSERT_TRUE(db->Delete("present"));
    EXPECT_FALSE(db->Get("present").has_value());
}

TEST(Oracle, DeleteOnMissingKeyReturnsTrue) {
    minidb::Options opts;
    (void)system("rm -rf /tmp/minidb_del");
    auto db = minidb::DB::Open("/tmp/minidb_del", opts);
    ASSERT_NE(db, nullptr);
    EXPECT_TRUE(db->Delete("never_existed"));
}

TEST(Oracle, ScanIsHalfOpen) {
    minidb::Options opts;
    (void)system("rm -rf /tmp/minidb_scan");
    auto db = minidb::DB::Open("/tmp/minidb_scan", opts);
    ASSERT_NE(db, nullptr);

    ASSERT_TRUE(db->Put("a", "1"));
    ASSERT_TRUE(db->Put("b", "2"));
    ASSERT_TRUE(db->Put("c", "3"));

    auto range = db->Scan("a", "c");
    ASSERT_EQ(range.size(), 2u);
    EXPECT_EQ(range[0].first, "a");
    EXPECT_EQ(range[1].first, "b");

    auto to_end = db->Scan("b", "");
    ASSERT_EQ(to_end.size(), 2u);
    EXPECT_EQ(to_end[0].first, "b");
    EXPECT_EQ(to_end[1].first, "c");
}
