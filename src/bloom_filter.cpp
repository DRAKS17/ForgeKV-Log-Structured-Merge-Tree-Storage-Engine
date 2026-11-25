#include "bloom_filter.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace forgekv {

BloomFilter::BloomFilter(std::size_t expected_items, double false_positive_rate) {
    if (expected_items == 0) {
        return;
    }
    if (false_positive_rate <= 0.0 || false_positive_rate >= 1.0) {
        false_positive_rate = 0.01;
    }

    const double n = static_cast<double>(expected_items);
    const double ln2 = std::log(2.0);
    const double m_double = -(n * std::log(false_positive_rate)) / (ln2 * ln2);

    num_bits_ = static_cast<uint32_t>(std::max(8.0, std::ceil(m_double)));
    const double k_double = (static_cast<double>(num_bits_) / n) * ln2;
    num_hashes_ = static_cast<uint32_t>(std::max(1.0, std::round(k_double)));

    bits_.assign((num_bits_ + 7U) / 8U, 0U);
}

BloomFilter::BloomFilter(uint32_t num_bits,
                         uint32_t num_hashes,
                         std::vector<uint8_t> bits)
    : num_bits_(num_bits), num_hashes_(num_hashes), bits_(std::move(bits)) {}

uint64_t BloomFilter::hash64(const std::string& key, uint64_t seed) {
    // 64-bit FNV-1a seeded pass followed by Murmur3 avalanche finalizer.
    uint64_t h = 14695981039346656037ULL ^ seed;
    for (unsigned char c : key) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

void BloomFilter::insert(const std::string& key) {
    if (num_bits_ == 0) {
        return;
    }

    const uint64_t h1 = hash64(key, 0ULL);
    uint64_t h2 = hash64(key, 0x9E3779B97F4A7C15ULL);
    if (h2 % num_bits_ == 0) {
        h2 |= 1ULL;
    }

    for (uint32_t i = 0; i < num_hashes_; ++i) {
        const uint64_t bit_pos =
            (h1 + static_cast<uint64_t>(i) * h2) % num_bits_;
        bits_[bit_pos / 8U] |= static_cast<uint8_t>(1U << (bit_pos % 8U));
    }
}

bool BloomFilter::possibly_contains(const std::string& key) const {
    if (num_bits_ == 0) {
        return false;
    }

    const uint64_t h1 = hash64(key, 0ULL);
    uint64_t h2 = hash64(key, 0x9E3779B97F4A7C15ULL);
    if (h2 % num_bits_ == 0) {
        h2 |= 1ULL;
    }

    for (uint32_t i = 0; i < num_hashes_; ++i) {
        const uint64_t bit_pos =
            (h1 + static_cast<uint64_t>(i) * h2) % num_bits_;
        if ((bits_[bit_pos / 8U] & static_cast<uint8_t>(1U << (bit_pos % 8U))) == 0) {
            return false;
        }
    }
    return true;
}

std::vector<char> BloomFilter::serialize() const {
    std::vector<char> out(8 + bits_.size());
    encode_u32(out.data(), num_bits_);
    encode_u32(out.data() + 4, num_hashes_);
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        out[8 + i] = static_cast<char>(bits_[i]);
    }
    return out;
}

BloomFilter BloomFilter::deserialize(const char* data, std::size_t size) {
    if (size < 8) {
        throw std::runtime_error("Corrupted BloomFilter header");
    }
    const uint32_t num_bits = decode_u32(data);
    const uint32_t num_hashes = decode_u32(data + 4);
    const std::size_t expected_bytes = (num_bits == 0) ? 0 : ((num_bits + 7U) / 8U);
    if (size != 8 + expected_bytes) {
        throw std::runtime_error("Corrupted BloomFilter bitset length");
    }

    std::vector<uint8_t> bits(expected_bytes);
    for (std::size_t i = 0; i < expected_bytes; ++i) {
        bits[i] = static_cast<uint8_t>(data[8 + i]);
    }
    return BloomFilter(num_bits, num_hashes, std::move(bits));
}

}  // namespace forgekv
