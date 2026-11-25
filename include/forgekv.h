#pragma once

#include "memtable.h"
#include "sstable.h"
#include "wal.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

/// Runtime observability metrics for ForgeKV.
struct EngineStats {
    std::size_t memtable_entries = 0;
    std::size_t memtable_bytes = 0;
    std::size_t sstable_count = 0;
    uint64_t total_disk_bytes = 0;
    uint64_t total_bloom_hits = 0;
    uint64_t total_disk_reads = 0;
};

/**
 * Thread-safe embedded Log-Structured Merge-Tree (LSM-Tree) Key-Value Engine.
 *
 * Coordinates WAL durability, SkipList MemTable buffering, threshold flushing
 * to immutable SSTables with Bloom Filters & Sparse Indexes, and crash recovery.
 * Multiple readers (`get`, `scan`, `stats`) run concurrently via std::shared_mutex;
 * writers (`put`, `del`, `open`, `close`) acquire exclusive locks.
 */
class ForgeKV {
public:
    static constexpr std::size_t DEFAULT_AUTO_COMPACT_THRESHOLD = 4;

    ForgeKV() = default;
    ~ForgeKV();

    ForgeKV(const ForgeKV&) = delete;
    ForgeKV& operator=(const ForgeKV&) = delete;

    /// Open (or create) a database directory and replay any existing SSTables + WAL.
    void open(const std::string& dir,
              std::size_t memtable_flush_bytes = 64 * 1024,
              std::size_t auto_compact_threshold = DEFAULT_AUTO_COMPACT_THRESHOLD);

    /// Insert or update a key-value pair (WAL-first, then MemTable, auto-flush).
    void put(const std::string& key, const std::string& value);

    /// Point lookup checking MemTable first, then SSTables newest-to-oldest.
    std::optional<std::string> get(const std::string& key,
                                   bool use_bloom = true) const;

    /// Delete a key by appending a durable tombstone marker.
    void del(const std::string& key);

    /// Sorted, deduplicated range scan over [start_key, end_key] omitting tombstones.
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start_key, const std::string& end_key) const;

    /// Run K-way merge compaction across all SSTables to reclaim disk space.
    void compact();

    /// Return current engine statistics.
    EngineStats stats() const;

    /// Reset cumulative Bloom filter and disk read counters across all SSTables.
    void reset_read_stats() const;

    /// Flush active WAL stream and close the engine cleanly.
    void close();

    bool is_open() const;

private:
    void flush_memtable_unlocked();
    void compact_unlocked();
    void close_unlocked();
    std::string format_sst_path(uint64_t sequence_number) const;

    mutable std::shared_mutex rw_mutex_;
    bool is_open_ = false;
    std::string data_dir_;
    std::size_t memtable_flush_bytes_ = 64 * 1024;
    std::size_t auto_compact_threshold_ = DEFAULT_AUTO_COMPACT_THRESHOLD;
    uint64_t next_sst_seq_ = 1;

    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<WALWriter> wal_writer_;
    std::vector<std::unique_ptr<SSTableReader>> sstables_;
};

}  // namespace forgekv
