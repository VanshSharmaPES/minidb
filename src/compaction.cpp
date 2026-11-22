#include "compaction.h"

#include <algorithm>

namespace minidb {

bool CoversOldestGeneration(std::vector<uint64_t> input_seqs,
                            std::vector<uint64_t> live_seqs) {
    if (input_seqs.empty() || input_seqs.size() > live_seqs.size()) return false;

    std::sort(input_seqs.begin(), input_seqs.end());
    std::sort(live_seqs.begin(), live_seqs.end());

    // Live seqs come from distinct filenames, so they are strictly
    // increasing; matching them position by position therefore also rejects
    // duplicated inputs and inputs that aren't live at all.
    return std::equal(input_seqs.begin(), input_seqs.end(), live_seqs.begin());
}

CompactionIterator::CompactionIterator(
    std::vector<std::unique_ptr<SSTable::Iterator>> sources, bool drop_tombstones)
    : merge_(std::move(sources)), drop_tombstones_(drop_tombstones) {
    // The very first merged entry may already be a tombstone.
    SkipDropped();
}

void CompactionIterator::Next() {
    merge_.Next();
    SkipDropped();
}

void CompactionIterator::SkipDropped() {
    if (!drop_tombstones_) return;
    while (merge_.Valid() && merge_.entry().is_delete) merge_.Next();
}

}  // namespace minidb
