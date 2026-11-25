#include "forgekv.h"

#include "compaction.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace forgekv {

namespace {

namespace fs = std::filesystem;

/// Parse `sst_000123.sst` into sequence number 123; returns nullopt if not matching.
std::optional<uint64_t> parse_sst_sequence(const std::string& filename) {
    constexpr const char* prefix = "sst_";
    constexpr const char* suffix = ".sst";
    if (filename.size() <= 8) {
        return std::nullopt;
    }
    if (filename.compare(0, 4, prefix) != 0 ||
        filename.compare(filename.size() - 4, 4, suffix) != 0) {
        return std::nullopt;
    }
    const std::string num_str = filename.substr(4, filename.size() - 8);
    for (unsigned char c : num_str) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    try {
        return std::stoull(num_str);
    } catch (...) {
        return std::nullopt;
    }
}

struct HeapScanItem {
    std::string key;
    ValueEntry entry;
    uint64_t recency = 0;
    std::size_t source_idx = 0;  // SIZE_MAX indicates MemTable vector
};

struct HeapScanCompare {
    bool operator()(const HeapScanItem& a, const HeapScanItem& b) const {
        if (a.key != b.key) {
            return a.key > b.key;  // Min-heap by key ascending.
        }
        return a.recency < b.recency;  // Higher recency pops first.
    }
};

}  // namespace

ForgeKV::~ForgeKV() {
    close();
}

void ForgeKV::open(const std::string& dir,
                   std::size_t memtable_flush_bytes,
                   std::size_t auto_compact_threshold) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (is_open_) {
        close_unlocked();
    }

    data_dir_ = dir;
    memtable_flush_bytes_ = std::max<std::size_t>(memtable_flush_bytes, 1024);
    auto_compact_threshold_ = std::max<std::size_t>(auto_compact_threshold, 2);
    fs::create_directories(data_dir_);

    // Clean up any leftover .tmp files from an interrupted flush or compaction,
    // and collect all committed .sst files ordered by sequence number.
    std::vector<std::pair<uint64_t, std::string>> discovered_ssts;
    for (const auto& entry : fs::directory_iterator(data_dir_)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string fname = entry.path().filename().string();
        if (fname.size() >= 4 && fname.compare(fname.size() - 4, 4, ".tmp") == 0) {
            std::error_code ec;
            fs::remove(entry.path(), ec);
            continue;
        }
        auto seq = parse_sst_sequence(fname);
        if (seq.has_value()) {
            discovered_ssts.emplace_back(*seq, entry.path().string());
        }
    }

    std::sort(discovered_ssts.begin(), discovered_ssts.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    sstables_.clear();
    uint64_t max_seq = 0;
    for (const auto& [seq, path] : discovered_ssts) {
        sstables_.push_back(SSTableReader::open(path, seq));
        if (seq > max_seq) {
            max_seq = seq;
        }
    }
    next_sst_seq_ = max_seq + 1;

    // Replay WAL into fresh MemTable and reopen WAL for appends.
    memtable_ = std::make_unique<MemTable>();
    const std::string wal_path = (fs::path(data_dir_) / "wal.log").string();
    WALReader::recover(wal_path, *memtable_);
    wal_writer_ = std::make_unique<WALWriter>(wal_path);

    is_open_ = true;
}

std::string ForgeKV::format_sst_path(uint64_t sequence_number) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "sst_%06llu.sst",
                  static_cast<unsigned long long>(sequence_number));
    return (fs::path(data_dir_) / buf).string();
}

void ForgeKV::flush_memtable_unlocked() {
    if (memtable_->entry_count() == 0) {
        return;
    }

    const uint64_t seq = next_sst_seq_++;
    const std::string sst_path = format_sst_path(seq);
    SSTableBuilder::build_from_memtable(sst_path, *memtable_);

    sstables_.push_back(SSTableReader::open(sst_path, seq));
    memtable_->clear();
    wal_writer_->truncate();

    if (sstables_.size() >= auto_compact_threshold_) {
        compact_unlocked();
    }
}

void ForgeKV::compact_unlocked() {
    if (sstables_.empty()) {
        return;
    }

    const uint64_t new_seq = next_sst_seq_++;
    const std::string new_sst_path = format_sst_path(new_seq);

    // Compact all SSTables together (bottom level => safe to purge tombstones).
    const bool wrote_file = CompactionManager::compact(
        sstables_, new_sst_path, /*is_bottom_level=*/true);

    std::vector<std::string> old_paths;
    old_paths.reserve(sstables_.size());
    for (const auto& sst : sstables_) {
        old_paths.push_back(sst->filepath());
    }

    std::vector<std::unique_ptr<SSTableReader>> compacted;
    if (wrote_file) {
        compacted.push_back(SSTableReader::open(new_sst_path, new_seq));
    }
    sstables_ = std::move(compacted);

    for (const auto& old_path : old_paths) {
        std::error_code ec;
        fs::remove(old_path, ec);
    }
}

void ForgeKV::compact() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (!is_open_) {
        throw std::runtime_error("ForgeKV engine is not open");
    }
    if (memtable_->entry_count() > 0) {
        flush_memtable_unlocked();
    }
    compact_unlocked();
}

void ForgeKV::put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        throw std::invalid_argument("ForgeKV keys must not be empty");
    }

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (!is_open_) {
        throw std::runtime_error("ForgeKV engine is not open");
    }

    wal_writer_->append_put(key, value);
    memtable_->put(key, value);

    if (memtable_->approximate_size_bytes() >= memtable_flush_bytes_) {
        flush_memtable_unlocked();
    }
}

void ForgeKV::del(const std::string& key) {
    if (key.empty()) {
        throw std::invalid_argument("ForgeKV keys must not be empty");
    }

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (!is_open_) {
        throw std::runtime_error("ForgeKV engine is not open");
    }

    wal_writer_->append_del(key);
    memtable_->del(key);

    if (memtable_->approximate_size_bytes() >= memtable_flush_bytes_) {
        flush_memtable_unlocked();
    }
}

std::optional<std::string> ForgeKV::get(const std::string& key,
                                        bool use_bloom) const {
    if (key.empty()) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!is_open_) {
        throw std::runtime_error("ForgeKV engine is not open");
    }

    // 1. Always check active MemTable first.
    if (auto mem_res = memtable_->get(key); mem_res.has_value()) {
        if (mem_res->is_deleted) {
            return std::nullopt;
        }
        return mem_res->value;
    }

    // 2. Probe SSTables from newest to oldest, stopping immediately on a tombstone.
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        if (auto sst_res = (*it)->get(key, use_bloom); sst_res.has_value()) {
            if (sst_res->is_deleted) {
                return std::nullopt;
            }
            return sst_res->value;
        }
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> ForgeKV::scan(
    const std::string& start_key, const std::string& end_key) const {
    std::vector<std::pair<std::string, std::string>> results;
    if (start_key > end_key) {
        return results;
    }

    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!is_open_) {
        throw std::runtime_error("ForgeKV engine is not open");
    }

    std::priority_queue<HeapScanItem, std::vector<HeapScanItem>, HeapScanCompare>
        min_heap;

    // Seed heap with MemTable range entries (highest recency).
    auto mem_entries = memtable_->scan(start_key, end_key);
    std::size_t mem_idx = 0;
    constexpr std::size_t MEM_SOURCE = std::numeric_limits<std::size_t>::max();
    if (!mem_entries.empty()) {
        min_heap.push(HeapScanItem{mem_entries[0].first, mem_entries[0].second,
                                   std::numeric_limits<uint64_t>::max(),
                                   MEM_SOURCE});
        mem_idx = 1;
    }

    // Seed heap with streaming iterators for each SSTable starting at >= start_key.
    std::vector<std::unique_ptr<SSTableReader::Iterator>> sst_iters;
    sst_iters.reserve(sstables_.size());
    for (std::size_t i = 0; i < sstables_.size(); ++i) {
        auto it = sstables_[i]->new_iterator_at(start_key);
        if (it->valid() && it->key() <= end_key) {
            min_heap.push(HeapScanItem{it->key(), it->entry(),
                                       sstables_[i]->sequence_number(), i});
            it->next();
        }
        sst_iters.push_back(std::move(it));
    }

    bool has_last_key = false;
    std::string last_key;

    while (!min_heap.empty()) {
        HeapScanItem top = min_heap.top();
        min_heap.pop();

        // Advance the source stream that produced `top`.
        if (top.source_idx == MEM_SOURCE) {
            if (mem_idx < mem_entries.size()) {
                min_heap.push(HeapScanItem{
                    mem_entries[mem_idx].first, mem_entries[mem_idx].second,
                    std::numeric_limits<uint64_t>::max(), MEM_SOURCE});
                ++mem_idx;
            }
        } else {
            auto& it = sst_iters[top.source_idx];
            if (it->valid() && it->key() <= end_key) {
                min_heap.push(HeapScanItem{
                    it->key(), it->entry(),
                    sstables_[top.source_idx]->sequence_number(),
                    top.source_idx});
                it->next();
            }
        }

        // Emit only the newest version of each key, skipping tombstones.
        if (!has_last_key || top.key != last_key) {
            last_key = std::move(top.key);
            has_last_key = true;
            if (!top.entry.is_deleted) {
                results.emplace_back(last_key, std::move(top.entry.value));
            }
        }
    }

    return results;
}

EngineStats ForgeKV::stats() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    EngineStats s;
    if (!is_open_) {
        return s;
    }

    s.memtable_entries = memtable_->entry_count();
    s.memtable_bytes = memtable_->approximate_size_bytes();
    s.sstable_count = sstables_.size();
    for (const auto& sst : sstables_) {
        s.total_disk_bytes += sst->file_size();
        s.total_bloom_hits += sst->bloom_hits();
        s.total_disk_reads += sst->disk_reads();
    }
    return s;
}

void ForgeKV::reset_read_stats() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    for (const auto& sst : sstables_) {
        sst->reset_stats();
    }
}

void ForgeKV::close_unlocked() {
    if (!is_open_) {
        return;
    }
    if (wal_writer_) {
        wal_writer_->close();
        wal_writer_.reset();
    }
    memtable_.reset();
    sstables_.clear();
    is_open_ = false;
}

void ForgeKV::close() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    close_unlocked();
}

bool ForgeKV::is_open() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return is_open_;
}

}  // namespace forgekv
