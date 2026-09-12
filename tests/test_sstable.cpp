#include <gtest/gtest.h>
#include "sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace {

std::string FreshPath(const std::string& name) {
    std::string path = "/tmp/" + name + ".sst";
    ::unlink(path.c_str());
    return path;
}

off_t FileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return st.st_size;
}

}  // namespace

using minidb::SSTable;

TEST(SSTable, WriteThenGetRoundTrips) {
    std::string path = FreshPath("sst_basic");
    std::vector<SSTable::Entry> entries = {
        {"a", "1", false}, {"b", "2", false}, {"c", "3", false},
    };
    ASSERT_TRUE(SSTable::Write(path, entries));

    auto sst = SSTable::Open(path);
    ASSERT_NE(sst, nullptr);

    auto a = sst->Get("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->value, "1");
    EXPECT_FALSE(a->is_delete);

    EXPECT_FALSE(sst->Get("missing").has_value());
}

TEST(SSTable, TombstoneIsPreservedNotDropped) {
    std::string path = FreshPath("sst_tombstone");
    std::vector<SSTable::Entry> entries = {
        {"a", "1", false}, {"b", "", true}, {"c", "3", false},
    };
    ASSERT_TRUE(SSTable::Write(path, entries));

    auto sst = SSTable::Open(path);
    ASSERT_NE(sst, nullptr);

    auto b = sst->Get("b");
    ASSERT_TRUE(b.has_value()) << "a tombstone must still be a record, not absence";
    EXPECT_TRUE(b->is_delete);
}

TEST(SSTable, ScanReturnsHalfOpenRange) {
    std::string path = FreshPath("sst_scan");
    std::vector<SSTable::Entry> entries;
    for (int i = 0; i < 50; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "k%03d", i);
        entries.push_back({key, "v" + std::to_string(i), false});
    }
    ASSERT_TRUE(SSTable::Write(path, entries));

    auto sst = SSTable::Open(path);
    ASSERT_NE(sst, nullptr);

    auto result = sst->Scan("k010", "k015");
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result.front().key, "k010");
    EXPECT_EQ(result.back().key, "k014");

    auto to_end = sst->Scan("k048", "");
    ASSERT_EQ(to_end.size(), 2u);
}

TEST(SSTable, IndexSpansMultipleIntervalsAndStillFindsEveryKey) {
    // kIndexInterval is 16 internally; use enough keys to exercise several
    // sparse index buckets and boundary lookups between them.
    std::string path = FreshPath("sst_many");
    std::vector<SSTable::Entry> entries;
    for (int i = 0; i < 500; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "k%04d", i);
        entries.push_back({key, "v" + std::to_string(i), false});
    }
    ASSERT_TRUE(SSTable::Write(path, entries));

    auto sst = SSTable::Open(path);
    ASSERT_NE(sst, nullptr);

    for (int i = 0; i < 500; i += 37) {
        char key[8];
        std::snprintf(key, sizeof(key), "k%04d", i);
        auto e = sst->Get(key);
        ASSERT_TRUE(e.has_value()) << key;
        EXPECT_EQ(e->value, "v" + std::to_string(i));
    }
}

TEST(SSTable, EmptyEntriesProducesEmptyReadableFile) {
    std::string path = FreshPath("sst_empty");
    ASSERT_TRUE(SSTable::Write(path, {}));

    auto sst = SSTable::Open(path);
    ASSERT_NE(sst, nullptr);
    EXPECT_FALSE(sst->Get("anything").has_value());
    EXPECT_TRUE(sst->Scan("", "").empty());
}

TEST(SSTable, CorruptedDataBlockFailsOpen) {
    std::string path = FreshPath("sst_corrupt");
    std::vector<SSTable::Entry> entries = {{"a", "1", false}, {"b", "2", false}};
    ASSERT_TRUE(SSTable::Write(path, entries));

    // Flip a byte inside the data block; the trailing CRC will no longer match.
    int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    char b;
    ASSERT_EQ(::pread(fd, &b, 1, 5), 1);
    b = static_cast<char>(b ^ 0xff);
    ASSERT_EQ(::pwrite(fd, &b, 1, 5), 1);
    ::close(fd);

    EXPECT_EQ(SSTable::Open(path), nullptr)
        << "a fsynced SSTable has no tail to forgive; any mismatch is real corruption";
}

TEST(SSTable, TruncatedFileFailsOpen) {
    std::string path = FreshPath("sst_truncated");
    std::vector<SSTable::Entry> entries = {{"a", "1", false}, {"b", "2", false}};
    ASSERT_TRUE(SSTable::Write(path, entries));

    off_t full = FileSize(path);
    ASSERT_GT(full, 0);
    ASSERT_EQ(::truncate(path.c_str(), full - 4), 0);

    EXPECT_EQ(SSTable::Open(path), nullptr);
}

TEST(SSTable, MissingFileFailsOpen) {
    EXPECT_EQ(SSTable::Open("/tmp/does_not_exist.sst"), nullptr);
}
