#include <gtest/gtest.h>
#include "db.h"

#include <dirent.h>

#include <cstdlib>
#include <map>
#include <random>
#include <string>

namespace {

std::string FreshDir(const std::string& name) {
    std::string dir = "/tmp/" + name;
    std::string cmd = "rm -rf " + dir;
    (void)system(cmd.c_str());
    return dir;
}

int CountSSTables(const std::string& dir) {
    int count = 0;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* entry = ::readdir(d)) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") ++count;
    }
    ::closedir(d);
    return count;
}

}  // namespace

TEST(Flush, ThresholdProducesSSTableFiles) {
    std::string dir = FreshDir("flush_threshold");
    minidb::Options opts;
    opts.flush_threshold_bytes = 1024;
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(db->Put("key" + std::to_string(i), std::string(20, 'x')));
    }

    EXPECT_GT(CountSSTables(dir), 0) << "writes well past the threshold should have flushed";
}

TEST(Flush, ReadsAfterFlushStillWork) {
    std::string dir = FreshDir("flush_reads");
    minidb::Options opts;
    opts.flush_threshold_bytes = 512;
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)));
    }
    ASSERT_GT(CountSSTables(dir), 0);

    for (int i = 0; i < 200; ++i) {
        auto v = db->Get("k" + std::to_string(i));
        ASSERT_TRUE(v.has_value()) << "k" << i;
        EXPECT_EQ(*v, "v" + std::to_string(i));
    }
}

TEST(Flush, OverwriteAfterFlushShadowsOlderFile) {
    std::string dir = FreshDir("flush_overwrite");
    minidb::Options opts;
    opts.flush_threshold_bytes = 16;  // flush almost immediately
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    ASSERT_TRUE(db->Put("a", "old"));
    ASSERT_TRUE(db->Put("b", "filler-to-cross-threshold"));  // forces the first flush
    ASSERT_GT(CountSSTables(dir), 0);

    ASSERT_TRUE(db->Put("a", "new"));
    EXPECT_EQ(db->Get("a").value_or("MISSING"), "new");
}

TEST(Flush, DeleteAfterFlushShadowsOlderFile) {
    std::string dir = FreshDir("flush_delete");
    minidb::Options opts;
    opts.flush_threshold_bytes = 16;
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    ASSERT_TRUE(db->Put("a", "present"));
    ASSERT_TRUE(db->Put("b", "filler-to-cross-threshold"));
    ASSERT_GT(CountSSTables(dir), 0);

    ASSERT_TRUE(db->Delete("a"));
    EXPECT_FALSE(db->Get("a").has_value())
        << "a tombstone in the memtable must shadow the value already flushed to disk";
}

TEST(Flush, SurvivesReopenAcrossFlushedFiles) {
    std::string dir = FreshDir("flush_reopen");
    minidb::Options opts;
    opts.flush_threshold_bytes = 512;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        for (int i = 0; i < 300; ++i) {
            ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)));
        }
        ASSERT_TRUE(db->Delete("k5"));
    }
    ASSERT_GT(CountSSTables(dir), 0);

    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 300; ++i) {
        if (i == 5) {
            EXPECT_FALSE(db->Get("k5").has_value());
            continue;
        }
        auto v = db->Get("k" + std::to_string(i));
        ASSERT_TRUE(v.has_value()) << "k" << i;
        EXPECT_EQ(*v, "v" + std::to_string(i));
    }
}

TEST(Flush, ScanMergesMemtableAndSSTables) {
    std::string dir = FreshDir("flush_scan");
    minidb::Options opts;
    opts.flush_threshold_bytes = 128;
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    for (int i = 0; i < 50; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "k%03d", i);
        ASSERT_TRUE(db->Put(key, "v" + std::to_string(i)));
    }
    ASSERT_GT(CountSSTables(dir), 0) << "scan test needs data split across memtable and files";
    ASSERT_TRUE(db->Delete("k010"));
    ASSERT_TRUE(db->Put("k020", "overwritten"));

    auto result = db->Scan("k005", "k015");
    ASSERT_EQ(result.size(), 9u);  // k005..k014 minus the deleted k010
    for (auto& [key, value] : result) EXPECT_NE(key, "k010");

    auto full = db->Scan("", "");
    bool found_overwrite = false;
    for (auto& [key, value] : full) {
        if (key == "k020") {
            EXPECT_EQ(value, "overwritten");
            found_overwrite = true;
        }
    }
    EXPECT_TRUE(found_overwrite);
}

TEST(Flush, LargeVolumeMatchesStdMapAcrossManyFlushes) {
    std::string dir = FreshDir("flush_oracle");
    minidb::Options opts;
    opts.flush_threshold_bytes = 2048;  // small enough to force many flushes
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);

    std::map<std::string, std::string> model;
    std::mt19937 rng(7);
    const int kKeySpace = 5000;  // large enough that unique bytes cross the threshold repeatedly

    for (int i = 0; i < 20000; ++i) {
        int op = rng() % 100;
        int k = rng() % kKeySpace;
        std::string key = "key" + std::to_string(k);

        if (op < 70) {
            std::string value = "val" + std::to_string(rng() % kKeySpace);
            ASSERT_TRUE(db->Put(key, value));
            model[key] = value;
        } else {
            ASSERT_TRUE(db->Delete(key));
            model.erase(key);
        }
    }
    ASSERT_GT(CountSSTables(dir), 1) << "this workload should have forced multiple flushes";

    for (auto& [key, value] : model) {
        auto got = db->Get(key);
        ASSERT_TRUE(got.has_value()) << key;
        EXPECT_EQ(*got, value) << key;
    }

    auto full = db->Scan("", "");
    std::vector<std::pair<std::string, std::string>> expected(model.begin(), model.end());
    EXPECT_EQ(full, expected);
}
