#pragma once

#include <cstdint>
#include <string>

namespace forgekv {

/// Operation code stored in WAL frames and SSTable records.
enum class OpType : uint8_t {
    PUT = 0x00,
    DELETE = 0x01
};

/// In-memory and decoded disk representation of a key's value or tombstone.
struct ValueEntry {
    std::string value;
    bool is_deleted = false;
};

/// 4-byte ASCII magic number ("FKV1") placed at the end of every .sst footer.
inline constexpr uint32_t SSTABLE_MAGIC = 0x464B5631U;

/// Number of Data Block records between consecutive Sparse Index entries.
inline constexpr uint32_t SPARSE_INDEX_INTERVAL = 16U;

/// Encode a 32-bit unsigned integer into 4 little-endian bytes.
inline void encode_u32(char* dst, uint32_t val) {
    dst[0] = static_cast<char>(val & 0xFFU);
    dst[1] = static_cast<char>((val >> 8) & 0xFFU);
    dst[2] = static_cast<char>((val >> 16) & 0xFFU);
    dst[3] = static_cast<char>((val >> 24) & 0xFFU);
}

/// Decode 4 little-endian bytes into a 32-bit unsigned integer.
inline uint32_t decode_u32(const char* src) {
    return static_cast<uint32_t>(static_cast<uint8_t>(src[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(src[3])) << 24);
}

/// Encode a 64-bit unsigned integer into 8 little-endian bytes.
inline void encode_u64(char* dst, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<char>((val >> (i * 8)) & 0xFFULL);
    }
}

/// Decode 8 little-endian bytes into a 64-bit unsigned integer.
inline uint64_t decode_u64(const char* src) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= static_cast<uint64_t>(static_cast<uint8_t>(src[i])) << (i * 8);
    }
    return val;
}

}  // namespace forgekv
