#pragma once

#include "common.h"

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

/**
 * Probabilistic SkipList-backed in-memory write buffer (MemTable).
 *
 * Maintains keys in sorted lexicographical order with expected O(log N)
 * insertion and lookup, and O(N) ordered traversal via level-0 forward links.
 */
class MemTable {
public:
    static constexpr int MAX_LEVEL = 16;

    MemTable();
    ~MemTable();

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    /// Insert or overwrite a live key-value pair. Throws if key is empty.
    void put(const std::string& key, const std::string& value);

    /// Insert or overwrite a key with a tombstone marker (is_deleted = true).
    void del(const std::string& key);

    /// Look up a key. Distinguishes absent (nullopt) from tombstone (is_deleted = true).
    std::optional<ValueEntry> get(const std::string& key) const;

    /// Return all entries (including tombstones) with keys in [start_key, end_key].
    std::vector<std::pair<std::string, ValueEntry>> scan(
        const std::string& start_key, const std::string& end_key) const;

    /// Return all entries in sorted key order (used when flushing to an SSTable).
    std::vector<std::pair<std::string, ValueEntry>> all_entries() const;

    /// Remove all entries and reset memory tracking back to zero.
    void clear();

    /// Approximate active memory footprint in bytes (used to trigger SSTable flushes).
    std::size_t approximate_size_bytes() const { return size_bytes_; }

    /// Total number of keys currently stored (live keys + tombstones).
    std::size_t entry_count() const { return entry_count_; }

private:
    struct SkipNode {
        std::string key;
        ValueEntry entry;
        std::vector<SkipNode*> forward;

        SkipNode(std::string k, ValueEntry e, int level)
            : key(std::move(k)), entry(std::move(e)), forward(level, nullptr) {}
    };

    int random_level();
    void upsert(const std::string& key, ValueEntry entry);
    void free_all_nodes();

    SkipNode* head_;
    int current_level_;
    std::size_t size_bytes_;
    std::size_t entry_count_;
    std::mt19937 rng_;
};

}  // namespace forgekv
