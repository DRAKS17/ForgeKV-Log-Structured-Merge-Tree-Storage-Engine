#include "memtable.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
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

std::string pad_key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%06d", i);
    return std::string(buf);
}

void test_basic_put_get_and_overwrite() {
    forgekv::MemTable mt;
    ASSERT_TRUE(mt.entry_count() == 0);
    ASSERT_TRUE(mt.approximate_size_bytes() == 0);
    ASSERT_TRUE(!mt.get("missing").has_value());

    mt.put("alpha", "hello");
    auto res = mt.get("alpha");
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(!res->is_deleted);
    ASSERT_TRUE(res->value == "hello");
    ASSERT_TRUE(mt.entry_count() == 1);

    std::size_t size_after_insert = mt.approximate_size_bytes();
    ASSERT_TRUE(size_after_insert > 0);

    // Overwrite with a longer value: count stays 1, byte size increases by delta.
    mt.put("alpha", "hello_world!");
    res = mt.get("alpha");
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(res->value == "hello_world!");
    ASSERT_TRUE(mt.entry_count() == 1);
    ASSERT_TRUE(mt.approximate_size_bytes() == size_after_insert + 7);
}

void test_tombstone_semantics() {
    forgekv::MemTable mt;
    mt.put("user:1", "Daksh");
    mt.del("user:1");

    // Deleted key MUST still exist in MemTable as a tombstone (not nullopt).
    auto res = mt.get("user:1");
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(res->is_deleted);
    ASSERT_TRUE(res->value.empty());

    // Deleting a key not in MemTable also writes a tombstone to shadow older disk files.
    mt.del("user:ghost");
    auto ghost = mt.get("user:ghost");
    ASSERT_TRUE(ghost.has_value());
    ASSERT_TRUE(ghost->is_deleted);

    // Re-inserting after delete clears the tombstone flag.
    mt.put("user:1", "Restored");
    res = mt.get("user:1");
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(!res->is_deleted);
    ASSERT_TRUE(res->value == "Restored");
}

void test_sequential_10k_and_scan() {
    forgekv::MemTable mt;
    constexpr int N = 10000;

    for (int i = 0; i < N; ++i) {
        mt.put(pad_key(i), "val_" + std::to_string(i));
    }
    ASSERT_TRUE(mt.entry_count() == static_cast<std::size_t>(N));

    for (int i = 0; i < N; ++i) {
        auto res = mt.get(pad_key(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(!res->is_deleted);
        ASSERT_TRUE(res->value == "val_" + std::to_string(i));
    }

    auto slice = mt.scan(pad_key(2500), pad_key(2599));
    ASSERT_TRUE(slice.size() == 100);
    ASSERT_TRUE(slice.front().first == pad_key(2500));
    ASSERT_TRUE(slice.back().first == pad_key(2599));

    // Reversed range returns empty.
    ASSERT_TRUE(mt.scan(pad_key(500), pad_key(100)).empty());
}

void test_random_10k_against_std_map() {
    forgekv::MemTable mt;
    std::map<std::string, forgekv::ValueEntry> oracle;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, 4999);
    std::uniform_int_distribution<int> op_dist(0, 9);

    constexpr int OPS = 10000;
    for (int step = 0; step < OPS; ++step) {
        std::string key = pad_key(key_dist(rng));
        int op = op_dist(rng);

        if (op < 7) {
            std::string val = "v_" + std::to_string(step);
            mt.put(key, val);
            oracle[key] = forgekv::ValueEntry{val, false};
        } else {
            mt.del(key);
            oracle[key] = forgekv::ValueEntry{"", true};
        }
    }

    ASSERT_TRUE(mt.entry_count() == oracle.size());
    for (int i = 0; i < 5000; ++i) {
        std::string key = pad_key(i);
        auto actual = mt.get(key);
        auto it = oracle.find(key);
        if (it == oracle.end()) {
            ASSERT_TRUE(!actual.has_value());
        } else {
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(actual->is_deleted == it->second.is_deleted);
            ASSERT_TRUE(actual->value == it->second.value);
        }
    }

    auto all = mt.all_entries();
    ASSERT_TRUE(all.size() == oracle.size());
    std::size_t idx = 0;
    for (const auto& [k, v] : oracle) {
        ASSERT_TRUE(all[idx].first == k);
        ASSERT_TRUE(all[idx].second.is_deleted == v.is_deleted);
        ASSERT_TRUE(all[idx].second.value == v.value);
        ++idx;
    }

    mt.clear();
    ASSERT_TRUE(mt.entry_count() == 0);
    ASSERT_TRUE(mt.approximate_size_bytes() == 0);
    ASSERT_TRUE(!mt.get(pad_key(0)).has_value());
}

void test_little_endian_helpers() {
    char buf4[4]{};
    forgekv::encode_u32(buf4, 0x464B5631U);
    ASSERT_TRUE(static_cast<uint8_t>(buf4[0]) == 0x31);
    ASSERT_TRUE(static_cast<uint8_t>(buf4[3]) == 0x46);
    ASSERT_TRUE(forgekv::decode_u32(buf4) == 0x464B5631U);

    char buf8[8]{};
    forgekv::encode_u64(buf8, 0x0123456789ABCDEFULL);
    ASSERT_TRUE(forgekv::decode_u64(buf8) == 0x0123456789ABCDEFULL);
}

}  // namespace

int main() {
    test_little_endian_helpers();
    test_basic_put_get_and_overwrite();
    test_tombstone_semantics();
    test_sequential_10k_and_scan();
    test_random_10k_against_std_map();
    std::cout << "[PASS] Stage 1: All MemTable & SkipList tests passed!\n";
    return 0;
}
