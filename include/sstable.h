#pragma once

#include "common.h"
#include "memtable.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

/// One entry in an SSTable's in-memory Sparse Index (every 16th key).
struct SparseIndexEntry {
    std::string key;
    uint64_t offset = 0;
};

/**
 * Serializes sorted key-value entries into an immutable .sst file on disk.
 *
 * File layout:
 *   1. Data Block    : sorted [op_type(1B) | key_len(4B) | val_len(4B) | key | val]
 *   2. Sparse Index  : every 16th key [key_len(4B) | key | data_file_offset(8B)]
 *   3. Bloom Filter  : (reserved 0 bytes in Stage 3; populated in Stage 4)
 *   4. Fixed Footer  : 36 bytes [index_off(8B)|index_sz(8B)|bloom_off(8B)|bloom_sz(8B)|magic(4B)]
 */
class SSTableBuilder {
public:
    /// Write all sorted entries from `memtable` to `filepath` atomically (.tmp -> rename).
    static void build_from_memtable(const std::string& filepath,
                                    const MemTable& memtable);

    /// Write pre-sorted entries (from MemTable or Compaction) to `filepath` atomically.
    static void build_from_entries(
        const std::string& filepath,
        const std::vector<std::pair<std::string, ValueEntry>>& entries);
};

/**
 * Reads an immutable .sst file using an in-memory Sparse Index and bounded disk reads.
 *
 * All lookup methods (`get`, `scan`, `new_iterator`) are safe to call concurrently
 * from multiple threads because they open independent read streams.
 */
class SSTableReader {
public:
    /// Streaming forward iterator over all records in the SSTable's Data Block.
    class Iterator {
    public:
        Iterator(const std::string& filepath, uint64_t data_end_offset);

        bool valid() const { return valid_; }
        void next();
        void seek(uint64_t start_offset, const std::string& target_key);

        const std::string& key() const { return current_key_; }
        const ValueEntry& entry() const { return current_entry_; }

    private:
        void read_current_record();

        std::ifstream stream_;
        uint64_t offset_ = 0;
        uint64_t data_end_offset_ = 0;
        bool valid_ = false;
        std::string current_key_;
        ValueEntry current_entry_;
    };

    /// Open an .sst file, validate its 36-byte footer magic, and load its Sparse Index.
    static std::unique_ptr<SSTableReader> open(const std::string& filepath,
                                               uint64_t sequence_number = 0);

    /// Point lookup using Sparse Index binary search + one bounded sequential disk read.
    std::optional<ValueEntry> get(const std::string& key) const;

    /// Range scan returning all entries (including tombstones) in [start_key, end_key].
    std::vector<std::pair<std::string, ValueEntry>> scan(
        const std::string& start_key, const std::string& end_key) const;

    /// Create a streaming iterator positioned at the beginning of the Data Block.
    std::unique_ptr<Iterator> new_iterator() const;

    /// Create a streaming iterator positioned at the first key >= `start_key`.
    std::unique_ptr<Iterator> new_iterator_at(const std::string& start_key) const;

    const std::string& filepath() const { return filepath_; }
    uint64_t sequence_number() const { return sequence_number_; }
    uint64_t file_size() const { return file_size_; }
    std::size_t sparse_index_size() const { return sparse_index_.size(); }

    uint64_t disk_reads() const { return disk_reads_.load(); }
    void reset_stats() const { disk_reads_.store(0); }

private:
    SSTableReader(std::string filepath,
                  uint64_t sequence_number,
                  uint64_t file_size,
                  uint64_t index_offset,
                  std::vector<SparseIndexEntry> sparse_index);

    uint64_t find_start_offset(const std::string& key, std::size_t* out_idx = nullptr) const;

    std::string filepath_;
    uint64_t sequence_number_;
    uint64_t file_size_;
    uint64_t index_offset_;
    std::vector<SparseIndexEntry> sparse_index_;
    mutable std::atomic<uint64_t> disk_reads_{0};
};

}  // namespace forgekv
