#include "wal.h"

#include "common.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace forgekv {

namespace {
constexpr std::size_t WAL_HEADER_SIZE = 13;  // crc32(4) + op(1) + key_len(4) + val_len(4)
}  // namespace

WALWriter::WALWriter(const std::string& filepath) : filepath_(filepath) {
    open_append();
}

WALWriter::~WALWriter() {
    close();
}

void WALWriter::open_append() {
    stream_.open(filepath_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
        throw std::runtime_error("Failed to open WAL file for append: " + filepath_);
    }
}

void WALWriter::append_frame(OpType op,
                             const std::string& key,
                             const std::string& value) {
    if (key.empty()) {
        throw std::invalid_argument("WAL key must not be empty");
    }
    if (!stream_.is_open()) {
        throw std::runtime_error("WALWriter stream is closed");
    }

    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t val_len = static_cast<uint32_t>(value.size());
    const std::size_t frame_size = WAL_HEADER_SIZE + key.size() + value.size();

    std::vector<char> frame(frame_size);
    frame[4] = static_cast<char>(static_cast<uint8_t>(op));
    encode_u32(frame.data() + 5, key_len);
    encode_u32(frame.data() + 9, val_len);

    if (!key.empty()) {
        std::memcpy(frame.data() + WAL_HEADER_SIZE, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(frame.data() + WAL_HEADER_SIZE + key.size(), value.data(), value.size());
    }

    // Checksum everything after the 4-byte crc32 field.
    const uint32_t checksum = crc32(frame.data() + 4, frame_size - 4);
    encode_u32(frame.data(), checksum);

    stream_.write(frame.data(), static_cast<std::streamsize>(frame_size));
    stream_.flush();
    if (!stream_.good()) {
        throw std::runtime_error("Failed writing frame to WAL: " + filepath_);
    }
}

void WALWriter::append_put(const std::string& key, const std::string& value) {
    append_frame(OpType::PUT, key, value);
}

void WALWriter::append_del(const std::string& key) {
    append_frame(OpType::DELETE, key, "");
}

void WALWriter::truncate() {
    close();
    std::ofstream trunc_stream(filepath_, std::ios::binary | std::ios::trunc);
    if (!trunc_stream.is_open()) {
        throw std::runtime_error("Failed to truncate WAL file: " + filepath_);
    }
    trunc_stream.close();
    open_append();
}

void WALWriter::close() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

std::size_t WALReader::recover(const std::string& filepath, MemTable& memtable) {
    namespace fs = std::filesystem;

    if (!fs::exists(filepath)) {
        return 0;
    }

    const uint64_t total_size = fs::file_size(filepath);
    if (total_size == 0) {
        return 0;
    }

    std::ifstream in(filepath, std::ios::binary | std::ios::in);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open WAL file for recovery: " + filepath);
    }

    uint64_t offset = 0;
    uint64_t valid_bytes_end = 0;
    std::size_t recovered_count = 0;
    bool needs_truncate = false;

    while (offset < total_size) {
        const uint64_t remaining = total_size - offset;
        if (remaining < WAL_HEADER_SIZE) {
            needs_truncate = true;
            break;
        }

        char header[WAL_HEADER_SIZE];
        if (!in.read(header, static_cast<std::streamsize>(WAL_HEADER_SIZE))) {
            needs_truncate = true;
            break;
        }

        const uint32_t expected_crc = decode_u32(header);
        const uint8_t raw_op = static_cast<uint8_t>(header[4]);
        const uint32_t key_len = decode_u32(header + 5);
        const uint32_t val_len = decode_u32(header + 9);

        const bool valid_op = (raw_op == static_cast<uint8_t>(OpType::PUT)) ||
                              (raw_op == static_cast<uint8_t>(OpType::DELETE));
        const uint64_t payload_len =
            static_cast<uint64_t>(key_len) + static_cast<uint64_t>(val_len);

        if (!valid_op || key_len == 0 ||
            (raw_op == static_cast<uint8_t>(OpType::DELETE) && val_len != 0) ||
            payload_len > (remaining - WAL_HEADER_SIZE)) {
            needs_truncate = true;
            break;
        }

        // Assemble [op_type | key_len | val_len | key | val] to verify CRC32.
        std::vector<char> verify_buf(9 + static_cast<std::size_t>(payload_len));
        std::memcpy(verify_buf.data(), header + 4, 9);

        if (!in.read(verify_buf.data() + 9, static_cast<std::streamsize>(payload_len))) {
            needs_truncate = true;
            break;
        }

        const uint32_t actual_crc = crc32(verify_buf.data(), verify_buf.size());
        if (actual_crc != expected_crc) {
            needs_truncate = true;
            break;
        }

        std::string key(verify_buf.data() + 9, key_len);
        if (raw_op == static_cast<uint8_t>(OpType::PUT)) {
            std::string value(verify_buf.data() + 9 + key_len, val_len);
            memtable.put(key, value);
        } else {
            memtable.del(key);
        }

        offset += WAL_HEADER_SIZE + payload_len;
        valid_bytes_end = offset;
        ++recovered_count;
    }

    in.close();

    if (needs_truncate && valid_bytes_end < total_size) {
        fs::resize_file(filepath, valid_bytes_end);
    }

    return recovered_count;
}

}  // namespace forgekv
