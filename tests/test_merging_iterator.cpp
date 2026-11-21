#include <gtest/gtest.h>
#include "merging_iterator.h"
#include "sstable.h"

#include <unistd.h>

#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

using minidb::MergingIterator;
using minidb::SSTable;

// Owns the SSTables so their iterators (which borrow them) stay valid for
// the whole test.
class Files {
public:
    // Each inner vector becomes one file; pass them oldest first.
    explicit Files(const std::string& name,
                   const std::vector<std::vector<SSTable::Entry>>& contents) {
        for (size_t i = 0; i < contents.size(); ++i) {
            std::string path = "/tmp/" + name + "_" + std::to_string(i) + ".sst";
            ::unlink(path.c_str());
            EXPECT_TRUE(SSTable::Write(path, contents[i]));
            tables_.push_back(SSTable::Open(path));
            EXPECT_NE(tables_.back(), nullptr);
        }
    }

    MergingIterator Merge() const {
        std::vector<std::unique_ptr<SSTable::Iterator>> its;
        for (const auto& t : tables_) its.push_back(t->NewIterator());
        return MergingIterator(std::move(its));
    }

private:
    std::vector<std::unique_ptr<SSTable>> tables_;
};

std::vector<SSTable::Entry> Drain(MergingIterator& it) {
    std::vector<SSTable::Entry> out;
    for (; it.Valid(); it.Next()) out.push_back(it.entry());
    EXPECT_TRUE(it.ok());
    return out;
}

SSTable::Entry Put(const std::string& k, const std::string& v) { return {k, v, false}; }
SSTable::Entry Del(const std::string& k) { return {k, "", true}; }

void ExpectEntries(const std::vector<SSTable::Entry>& got,
                   const std::vector<SSTable::Entry>& want) {
    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(got[i].key, want[i].key) << "at " << i;
        EXPECT_EQ(got[i].value, want[i].value) << "at " << i;
        EXPECT_EQ(got[i].is_delete, want[i].is_delete) << "at " << i;
    }
}

}  // namespace

TEST(MergingIterator, NoSourcesIsEmpty) {
    MergingIterator it({});
    EXPECT_FALSE(it.Valid());
    EXPECT_TRUE(it.ok());
}

TEST(MergingIterator, EmptyFilesAreSkipped) {
    Files f("merge_empty", {{}, {Put("a", "1")}, {}});
    auto it = f.Merge();
    ExpectEntries(Drain(it), {Put("a", "1")});
}

TEST(MergingIterator, DisjointFilesInterleaveIntoOneSortedStream) {
    Files f("merge_disjoint", {
        {Put("a", "1"), Put("d", "4"), Put("g", "7")},
        {Put("b", "2"), Put("e", "5")},
        {Put("c", "3"), Put("f", "6"), Put("h", "8")},
    });
    auto it = f.Merge();
    ExpectEntries(Drain(it), {Put("a", "1"), Put("b", "2"), Put("c", "3"), Put("d", "4"),
                              Put("e", "5"), Put("f", "6"), Put("g", "7"), Put("h", "8")});
}

TEST(MergingIterator, NewestFileWinsOnDuplicateKey) {
    Files f("merge_dup", {
        {Put("k", "oldest"), Put("only_old", "x")},
        {Put("k", "middle")},
        {Put("k", "newest"), Put("only_new", "y")},
    });
    auto it = f.Merge();
    ExpectEntries(Drain(it), {Put("k", "newest"), Put("only_new", "y"), Put("only_old", "x")});
}

TEST(MergingIterator, NewerTombstoneShadowsOlderValueAndIsEmitted) {
    // The tombstone must come out of the merge: dropping it is a compaction
    // decision (M3.3), and dropping the older value without it would make a
    // partial merge resurrect the key.
    Files f("merge_tomb", {
        {Put("a", "1"), Put("b", "2")},
        {Del("a")},
    });
    auto it = f.Merge();
    ExpectEntries(Drain(it), {Del("a"), Put("b", "2")});
}

TEST(MergingIterator, NewerPutOverridesOlderTombstone) {
    Files f("merge_tomb_then_put", {
        {Del("a")},
        {Put("a", "back")},
    });
    auto it = f.Merge();
    ExpectEntries(Drain(it), {Put("a", "back")});
}

TEST(MergingIterator, SourceErrorStopsTheMergeAndIsReported) {
    // Second file's keys go backwards, so its iterator fails partway through.
    Files f("merge_err", {
        {Put("a", "1"), Put("b", "2"), Put("z", "26")},
        {Put("c", "3"), Put("y", "25"), Put("d", "4")},
    });
    auto it = f.Merge();
    size_t seen = 0;
    for (; it.Valid(); it.Next()) ++seen;
    EXPECT_FALSE(it.ok()) << "a failed source must not look like a clean end of stream";
    EXPECT_LT(seen, 6u);
}

TEST(MergingIterator, RandomizedMatchesStdMapAppliedOldestToNewest) {
    // Reference model: apply every file to a std::map in age order, so a
    // later file's put or tombstone overwrites an earlier one. That is
    // exactly the newest-wins rule, with tombstones kept as entries.
    // A small key space forces heavy overlap between files; files larger than
    // the SSTable index interval exercise the iterator across index buckets.
    std::mt19937 rng(20261121);
    constexpr int kFiles = 7;
    constexpr int kKeySpace = 400;

    std::vector<std::vector<SSTable::Entry>> contents(kFiles);
    std::map<std::string, SSTable::Entry> model;
    for (int f = 0; f < kFiles; ++f) {
        std::map<std::string, SSTable::Entry> file;  // sorted, one entry per key
        int n = static_cast<int>(rng() % 250);
        for (int i = 0; i < n; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "key%04d", static_cast<int>(rng() % kKeySpace));
            bool del = rng() % 4 == 0;
            file[key] = {key, del ? "" : "f" + std::to_string(f) + "_" + std::to_string(i), del};
        }
        for (auto& [k, e] : file) {
            contents[f].push_back(e);
            model[k] = e;
        }
    }

    Files files("merge_random", contents);
    auto it = files.Merge();
    std::vector<SSTable::Entry> want;
    for (auto& [k, e] : model) want.push_back(e);
    ExpectEntries(Drain(it), want);
}
