#include <gtest/gtest.h>
#include "compaction.h"
#include "merging_iterator.h"
#include "sstable.h"

#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

using minidb::CompactionIterator;
using minidb::CoversOldestGeneration;
using minidb::MergingIterator;
using minidb::SSTable;

namespace {

// Writes one SSTable per inner vector (oldest first) and keeps them open so
// the iterators borrowing them stay valid for the test.
class Inputs {
public:
    Inputs(const std::string& name, const std::vector<std::vector<SSTable::Entry>>& contents) {
        for (size_t i = 0; i < contents.size(); ++i) {
            std::string path = "/tmp/" + name + "_" + std::to_string(i) + ".sst";
            ::unlink(path.c_str());
            EXPECT_TRUE(SSTable::Write(path, contents[i]));
            tables_.push_back(SSTable::Open(path));
            EXPECT_NE(tables_.back(), nullptr);
        }
    }

    // File i as a compaction input. Seqs start at kFirstSeq in file order,
    // so file 0 is the oldest.
    static constexpr uint64_t kFirstSeq = 10;
    minidb::CompactionInput Input(size_t i) const {
        return {kFirstSeq + i, tables_.at(i).get()};
    }

    // Every file's seq: the live set when these files are all that's on disk.
    std::vector<uint64_t> LiveSeqs() const {
        std::vector<uint64_t> seqs;
        for (size_t i = 0; i < tables_.size(); ++i) seqs.push_back(kFirstSeq + i);
        return seqs;
    }

    std::vector<std::unique_ptr<SSTable::Iterator>> Iterators() const {
        std::vector<std::unique_ptr<SSTable::Iterator>> its;
        for (const auto& t : tables_) its.push_back(t->NewIterator());
        return its;
    }

private:
    std::vector<std::unique_ptr<SSTable>> tables_;
};

template <typename It>
std::vector<SSTable::Entry> Drain(It& it) {
    std::vector<SSTable::Entry> out;
    for (; it.Valid(); it.Next()) out.push_back(it.entry());
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

TEST(CoversOldestGeneration, AllLiveFilesCoverIt) {
    // The current policy: every live file is an input.
    EXPECT_TRUE(CoversOldestGeneration({3, 4, 5}, {3, 4, 5}));
}

TEST(CoversOldestGeneration, OldestPrefixCoversIt) {
    // Leaving newer files out is fine: anything they hold for a key already
    // shadows whatever the compaction writes.
    EXPECT_TRUE(CoversOldestGeneration({3}, {3, 4, 5}));
    EXPECT_TRUE(CoversOldestGeneration({3, 4}, {3, 4, 5}));
}

TEST(CoversOldestGeneration, NewestFilesOnlyMustNotDropTombstones) {
    // The "dropped too early" case: file 3 may hold a value that a tombstone
    // in 4 or 5 is shadowing. Dropping that tombstone would resurrect it.
    EXPECT_FALSE(CoversOldestGeneration({4, 5}, {3, 4, 5}));
    EXPECT_FALSE(CoversOldestGeneration({5}, {3, 4, 5}));
}

TEST(CoversOldestGeneration, GapInInputsMustNotDropTombstones) {
    // Includes the oldest file but skips one in between; a tombstone in 5
    // could be shadowing a value in 4.
    EXPECT_FALSE(CoversOldestGeneration({3, 5}, {3, 4, 5}));
}

TEST(CoversOldestGeneration, InputOrderDoesNotMatter) {
    EXPECT_TRUE(CoversOldestGeneration({5, 3, 4}, {4, 5, 3}));
}

TEST(CoversOldestGeneration, SeqGapsInLiveSetAreFine) {
    // Earlier compactions remove files, so live seqs need not be contiguous
    // numbers; only being a prefix of what is live matters.
    EXPECT_TRUE(CoversOldestGeneration({2, 7}, {2, 7, 9}));
    EXPECT_FALSE(CoversOldestGeneration({7, 9}, {2, 7, 9}));
}

TEST(CoversOldestGeneration, UnexpectedInputsAreConservative) {
    EXPECT_FALSE(CoversOldestGeneration({}, {3, 4})) << "empty input";
    EXPECT_FALSE(CoversOldestGeneration({}, {})) << "nothing live";
    EXPECT_FALSE(CoversOldestGeneration({2, 3}, {3, 4})) << "input not live (stale list)";
    EXPECT_FALSE(CoversOldestGeneration({3, 3}, {3, 4})) << "duplicated input";
    EXPECT_FALSE(CoversOldestGeneration({3, 4, 5}, {3, 4})) << "more inputs than live files";
}

TEST(CompactionIterator, KeepingTombstonesMatchesThePlainMerge) {
    // With dropping off, the filter must be invisible: same entries, same
    // order, tombstones included, as the merge it wraps.
    Inputs in("cit_keep", {
        {Put("a", "1"), Put("b", "2"), Put("d", "4")},
        {Del("b"), Put("c", "3")},
        {Put("a", "1new"), Del("e")},
    });
    MergingIterator merge(in.Iterators());
    CompactionIterator keep(in.Iterators(), /*drop_tombstones=*/false);

    auto merged = Drain(merge);
    ExpectEntries(Drain(keep), merged);
    EXPECT_TRUE(keep.ok());
    ExpectEntries(merged, {Put("a", "1new"), Del("b"), Put("c", "3"), Put("d", "4"), Del("e")});
}

TEST(CompactionIterator, DroppingTombstonesEmitsOnlyPuts) {
    Inputs in("cit_drop", {
        {Put("a", "1"), Del("c"), Put("e", "5")},
        {Del("b"), Put("d", "4"), Del("f")},
    });
    CompactionIterator it(in.Iterators(), /*drop_tombstones=*/true);
    ExpectEntries(Drain(it), {Put("a", "1"), Put("d", "4"), Put("e", "5")});
    EXPECT_TRUE(it.ok());
}

TEST(CompactionIterator, LeadingTombstonesAreSkippedBeforeTheFirstEntry) {
    // The first merged entries are tombstones, so the skip has to happen on
    // construction, not only on Next().
    Inputs in("cit_leading", {{Del("a"), Del("b"), Put("c", "3")}});
    CompactionIterator it(in.Iterators(), /*drop_tombstones=*/true);
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.entry().key, "c");
    ExpectEntries(Drain(it), {Put("c", "3")});
}

TEST(CompactionIterator, SourceErrorIsForwarded) {
    // Keys go backwards in this file, so its iterator fails partway through.
    Inputs in("cit_err", {{Put("a", "1"), Put("c", "3"), Put("b", "2")}});
    CompactionIterator it(in.Iterators(), /*drop_tombstones=*/false);
    Drain(it);
    EXPECT_FALSE(it.ok()) << "a failed source must not look like a finished compaction";
}

TEST(CompactionIterator, DroppedTombstoneDoesNotUncoverAnOlderValue) {
    // The newest version of "k" is a tombstone over older values. Dropping
    // must remove the key outright; if the filter ran before the merge
    // de-duplicated, an older "k" would win instead and the delete would
    // be undone.
    Inputs in("cit_shadow", {
        {Put("a", "1"), Put("k", "v1")},
        {Put("k", "v2")},
        {Del("k"), Put("z", "26")},
    });

    CompactionIterator drop(in.Iterators(), /*drop_tombstones=*/true);
    ExpectEntries(Drain(drop), {Put("a", "1"), Put("z", "26")});
    EXPECT_TRUE(drop.ok());

    CompactionIterator keep(in.Iterators(), /*drop_tombstones=*/false);
    ExpectEntries(Drain(keep), {Put("a", "1"), Del("k"), Put("z", "26")});
    EXPECT_TRUE(keep.ok());
}

TEST(CompactionIterator, PutOverTombstoneSurvivesInBothModes) {
    // Delete then re-insert: the newest version is a put, so the older
    // tombstone was already shadowed by the merge and the filter never sees
    // it. Neither mode may lose the re-inserted value.
    Inputs in("cit_reinsert", {
        {Put("k", "first")},
        {Del("k")},
        {Put("k", "again")},
    });
    for (bool drop : {true, false}) {
        CompactionIterator it(in.Iterators(), drop);
        ExpectEntries(Drain(it), {Put("k", "again")});
        EXPECT_TRUE(it.ok()) << "drop=" << drop;
    }
}

TEST(CompactionIterator, AllTombstonesDropToAnEmptyStream) {
    // Every key's newest version is a tombstone, some of them over older
    // values, and several in a row: the skip must run until the stream ends,
    // not stop after one.
    Inputs in("cit_all_dead", {
        {Put("a", "1"), Put("b", "2")},
        {Del("a"), Del("b"), Del("c"), Del("d")},
    });

    CompactionIterator drop(in.Iterators(), /*drop_tombstones=*/true);
    EXPECT_FALSE(drop.Valid());
    EXPECT_TRUE(drop.ok()) << "an empty result is a finished compaction, not an error";

    CompactionIterator keep(in.Iterators(), /*drop_tombstones=*/false);
    ExpectEntries(Drain(keep), {Del("a"), Del("b"), Del("c"), Del("d")});
}

TEST(CompactionIterator, ErrorWhileSkippingTombstonesIsStillReported) {
    // The failure is reached inside the skip loop, after tombstones the
    // filter consumed without emitting. "b" comes after "d" in the file, so
    // the source fails there; the stream must end with ok() false rather
    // than look like it ran out of entries.
    Inputs in("cit_err_skip", {{Put("a", "1"), Del("c"), Del("d"), Put("b", "2")}});
    CompactionIterator it(in.Iterators(), /*drop_tombstones=*/true);
    ExpectEntries(Drain(it), {Put("a", "1")});
    EXPECT_FALSE(it.ok());
}

TEST(CompactionIterator, ErrorDuringTheInitialSkipIsStillReported) {
    // Same, but the failure is hit by the constructor's skip, before the
    // caller has seen a single entry.
    Inputs in("cit_err_ctor", {{Del("c"), Del("d"), Put("b", "2")}});
    CompactionIterator it(in.Iterators(), /*drop_tombstones=*/true);
    EXPECT_FALSE(it.Valid());
    EXPECT_FALSE(it.ok());
}

// ---- Tombstone decision from sequence numbers -------------------------------
//
// Three files, oldest first: "k" is written, then deleted, then unrelated
// keys arrive with a tombstone for "c" (which never had a value).

namespace {

Inputs DecisionFiles(const std::string& name) {
    return Inputs(name, {
        {Put("k", "v")},
        {Put("a", "1"), Del("k")},
        {Put("b", "2"), Del("c")},
    });
}

}  // namespace

TEST(CompactionDecision, NewestFilesOnlyKeepTombstones) {
    // File 0 stays behind holding k=v. The compacted output must keep the
    // tombstone for "k", or reads would fall through to file 0 and find it.
    Inputs in = DecisionFiles("dec_newest");
    CompactionIterator it({in.Input(1), in.Input(2)}, in.LiveSeqs());
    EXPECT_FALSE(it.drops_tombstones());
    ExpectEntries(Drain(it), {Put("a", "1"), Put("b", "2"), Del("c"), Del("k")});
}

TEST(CompactionDecision, OldestPrefixDropsTombstones) {
    Inputs in = DecisionFiles("dec_prefix");
    CompactionIterator it({in.Input(0), in.Input(1)}, in.LiveSeqs());
    EXPECT_TRUE(it.drops_tombstones());
    ExpectEntries(Drain(it), {Put("a", "1")});
}

TEST(CompactionDecision, AllLiveFilesDropTombstones) {
    // The current compact-everything policy.
    Inputs in = DecisionFiles("dec_all");
    CompactionIterator it({in.Input(0), in.Input(1), in.Input(2)}, in.LiveSeqs());
    EXPECT_TRUE(it.drops_tombstones());
    ExpectEntries(Drain(it), {Put("a", "1"), Put("b", "2")});
}

TEST(CompactionDecision, StaleLiveSetKeepsTombstones) {
    // The live set no longer contains file 0's seq (as if the caller's view
    // of the inputs were out of date), and an unknown older file 9 is live.
    // That is not provably the oldest generation, so tombstones stay.
    Inputs in = DecisionFiles("dec_stale");
    CompactionIterator it({in.Input(0), in.Input(1), in.Input(2)}, {9, 11, 12});
    EXPECT_FALSE(it.drops_tombstones());
    ExpectEntries(Drain(it), {Put("a", "1"), Put("b", "2"), Del("c"), Del("k")});
}

TEST(CompactionDecision, InputsAreMergedInSeqOrderNotListedOrder) {
    // The caller lists the newer file first. The merge breaks ties by source
    // position, so without sorting by seq the older value would win.
    Inputs in("dec_order", {
        {Put("k", "old"), Put("x", "1")},
        {Put("k", "new")},
    });
    CompactionIterator it({in.Input(1), in.Input(0)}, in.LiveSeqs());
    ExpectEntries(Drain(it), {Put("k", "new"), Put("x", "1")});
}
