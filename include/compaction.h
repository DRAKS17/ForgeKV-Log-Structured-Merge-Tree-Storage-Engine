#pragma once

#include "sstable.h"

#include <memory>
#include <string>
#include <vector>

namespace forgekv {

/**
 * Min-Heap K-Way Merge Compactor for immutable SSTables.
 *
 * Merges multiple overlapping SSTables into one sorted SSTable ordered by
 * (key ASC, sst_sequence_num DESC), keeping only the newest version of each
 * key and purging tombstones when compacting down to the bottommost level.
 */
class CompactionManager {
public:
    /// Compact `sstables` into `output_filepath`. Returns true if an SSTable was
    /// written, or false if all entries were purged (e.g. only bottom tombstones).
    static bool compact(
        const std::vector<std::unique_ptr<SSTableReader>>& sstables,
        const std::string& output_filepath,
        bool is_bottom_level);
};

}  // namespace forgekv
