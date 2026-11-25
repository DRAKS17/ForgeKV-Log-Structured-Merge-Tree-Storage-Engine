#include "common.h"
#include "memtable.h"
#include "wal.h"

#include <cstdint>
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

const std::string TEST_WAL_PATH = "data/test_stage2_wal.log";

void cleanup_wal() {
    std::error_code ec;
    fs::remove(TEST_WAL_PATH, ec);
}

void test_crc32_standard_vector() {
    // IEEE 802.3 standard check value for ASCII "123456789" is 0xCBF43926.
    ASSERT_TRUE(forgekv::crc32("123456789") == 0xCBF43926U);
    ASSERT_TRUE(forgekv::crc32("") == 0x00000000U);
}

void test_missing_wal_recovers_zero() {
    cleanup_wal();
    forgekv::MemTable mt;
    ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, mt) == 0);
    ASSERT_TRUE(mt.entry_count() == 0);
}

void test_normal_write_and_recovery() {
    cleanup_wal();
    {
        forgekv::WALWriter writer(TEST_WAL_PATH);
        writer.append_put("k1", "value_one");
        writer.append_put("k2", "value_two");
        writer.append_put("k1", "value_one_updated");
        writer.append_del("k2");
        writer.append_put("k3", "value_three");
        writer.close();
    }

    forgekv::MemTable recovered;
    const std::size_t count = forgekv::WALReader::recover(TEST_WAL_PATH, recovered);
    ASSERT_TRUE(count == 5);
    ASSERT_TRUE(recovered.entry_count() == 3);

    auto v1 = recovered.get("k1");
    ASSERT_TRUE(v1.has_value() && !v1->is_deleted && v1->value == "value_one_updated");

    auto v2 = recovered.get("k2");
    ASSERT_TRUE(v2.has_value() && v2->is_deleted && v2->value.empty());

    auto v3 = recovered.get("k3");
    ASSERT_TRUE(v3.has_value() && !v3->is_deleted && v3->value == "value_three");
    cleanup_wal();
}

void test_binary_crlf_and_null_bytes_safe() {
    cleanup_wal();
    std::string weird_key = std::string("bin\x00key\x0A\x0D", 9);
    std::string weird_val = std::string("\x0A\x0D\x00\xFFhello\x0A", 11);

    {
        forgekv::WALWriter writer(TEST_WAL_PATH);
        writer.append_put(weird_key, weird_val);
        writer.close();
    }

    forgekv::MemTable recovered;
    ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, recovered) == 1);
    auto res = recovered.get(weird_key);
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(!res->is_deleted);
    ASSERT_TRUE(res->value == weird_val);
    cleanup_wal();
}

void test_truncate_clears_wal() {
    cleanup_wal();
    {
        forgekv::WALWriter writer(TEST_WAL_PATH);
        writer.append_put("temp_a", "123");
        writer.append_put("temp_b", "456");
        writer.truncate();
        ASSERT_TRUE(fs::file_size(TEST_WAL_PATH) == 0);

        writer.append_put("after_trunc", "fresh");
        writer.close();
    }

    forgekv::MemTable recovered;
    ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, recovered) == 1);
    ASSERT_TRUE(!recovered.get("temp_a").has_value());
    ASSERT_TRUE(recovered.get("after_trunc")->value == "fresh");
    cleanup_wal();
}

void test_torn_tail_crash_recovery() {
    cleanup_wal();

    // 1. Write 25 valid records and snapshot the valid file length.
    {
        forgekv::WALWriter writer(TEST_WAL_PATH);
        for (int i = 0; i < 25; ++i) {
            writer.append_put("key_" + std::to_string(i), "payload_" + std::to_string(i));
        }
        writer.append_del("key_5");
        writer.close();
    }
    const uint64_t valid_size = fs::file_size(TEST_WAL_PATH);

    // 2. Simulate mid-header crash (7 incomplete bytes at EOF).
    {
        std::ofstream corrupt(TEST_WAL_PATH, std::ios::binary | std::ios::app);
        const char partial_header[7] = {0x12, 0x34, 0x56, 0x78, 0x00, 0x05, 0x00};
        corrupt.write(partial_header, 7);
    }
    ASSERT_TRUE(fs::file_size(TEST_WAL_PATH) == valid_size + 7);

    {
        forgekv::MemTable recovered;
        ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, recovered) == 26);
        ASSERT_TRUE(fs::file_size(TEST_WAL_PATH) == valid_size);
        ASSERT_TRUE(recovered.get("key_5")->is_deleted);
        ASSERT_TRUE(recovered.get("key_24")->value == "payload_24");
    }

    // 3. Simulate mid-payload crash (valid-looking 13B header claiming 500 bytes, only 10 written).
    {
        std::ofstream corrupt(TEST_WAL_PATH, std::ios::binary | std::ios::app);
        char fake_hdr[13]{};
        forgekv::encode_u32(fake_hdr, 0xDEADBEEFU);
        fake_hdr[4] = 0x00;
        forgekv::encode_u32(fake_hdr + 5, 250);
        forgekv::encode_u32(fake_hdr + 9, 250);
        corrupt.write(fake_hdr, 13);
        corrupt.write("half_writn", 10);
    }

    {
        forgekv::MemTable recovered;
        ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, recovered) == 26);
        ASSERT_TRUE(fs::file_size(TEST_WAL_PATH) == valid_size);
    }

    // 4. Simulate corrupted bit inside a full-length trailing frame (CRC32 mismatch).
    {
        forgekv::WALWriter writer(TEST_WAL_PATH);
        writer.append_put("doomed_key", "doomed_value");
        writer.close();
    }
    {
        std::fstream flip(TEST_WAL_PATH, std::ios::binary | std::ios::in | std::ios::out);
        flip.seekg(-1, std::ios::end);
        char last_byte = 0;
        flip.read(&last_byte, 1);
        flip.seekp(-1, std::ios::end);
        last_byte ^= 0xFF;
        flip.write(&last_byte, 1);
    }

    {
        forgekv::MemTable recovered;
        ASSERT_TRUE(forgekv::WALReader::recover(TEST_WAL_PATH, recovered) == 26);
        ASSERT_TRUE(fs::file_size(TEST_WAL_PATH) == valid_size);
        ASSERT_TRUE(!recovered.get("doomed_key").has_value());
    }

    cleanup_wal();
}

}  // namespace

int main() {
    fs::create_directories("data");
    test_crc32_standard_vector();
    test_missing_wal_recovers_zero();
    test_normal_write_and_recovery();
    test_binary_crlf_and_null_bytes_safe();
    test_truncate_clears_wal();
    test_torn_tail_crash_recovery();
    std::cout << "[PASS] Stage 2: All WAL & CRC32 crash recovery tests passed!\n";
    return 0;
}
