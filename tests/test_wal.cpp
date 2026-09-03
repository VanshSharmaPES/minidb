#include <gtest/gtest.h>
#include "db.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string FreshDir(const std::string& name) {
    std::string dir = "/tmp/" + name;
    std::string cmd = "rm -rf " + dir;
    (void)system(cmd.c_str());
    return dir;
}

off_t FileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return st.st_size;
}

void Truncate(const std::string& path, off_t bytes) {
    ASSERT_EQ(::truncate(path.c_str(), bytes), 0);
}

}  // namespace

TEST(WAL, DataSurvivesReopen) {
    std::string dir = FreshDir("wal_reopen");
    minidb::Options opts;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        ASSERT_TRUE(db->Put("a", "1"));
        ASSERT_TRUE(db->Put("b", ""));
        ASSERT_TRUE(db->Put("c", "3"));
        ASSERT_TRUE(db->Delete("c"));
    }
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->Get("a").value_or("MISSING"), "1");
    ASSERT_TRUE(db->Get("b").has_value());
    EXPECT_EQ(*db->Get("b"), "");
    EXPECT_FALSE(db->Get("c").has_value());
}

TEST(WAL, OverwritesReplayInOrder) {
    std::string dir = FreshDir("wal_order");
    minidb::Options opts;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        ASSERT_TRUE(db->Put("k", "first"));
        ASSERT_TRUE(db->Put("k", "second"));
        ASSERT_TRUE(db->Put("k", "third"));
    }
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->Get("k").value_or("MISSING"), "third");
}

TEST(WAL, TruncatedTailIsForgiven) {
    std::string dir = FreshDir("wal_trunc");
    minidb::Options opts;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        ASSERT_TRUE(db->Put("keep1", "v1"));
        ASSERT_TRUE(db->Put("keep2", "v2"));
    }
    std::string wal = dir + "/wal.log";
    off_t full = FileSize(wal);
    ASSERT_GT(full, 0);

    // Chop the last few bytes: exactly what a crash mid-write leaves behind.
    Truncate(wal, full - 5);

    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr) << "a truncated tail must not fail the open";
    EXPECT_EQ(db->Get("keep1").value_or("MISSING"), "v1");
    EXPECT_FALSE(db->Get("keep2").has_value())
        << "the torn record was never acknowledged, so it must be dropped";
}

TEST(WAL, HalfWrittenHeaderIsForgiven) {
    std::string dir = FreshDir("wal_halfhdr");
    minidb::Options opts;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        ASSERT_TRUE(db->Put("only", "v"));
    }
    std::string wal = dir + "/wal.log";
    off_t full = FileSize(wal);

    // Append two bytes of a length field that never got finished.
    int fd = ::open(wal.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const char junk[2] = {0x7f, 0x0a};
    ASSERT_EQ(::write(fd, junk, 2), 2);
    ::close(fd);
    ASSERT_EQ(FileSize(wal), full + 2);

    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->Get("only").value_or("MISSING"), "v");
}

TEST(WAL, MidFileCorruptionFailsLoudly) {
    std::string dir = FreshDir("wal_corrupt");
    minidb::Options opts;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(db->Put("key" + std::to_string(i), "value" + std::to_string(i)));
        }
    }
    std::string wal = dir + "/wal.log";

    // Flip a byte inside the first record. Valid records follow it, so this
    // cannot be a crash artifact.
    int fd = ::open(wal.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::lseek(fd, 12, SEEK_SET), 12);
    char b;
    ASSERT_EQ(::read(fd, &b, 1), 1);
    b = static_cast<char>(b ^ 0xff);
    ASSERT_EQ(::lseek(fd, 12, SEEK_SET), 12);
    ASSERT_EQ(::write(fd, &b, 1), 1);
    ::close(fd);

    auto db = minidb::DB::Open(dir, opts);
    EXPECT_EQ(db, nullptr) << "corruption with valid records after it must fail the open";
}

TEST(WAL, BatchedModeStillReplaysWhatWasSynced) {
    std::string dir = FreshDir("wal_batched");
    minidb::Options opts;
    opts.sync_every_write = false;
    opts.sync_interval = 10;
    {
        auto db = minidb::DB::Open(dir, opts);
        ASSERT_NE(db, nullptr);
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)));
        }
    }
    auto db = minidb::DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(db->Get("k" + std::to_string(i)).value_or("MISSING"),
                  "v" + std::to_string(i));
    }
}

TEST(WAL, SyncCostBenchmark) {
    const int kWrites = 10000;

    auto run = [&](bool sync_every, size_t interval) {
        std::string dir = FreshDir("wal_bench");
        minidb::Options opts;
        opts.sync_every_write = sync_every;
        opts.sync_interval = interval;
        auto db = minidb::DB::Open(dir, opts);
        EXPECT_NE(db, nullptr);

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kWrites; ++i) {
            db->Put("key" + std::to_string(i), "value" + std::to_string(i));
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double>(elapsed).count();
    };

    double synced = run(true, 0);
    double batched = run(false, 1000);

    std::cout << "\n  sync_every_write=true : " << synced << " s ("
              << (kWrites / synced) << " writes/sec)\n"
              << "  sync_interval=1000    : " << batched << " s ("
              << (kWrites / batched) << " writes/sec)\n"
              << "  ratio                 : " << (synced / batched) << "x\n\n";

    SUCCEED();
}