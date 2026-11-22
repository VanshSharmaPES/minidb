#pragma once

#include <cstdint>
#include <vector>

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

}  // namespace minidb
