#include "compaction.h"
#include "forgekv.h"
#include "memtable.h"
#include "sstable.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
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

std::string pad_key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k_%06d", i);
    return std::string(buf);
}

void cleanup_dir(const std::string& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void test_kway_merge_tiebreak_and_tombstone_rules() {
    const std::string dir = "data/test_stage5_unit";
    cleanup_dir(dir);
    fs::create_directories(dir);

    // SSTable 1 (oldest, seq=1): a=1, b=2, c=3, d=4
    {
        forgekv::MemTable mt;
        mt.put("a", "v1");
        mt.put("b", "v1");
        mt.put("c", "v1");
        mt.put("d", "v1");
        forgekv::SSTableBuilder::build_from_memtable(dir + "/sst_1.sst", mt);
    }
    // SSTable 2 (middle, seq=2): b=v2, c=TOMBSTONE
    {
        forgekv::MemTable mt;
        mt.put("b", "v2");
        mt.del("c");
        forgekv::SSTableBuilder::build_from_memtable(dir + "/sst_2.sst", mt);
    }
    // SSTable 3 (newest, seq=3): d=TOMBSTONE, e=v3
    {
        forgekv::MemTable mt;
        mt.del("d");
        mt.put("e", "v3");
        forgekv::SSTableBuilder::build_from_memtable(dir + "/sst_3.sst", mt);
    }

    // Intentionally push readers out-of-order so heap MUST rely on sequence_number.
    std::vector<std::unique_ptr<forgekv::SSTableReader>> inputs;
    inputs.push_back(forgekv::SSTableReader::open(dir + "/sst_2.sst", 2));
    inputs.push_back(forgekv::SSTableReader::open(dir + "/sst_1.sst", 1));
    inputs.push_back(forgekv::SSTableReader::open(dir + "/sst_3.sst", 3));

    // Case A: Non-bottom compaction MUST preserve tombstones c and d.
    const std::string partial_out = dir + "/partial.sst";
    ASSERT_TRUE(forgekv::CompactionManager::compact(inputs, partial_out, false));
    auto partial = forgekv::SSTableReader::open(partial_out, 10);
    ASSERT_TRUE(partial->get("a")->value == "v1");
    ASSERT_TRUE(partial->get("b")->value == "v2");
    ASSERT_TRUE(partial->get("c")->is_deleted);
    ASSERT_TRUE(partial->get("d")->is_deleted);
    ASSERT_TRUE(partial->get("e")->value == "v3");

    // Case B: Bottom-level compaction MUST purge tombstones c and d completely.
    const std::string bottom_out = dir + "/bottom.sst";
    ASSERT_TRUE(forgekv::CompactionManager::compact(inputs, bottom_out, true));
    auto bottom = forgekv::SSTableReader::open(bottom_out, 11);
    ASSERT_TRUE(bottom->get("a")->value == "v1");
    ASSERT_TRUE(bottom->get("b")->value == "v2");
    ASSERT_TRUE(!bottom->get("c").has_value());
    ASSERT_TRUE(!bottom->get("d").has_value());
    ASSERT_TRUE(bottom->get("e")->value == "v3");

    cleanup_dir(dir);
}

void test_auto_compaction_and_space_reclamation() {
    const std::string dir = "data/test_stage5_db";
    cleanup_dir(dir);

    forgekv::ForgeKV db;
    // 4 KB flush threshold, auto-compact at 4 SSTables.
    db.open(dir, 4096, 4);

    // Write 1,000 keys (triggers multiple flushes & auto-compactions).
    for (int i = 0; i < 1000; ++i) {
        db.put(pad_key(i), std::string(120, 'A' + (i % 26)));
    }
    auto st_mid = db.stats();
    ASSERT_TRUE(st_mid.sstable_count < 4);

    // Overwrite 25% and delete 25% (50% churn) with high auto-compact threshold first
    // so we can measure disk reclamation across multiple uncompacted SSTables.
    db.close();
    db.open(dir, 4096, 100);
    for (int i = 0; i < 500; ++i) {
        if (i % 2 == 0) {
            db.del(pad_key(i));
        } else {
            db.put(pad_key(i), std::string(120, 'z'));
        }
    }
    const uint64_t bytes_before = db.stats().total_disk_bytes;
    ASSERT_TRUE(db.stats().sstable_count >= 2);

    db.compact();
    const uint64_t bytes_after = db.stats().total_disk_bytes;
    ASSERT_TRUE(db.stats().sstable_count == 1);
    ASSERT_TRUE(bytes_after < bytes_before);

    // Verify correctness of all 1,000 keys after compaction & restart.
    db.close();
    db.open(dir, 4096, 4);
    for (int i = 0; i < 1000; ++i) {
        auto res = db.get(pad_key(i));
        if (i < 500 && i % 2 == 0) {
            ASSERT_TRUE(!res.has_value());
        } else if (i < 500) {
            ASSERT_TRUE(res.has_value() && *res == std::string(120, 'z'));
        } else {
            ASSERT_TRUE(res.has_value() && *res == std::string(120, 'A' + (i % 26)));
        }
    }

    db.close();
    cleanup_dir(dir);
}

}  // namespace

int main() {
    fs::create_directories("data");
    test_kway_merge_tiebreak_and_tombstone_rules();
    test_auto_compaction_and_space_reclamation();
    std::cout << "[PASS] Stage 5: All Compaction & Space Reclamation tests passed!\n";
    return 0;
}
