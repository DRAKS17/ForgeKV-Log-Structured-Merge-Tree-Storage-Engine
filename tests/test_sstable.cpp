#include "memtable.h"
#include "sstable.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

const std::string TEST_SST_PATH = "data/test_stage3.sst";

std::string pad_key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%06d", i);
    return std::string(buf);
}

void cleanup_sst() {
    std::error_code ec;
    fs::remove(TEST_SST_PATH, ec);
    fs::remove(TEST_SST_PATH + ".tmp", ec);
}

void test_5000_keys_sparse_index_and_tombstones() {
    cleanup_sst();

    forgekv::MemTable mt;
    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        if (i % 10 == 0) {
            mt.del(pad_key(i));
        } else {
            mt.put(pad_key(i), "value_" + std::to_string(i));
        }
    }

    forgekv::SSTableBuilder::build_from_memtable(TEST_SST_PATH, mt);
    ASSERT_TRUE(fs::exists(TEST_SST_PATH));
    ASSERT_TRUE(!fs::exists(TEST_SST_PATH + ".tmp"));

    auto reader = forgekv::SSTableReader::open(TEST_SST_PATH, /*sequence=*/7);
    ASSERT_TRUE(reader->sequence_number() == 7);
    // 5000 entries sampled every 16th key => ceil(5000 / 16) = 313 sparse entries.
    ASSERT_TRUE(reader->sparse_index_size() == (5000 + 15) / 16);

    // Verify every single key (present & tombstone) via binary search + bounded read.
    for (int i = 0; i < N; ++i) {
        auto res = reader->get(pad_key(i));
        ASSERT_TRUE(res.has_value());
        if (i % 10 == 0) {
            ASSERT_TRUE(res->is_deleted);
            ASSERT_TRUE(res->value.empty());
        } else {
            ASSERT_TRUE(!res->is_deleted);
            ASSERT_TRUE(res->value == "value_" + std::to_string(i));
        }
    }

    // Verify missing keys: before min, between keys, and after max.
    ASSERT_TRUE(!reader->get("aaa_before_first").has_value());
    ASSERT_TRUE(!reader->get("key_000100_between").has_value());
    ASSERT_TRUE(!reader->get("zzz_after_last").has_value());

    // Range scan across multiple sparse blocks.
    auto range = reader->scan(pad_key(150), pad_key(200));
    ASSERT_TRUE(range.size() == 51);
    ASSERT_TRUE(range.front().first == pad_key(150));
    ASSERT_TRUE(range.front().second.is_deleted);  // 150 % 10 == 0
    ASSERT_TRUE(range.back().first == pad_key(200));
    ASSERT_TRUE(reader->scan(pad_key(300), pad_key(100)).empty());

    // Streaming iterator checks all 5000 entries in exact sorted order.
    auto it = reader->new_iterator();
    int seen = 0;
    while (it->valid()) {
        ASSERT_TRUE(it->key() == pad_key(seen));
        ++seen;
        it->next();
    }
    ASSERT_TRUE(seen == N);

    cleanup_sst();
}

void test_corrupted_magic_rejected() {
    cleanup_sst();
    forgekv::MemTable mt;
    mt.put("hello", "world");
    forgekv::SSTableBuilder::build_from_memtable(TEST_SST_PATH, mt);

    // Corrupt the last byte of the 36-byte footer magic number.
    {
        std::fstream f(TEST_SST_PATH, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(-1, std::ios::end);
        char b = 0;
        f.read(&b, 1);
        f.seekp(-1, std::ios::end);
        b ^= 0xFF;
        f.write(&b, 1);
    }

    bool threw = false;
    try {
        auto reader = forgekv::SSTableReader::open(TEST_SST_PATH);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    cleanup_sst();
}

void test_binary_payload_and_empty_sstable() {
    cleanup_sst();

    // Empty MemTable produces a valid empty SSTable without crashing.
    forgekv::MemTable empty_mt;
    forgekv::SSTableBuilder::build_from_memtable(TEST_SST_PATH, empty_mt);
    auto empty_reader = forgekv::SSTableReader::open(TEST_SST_PATH);
    ASSERT_TRUE(empty_reader->sparse_index_size() == 0);
    ASSERT_TRUE(!empty_reader->get("anything").has_value());
    ASSERT_TRUE(empty_reader->scan("a", "z").empty());

    // Binary CRLF / null-byte safety.
    forgekv::MemTable bin_mt;
    std::string k = std::string("bin\x00\x0A\x0D", 6);
    std::string v = std::string("\x0D\x0A\x00\xFE", 4);
    bin_mt.put(k, v);
    forgekv::SSTableBuilder::build_from_memtable(TEST_SST_PATH, bin_mt);
    auto bin_reader = forgekv::SSTableReader::open(TEST_SST_PATH);
    auto got = bin_reader->get(k);
    ASSERT_TRUE(got.has_value() && !got->is_deleted && got->value == v);

    cleanup_sst();
}

}  // namespace

int main() {
    fs::create_directories("data");
    test_5000_keys_sparse_index_and_tombstones();
    test_corrupted_magic_rejected();
    test_binary_payload_and_empty_sstable();
    std::cout << "[PASS] Stage 3: All SSTable & Sparse Index tests passed!\n";
    return 0;
}
