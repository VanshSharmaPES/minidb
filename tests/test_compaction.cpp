#include <gtest/gtest.h>
#include "compaction.h"

using minidb::CoversOldestGeneration;

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
