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

namespace {

std::vector<uint64_t> SeqsOf(const std::vector<CompactionInput>& inputs) {
    std::vector<uint64_t> seqs;
    seqs.reserve(inputs.size());
    for (const auto& in : inputs) seqs.push_back(in.seq);
    return seqs;
}

// MergingIterator breaks ties by source position, so the sources must be in
// age order for newest-wins to hold. Sorting here rather than trusting the
// caller's order keeps that a property of the seqs alone.
std::vector<std::unique_ptr<SSTable::Iterator>> IteratorsOldestFirst(
    std::vector<CompactionInput> inputs) {
    std::sort(inputs.begin(), inputs.end(),
              [](const CompactionInput& a, const CompactionInput& b) { return a.seq < b.seq; });
    std::vector<std::unique_ptr<SSTable::Iterator>> its;
    its.reserve(inputs.size());
    for (const auto& in : inputs) its.push_back(in.table->NewIterator());
    return its;
}

}  // namespace

CompactionIterator::CompactionIterator(const std::vector<CompactionInput>& inputs,
                                       const std::vector<uint64_t>& live_seqs)
    : CompactionIterator(IteratorsOldestFirst(inputs),
                         CoversOldestGeneration(SeqsOf(inputs), live_seqs)) {}

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
