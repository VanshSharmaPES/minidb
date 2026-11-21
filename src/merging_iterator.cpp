#include "merging_iterator.h"

#include <algorithm>
#include <string>

namespace minidb {

MergingIterator::MergingIterator(std::vector<std::unique_ptr<SSTable::Iterator>> sources)
    : sources_(std::move(sources)) {
    heap_.reserve(sources_.size());
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (!sources_[i]->ok()) ok_ = false;
        if (sources_[i]->Valid()) Push(i);
    }
}

bool MergingIterator::Below(size_t a, size_t b) const {
    const std::string& ka = sources_[a]->entry().key;
    const std::string& kb = sources_[b]->entry().key;
    if (ka != kb) return ka > kb;
    return a < b;
}

void MergingIterator::Push(size_t i) {
    heap_.push_back(i);
    std::push_heap(heap_.begin(), heap_.end(),
                   [this](size_t a, size_t b) { return Below(a, b); });
}

size_t MergingIterator::Pop() {
    std::pop_heap(heap_.begin(), heap_.end(),
                  [this](size_t a, size_t b) { return Below(a, b); });
    size_t i = heap_.back();
    heap_.pop_back();
    return i;
}

void MergingIterator::Advance(size_t i) {
    sources_[i]->Next();
    if (!sources_[i]->ok()) {
        ok_ = false;
    } else if (sources_[i]->Valid()) {
        Push(i);
    }
}

void MergingIterator::Next() {
    if (!Valid()) return;

    // Copy the key: advancing the winning source overwrites its entry().
    size_t winner = Pop();
    std::string key = sources_[winner]->entry().key;
    Advance(winner);

    // Every other source sitting on the same key holds an older, shadowed
    // version of it. Each file has a key at most once, so one step each is
    // enough to move them all past it.
    while (ok_ && !heap_.empty() && sources_[heap_.front()]->entry().key == key) {
        Advance(Pop());
    }
}

}  // namespace minidb
