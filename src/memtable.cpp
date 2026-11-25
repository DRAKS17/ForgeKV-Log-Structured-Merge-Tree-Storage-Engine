#include "memtable.h"

#include <stdexcept>

namespace forgekv {

MemTable::MemTable()
    : head_(new SkipNode("", ValueEntry{"", false}, MAX_LEVEL)),
      current_level_(1),
      size_bytes_(0),
      entry_count_(0),
      rng_(std::random_device{}()) {}

MemTable::~MemTable() {
    free_all_nodes();
    delete head_;
    head_ = nullptr;
}

void MemTable::free_all_nodes() {
    SkipNode* curr = head_->forward[0];
    while (curr != nullptr) {
        SkipNode* next = curr->forward[0];
        delete curr;
        curr = next;
    }
    for (int i = 0; i < MAX_LEVEL; ++i) {
        head_->forward[i] = nullptr;
    }
}

int MemTable::random_level() {
    // Flip a fair coin (p = 0.5) to promote a node up each additional express lane.
    int level = 1;
    while (level < MAX_LEVEL && (rng_() & 1U) == 0U) {
        ++level;
    }
    return level;
}

void MemTable::upsert(const std::string& key, ValueEntry entry) {
    if (key.empty()) {
        throw std::invalid_argument("ForgeKV keys must not be empty");
    }

    std::vector<SkipNode*> update(MAX_LEVEL, head_);
    SkipNode* curr = head_;

    // Walk down from the highest active express lane to level 0,
    // recording the rightmost node strictly less than `key` at each level.
    for (int lvl = current_level_ - 1; lvl >= 0; --lvl) {
        while (curr->forward[lvl] != nullptr && curr->forward[lvl]->key < key) {
            curr = curr->forward[lvl];
        }
        update[lvl] = curr;
    }

    SkipNode* target = curr->forward[0];
    if (target != nullptr && target->key == key) {
        // Key already exists: adjust byte footprint for the new value length only.
        size_bytes_ -= target->entry.value.size();
        size_bytes_ += entry.value.size();
        target->entry = std::move(entry);
        return;
    }

    int new_level = random_level();
    if (new_level > current_level_) {
        for (int lvl = current_level_; lvl < new_level; ++lvl) {
            update[lvl] = head_;
        }
        current_level_ = new_level;
    }

    auto* new_node = new SkipNode(key, std::move(entry), new_level);
    for (int lvl = 0; lvl < new_level; ++lvl) {
        new_node->forward[lvl] = update[lvl]->forward[lvl];
        update[lvl]->forward[lvl] = new_node;
    }

    size_bytes_ += new_node->key.size() + new_node->entry.value.size() +
                   sizeof(SkipNode) +
                   new_node->forward.size() * sizeof(SkipNode*);
    ++entry_count_;
}

void MemTable::put(const std::string& key, const std::string& value) {
    upsert(key, ValueEntry{value, false});
}

void MemTable::del(const std::string& key) {
    // Store an explicit tombstone with an empty value so older SSTable entries stay shadowed.
    upsert(key, ValueEntry{"", true});
}

std::optional<ValueEntry> MemTable::get(const std::string& key) const {
    if (key.empty()) {
        return std::nullopt;
    }

    const SkipNode* curr = head_;
    for (int lvl = current_level_ - 1; lvl >= 0; --lvl) {
        while (curr->forward[lvl] != nullptr && curr->forward[lvl]->key < key) {
            curr = curr->forward[lvl];
        }
    }

    const SkipNode* candidate = curr->forward[0];
    if (candidate != nullptr && candidate->key == key) {
        return candidate->entry;
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, ValueEntry>> MemTable::scan(
    const std::string& start_key, const std::string& end_key) const {
    std::vector<std::pair<std::string, ValueEntry>> results;
    if (start_key > end_key) {
        return results;
    }

    const SkipNode* curr = head_;
    for (int lvl = current_level_ - 1; lvl >= 0; --lvl) {
        while (curr->forward[lvl] != nullptr &&
               curr->forward[lvl]->key < start_key) {
            curr = curr->forward[lvl];
        }
    }

    curr = curr->forward[0];
    while (curr != nullptr && curr->key <= end_key) {
        results.emplace_back(curr->key, curr->entry);
        curr = curr->forward[0];
    }
    return results;
}

std::vector<std::pair<std::string, ValueEntry>> MemTable::all_entries() const {
    std::vector<std::pair<std::string, ValueEntry>> results;
    results.reserve(entry_count_);

    const SkipNode* curr = head_->forward[0];
    while (curr != nullptr) {
        results.emplace_back(curr->key, curr->entry);
        curr = curr->forward[0];
    }
    return results;
}

void MemTable::clear() {
    free_all_nodes();
    current_level_ = 1;
    size_bytes_ = 0;
    entry_count_ = 0;
}

}  // namespace forgekv
