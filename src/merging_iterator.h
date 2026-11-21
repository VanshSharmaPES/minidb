#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "sstable.h"

namespace minidb {

// Merges N SSTable iterators, each already sorted by key, into one sorted
// stream with exactly one entry per distinct key.
//
// Sources are passed oldest first, the same order DBImpl keeps sstables_ in.
// When several sources hold the same key, the entry from the newest source
// (highest index) is the one emitted and the older ones are skipped, matching
// the newest-wins rule Get and Scan already apply across flushes.
//
// Tombstones are emitted like any other entry, not dropped: whether a
// tombstone is safe to discard depends on what the caller is merging (see
// the Compaction section of the README), so that decision belongs to the
// compaction step, not here.
//
// Memory use is one current entry per source plus a heap of N indices,
// independent of how large the files are: each SSTable::Iterator reads a
// single record from disk at a time.
class MergingIterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<SSTable::Iterator>> sources);

    MergingIterator(const MergingIterator&) = delete;
    MergingIterator& operator=(const MergingIterator&) = delete;

    // True while entry() refers to a merged entry. Goes false once every
    // source is exhausted, or as soon as any source reports an error.
    bool Valid() const { return ok_ && !heap_.empty(); }

    // The current entry: the smallest remaining key, taken from the newest
    // source that holds it. Only meaningful while Valid().
    const SSTable::Entry& entry() const { return sources_[heap_.front()]->entry(); }

    // Advances past the current key in every source that holds it.
    void Next();

    // False if any source failed a read. A caller that sees !Valid() must
    // check this before treating the stream as complete; otherwise a read
    // error mid-merge looks like a clean end and the rest of the data is lost.
    bool ok() const { return ok_; }

private:
    // Heap ordering: smallest key on top, and for equal keys, the newest
    // (highest-index) source on top. Written as "a sorts below b" because
    // std::push_heap/pop_heap build a max-heap.
    bool Below(size_t a, size_t b) const;

    void Push(size_t i);
    size_t Pop();
    void Advance(size_t i);

    std::vector<std::unique_ptr<SSTable::Iterator>> sources_;
    std::vector<size_t> heap_;  // indices into sources_, valid ones only
    bool ok_ = true;
};

}  // namespace minidb
