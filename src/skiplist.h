#pragma once

#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace minidb {

// Ordered map keyed by string, used as the memtable.
//
// A skip list is a linked list with extra "express lane" pointers. Each node
// picks a random height; a node of height h is linked into levels 0..h-1.
// Level 0 holds every node in sorted order, so Scan is just a walk along
// level 0. Higher levels skip ahead, giving O(log n) search on average
// without any rebalancing.
//
// This is what real LSM memtables use, because inserts never have to rotate
// or split nodes the way a balanced tree does.
class SkipList {
public:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node*> next;

        Node(std::string k, std::string v, int height)
            : key(std::move(k)), value(std::move(v)), next(height, nullptr) {}
    };

    SkipList() : head_(new Node("", "", kMaxHeight)), rng_(12345) {}

    ~SkipList() {
        Node* n = head_;
        while (n) {
            Node* next = n->next[0];
            delete n;
            n = next;
        }
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    void Insert(const std::string& key, const std::string& value);
    void Erase(const std::string& key);
    const Node* Find(const std::string& key) const;

    // First node with a key >= the argument, or nullptr past the end.
    const Node* LowerBound(const std::string& key) const;

    // Step to the next node in sorted order. Level 0 links every node.
    static const Node* Next(const Node* n) { return n->next[0]; }

    size_t size() const { return size_; }

private:
    static constexpr int kMaxHeight = 12;

    // Walks down from the top level to level 0. On the way, update[i] is left
    // pointing at the last node on level i whose key is < the target. That
    // array is exactly what Insert and Erase need in order to splice a node
    // in or out at every level it occupies.
    //
    // Returns the first node at level 0 with key >= the target, or nullptr.
    Node* FindGreaterOrEqual(const std::string& key,
                             std::vector<Node*>* update) const {
        Node* x = head_;
        for (int level = kMaxHeight - 1; level >= 0; --level) {
            while (x->next[level] != nullptr && x->next[level]->key < key) {
                x = x->next[level];
            }
            if (update) (*update)[level] = x;
        }
        return x->next[0];
    }

    // Height h with probability (1/4)^(h-1): most nodes are height 1, and each
    // extra level is four times rarer, which keeps the express lanes sparse
    // enough to be useful.
    int RandomHeight() {
        int height = 1;
        while (height < kMaxHeight && (rng_() % 4) == 0) ++height;
        return height;
    }

    Node* head_;
    std::mt19937 rng_;
    size_t size_ = 0;
};

inline void SkipList::Insert(const std::string& key, const std::string& value) {
    std::vector<Node*> update(kMaxHeight, nullptr);
    Node* found = FindGreaterOrEqual(key, &update);
    if (found != nullptr && found->key == key) {
        found->value = value;
        return;
    }
    int height = RandomHeight();
    Node* node = new Node(key, value, height);
    for (int level = 0; level < height; ++level) {
        node->next[level] = update[level]->next[level];
        update[level]->next[level] = node;
    }
    ++size_;
}

inline void SkipList::Erase(const std::string& key) {
    std::vector<Node*> update(kMaxHeight, nullptr);
    Node* found = FindGreaterOrEqual(key, &update);
    if (found == nullptr || found->key != key) 
        return;
    for (int level = 0; level < kMaxHeight; ++level) {
        if (update[level]->next[level] != found)
            break;
        update[level]->next[level] = found->next[level];
    }
    delete found;
    --size_;
}

inline const SkipList::Node* SkipList::Find(const std::string& key) const {
    Node* n = FindGreaterOrEqual(key, nullptr);
    if (n == nullptr || n->key != key) return nullptr;
    return n;
}

inline const SkipList::Node* SkipList::LowerBound(const std::string& key) const {
    return FindGreaterOrEqual(key, nullptr);
}

}  // namespace minidb