#include "compaction.h"

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace forgekv {

namespace {

struct CompactionHeapNode {
    std::string key;
    ValueEntry entry;
    uint64_t sst_sequence_num = 0;
    std::size_t reader_idx = 0;
};

struct CompactionHeapCompare {
    bool operator()(const CompactionHeapNode& a,
                    const CompactionHeapNode& b) const {
        if (a.key != b.key) {
            return a.key > b.key;  // Smallest key pops first.
        }
        // Tie-breaker: highest SSTable sequence number pops first (newest wins).
        return a.sst_sequence_num < b.sst_sequence_num;
    }
};

}  // namespace

bool CompactionManager::compact(
    const std::vector<std::unique_ptr<SSTableReader>>& sstables,
    const std::string& output_filepath,
    bool is_bottom_level) {
    if (sstables.empty()) {
        return false;
    }

    std::vector<std::unique_ptr<SSTableReader::Iterator>> iters;
    iters.reserve(sstables.size());

    std::priority_queue<CompactionHeapNode,
                        std::vector<CompactionHeapNode>,
                        CompactionHeapCompare>
        min_heap;

    for (std::size_t i = 0; i < sstables.size(); ++i) {
        auto it = sstables[i]->new_iterator();
        if (it->valid()) {
            min_heap.push(CompactionHeapNode{
                it->key(), it->entry(), sstables[i]->sequence_number(), i});
            it->next();
        }
        iters.push_back(std::move(it));
    }

    std::vector<std::pair<std::string, ValueEntry>> merged_entries;
    bool has_last_key = false;
    std::string last_key;

    while (!min_heap.empty()) {
        CompactionHeapNode top = min_heap.top();
        min_heap.pop();

        // Advance the input SSTable stream that yielded `top`.
        auto& it = iters[top.reader_idx];
        if (it->valid()) {
            min_heap.push(CompactionHeapNode{
                it->key(), it->entry(),
                sstables[top.reader_idx]->sequence_number(), top.reader_idx});
            it->next();
        }

        // Skip older versions of a key already emitted.
        if (has_last_key && top.key == last_key) {
            continue;
        }
        last_key = std::move(top.key);
        has_last_key = true;

        // Purge tombstones only at the bottommost level where no older SSTables exist.
        if (is_bottom_level && top.entry.is_deleted) {
            continue;
        }

        merged_entries.emplace_back(last_key, std::move(top.entry));
    }

    if (merged_entries.empty()) {
        return false;
    }

    SSTableBuilder::build_from_entries(output_filepath, merged_entries);
    return true;
}

}  // namespace forgekv
