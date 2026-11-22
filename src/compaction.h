#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "merging_iterator.h"
#include "sstable.h"

namespace minidb {

// Whether a compaction over `input_seqs` may drop tombstones, given the
// sequence numbers of every SSTable currently live on disk.
//
// A tombstone exists to shadow older values for its key. Dropping it is only
// safe if no older value can be left behind for it to have shadowed, which
// means every file older than the tombstone's own must be part of the same
// compaction. Newer files don't matter: anything they hold already shadows
// the tombstone. Sequence numbers increase with flush order, so the
// condition is that the inputs are exactly the k oldest live files, for
// some k -- a prefix of the live set in sequence order.
//
// Under the current policy (compact every live file at once) this is always
// true. It is computed rather than assumed so that a partial-compaction
// policy, or a compaction started from a stale file list, falls back to
// keeping tombstones instead of silently resurrecting deleted keys.
//
// Conservative on anything unexpected: an empty input set, an input seq that
// isn't live, or a duplicated input all return false. Keeping a tombstone
// that could have been dropped only costs space; dropping one too early
// loses a delete.
bool CoversOldestGeneration(std::vector<uint64_t> input_seqs,
                            std::vector<uint64_t> live_seqs);

// The stream a compaction writes out: the newest-wins merge of its inputs,
// minus tombstones when they are safe to drop.
//
// Filtering happens on the merged stream, never on individual sources. A
// tombstone is how the merge knows to hide older values for its key; if it
// were skipped per source, before de-duplication, the next-newest source's
// value for that key would win the merge and the deleted key would come
// back. Skipping it after the merge has already picked it as the winner
// drops the key outright, which is the point.
//
// Whether to drop is the caller's decision for now, as a plain flag; it must
// only be true when CoversOldestGeneration holds for these inputs. (M3.3b.3
// replaces the flag with a factory that makes that call itself.)
class CompactionIterator {
public:
    // Sources oldest first, as for MergingIterator.
    CompactionIterator(std::vector<std::unique_ptr<SSTable::Iterator>> sources,
                       bool drop_tombstones);

    CompactionIterator(const CompactionIterator&) = delete;
    CompactionIterator& operator=(const CompactionIterator&) = delete;

    bool Valid() const { return merge_.Valid(); }
    const SSTable::Entry& entry() const { return merge_.entry(); }
    void Next();

    // Forwarded from the merge. As there, !Valid() with !ok() means a source
    // failed and the stream is incomplete, not finished.
    bool ok() const { return merge_.ok(); }

private:
    // Advances past any tombstones at the current position when dropping.
    void SkipDropped();

    MergingIterator merge_;
    bool drop_tombstones_;
};

}  // namespace minidb
