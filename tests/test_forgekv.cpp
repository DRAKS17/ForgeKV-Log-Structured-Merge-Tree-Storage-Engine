#include "bloom_filter.h"
#include "forgekv.h"
#include "sstable.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

#define ASSERT_TRUE(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__               \
                      << " assertion failed: " #cond "\n";                      \
            std::exit(1);                                                       \
        }                                                                       \
    } while (false)

namespace fs = std::filesystem;

const std::string TEST_DB_DIR = "data/test_stage4_db";

std::string pad_key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k_%06d", i);
    return std::string(buf);
}

void cleanup_dir(const std::string& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void test_bloom_filter_accuracy_and_serialization() {
    constexpr int N = 10000;
    forgekv::BloomFilter bf(N, 0.01);

    for (int i = 0; i < N; ++i) {
        bf.insert("present_" + std::to_string(i));
    }

    // Zero false negatives allowed.
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(bf.possibly_contains("present_" + std::to_string(i)));
    }

    // Serialize -> deserialize round trip.
    auto bytes = bf.serialize();
    auto restored = forgekv::BloomFilter::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(restored.num_bits() == bf.num_bits());
    ASSERT_TRUE(restored.num_hashes() == bf.num_hashes());

    // Measure false positives across 10,000 absent keys (target ~1%, allow < 2%).
    int false_positives = 0;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(restored.possibly_contains("present_" + std::to_string(i)));
        if (restored.possibly_contains("absent_" + std::to_string(i))) {
            ++false_positives;
        }
    }
    ASSERT_TRUE(false_positives < 200);
}

void test_sstable_bloom_gate_avoids_disk_reads() {
    const std::string sst_path = "data/test_stage4_bloom.sst";
    std::error_code ec;
    fs::remove(sst_path, ec);

    forgekv::MemTable mt;
    for (int i = 0; i < 2000; ++i) {
        mt.put(pad_key(i), "val_" + std::to_string(i));
    }
    forgekv::SSTableBuilder::build_from_memtable(sst_path, mt);

    auto reader = forgekv::SSTableReader::open(sst_path, 1);
    for (int i = 0; i < 1000; ++i) {
        auto res = reader->get("nonexistent_" + std::to_string(i), /*use_bloom=*/true);
        ASSERT_TRUE(!res.has_value());
    }

    // Nearly all 1,000 missing queries must be stopped by Bloom filter with 0 disk I/O.
    ASSERT_TRUE(reader->bloom_hits() >= 970);
    ASSERT_TRUE(reader->disk_reads() <= 30);

    fs::remove(sst_path, ec);
}

void test_forgekv_flushes_overwrites_deletes_and_scan() {
    cleanup_dir(TEST_DB_DIR);

    forgekv::ForgeKV db;
    // Small flush threshold (2 KB) to force multiple SSTables quickly.
    db.open(TEST_DB_DIR, 2048);

    // Phase 1: Insert 300 keys (forces several SSTable flushes & auto-compaction).
    for (int i = 0; i < 300; ++i) {
        db.put(pad_key(i), "v1_" + std::string(40, 'a' + (i % 26)));
    }
    auto st = db.stats();
    ASSERT_TRUE(st.sstable_count >= 1 && st.sstable_count < 4);

    // Phase 2: Overwrite even keys and delete every 5th key (mix of flushed + MemTable).
    for (int i = 0; i < 300; ++i) {
        if (i % 5 == 0) {
            db.del(pad_key(i));
        } else if (i % 2 == 0) {
            db.put(pad_key(i), "v2_" + std::to_string(i));
        }
    }

    // Point lookup verification.
    for (int i = 0; i < 300; ++i) {
        auto val = db.get(pad_key(i));
        if (i % 5 == 0) {
            ASSERT_TRUE(!val.has_value());
        } else if (i % 2 == 0) {
            ASSERT_TRUE(val.has_value() && *val == "v2_" + std::to_string(i));
        } else {
            ASSERT_TRUE(val.has_value() &&
                        *val == "v1_" + std::string(40, 'a' + (i % 26)));
        }
    }

    // Deduplicated range scan must omit tombstones and reflect newest overwrites.
    auto range = db.scan(pad_key(10), pad_key(25));
    // Keys 10..25 => 16 keys minus multiples of 5 (10, 15, 20, 25 => 4) = 12 keys.
    ASSERT_TRUE(range.size() == 12);
    ASSERT_TRUE(range.front().first == pad_key(11));
    ASSERT_TRUE(range[1].first == pad_key(12));
    ASSERT_TRUE(range[1].second == "v2_12");
    ASSERT_TRUE(range.back().first == pad_key(24));

    db.close();
}

void test_restart_and_crash_recovery() {
    // Reopen the DB populated in the previous test, plus simulate a leftover .tmp & torn WAL.
    const std::string leftover_tmp = TEST_DB_DIR + "/sst_999999.sst.tmp";
    {
        std::ofstream tmp(leftover_tmp, std::ios::binary);
        tmp << "incomplete_crash_garbage";
    }
    {
        forgekv::ForgeKV db;
        db.open(TEST_DB_DIR, 2048);
        db.put("wal_only_key", "survived_restart");
        db.del("k_000001");
        db.close();
    }

    // Append torn WAL bytes at EOF to simulate power loss mid-write.
    {
        std::ofstream wal_corrupt(TEST_DB_DIR + "/wal.log",
                                  std::ios::binary | std::ios::app);
        wal_corrupt.write("\xDE\xAD\xBE\xEF\x00\x05", 6);
    }

    forgekv::ForgeKV reopened;
    reopened.open(TEST_DB_DIR, 2048);
    ASSERT_TRUE(!fs::exists(leftover_tmp));

    ASSERT_TRUE(reopened.get("wal_only_key") == std::optional<std::string>("survived_restart"));
    ASSERT_TRUE(!reopened.get("k_000001").has_value());
    ASSERT_TRUE(!reopened.get(pad_key(15)).has_value());
    ASSERT_TRUE(reopened.get(pad_key(12)) == std::optional<std::string>("v2_12"));

    reopened.close();
    cleanup_dir(TEST_DB_DIR);
}

void test_concurrent_readers_and_writer() {
    cleanup_dir(TEST_DB_DIR);
    forgekv::ForgeKV db;
    db.open(TEST_DB_DIR, 4096);

    for (int i = 0; i < 200; ++i) {
        db.put(pad_key(i), "init_" + std::to_string(i));
    }

    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 200; i < 1000; ++i) {
            db.put(pad_key(i), "thread_" + std::to_string(i));
            if (i % 10 == 0) {
                db.del(pad_key(i - 50));
            }
        }
        writer_done.store(true);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r]() {
            int iter = 0;
            while (!writer_done.load() || iter < 200) {
                const int target = (iter * 7 + r) % 1000;
                auto res = db.get(pad_key(target));
                if (res.has_value()) {
                    ASSERT_TRUE(!res->empty());
                }
                if (iter % 50 == 0) {
                    auto slice = db.scan(pad_key(0), pad_key(50));
                    ASSERT_TRUE(slice.size() <= 51);
                }
                ++iter;
            }
        });
    }

    writer.join();
    for (auto& t : readers) {
        t.join();
    }

    ASSERT_TRUE(db.get(pad_key(999)) == std::optional<std::string>("thread_999"));
    ASSERT_TRUE(!db.get(pad_key(990 - 50)).has_value());

    db.close();
    cleanup_dir(TEST_DB_DIR);
}

}  // namespace

int main() {
    fs::create_directories("data");
    test_bloom_filter_accuracy_and_serialization();
    test_sstable_bloom_gate_avoids_disk_reads();
    test_forgekv_flushes_overwrites_deletes_and_scan();
    test_restart_and_crash_recovery();
    test_concurrent_readers_and_writer();
    std::cout << "[PASS] Stage 4: All Bloom Filter & ForgeKV engine tests passed!\n";
    return 0;
}
