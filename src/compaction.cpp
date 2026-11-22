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

}  // namespace minidb
