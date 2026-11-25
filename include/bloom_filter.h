#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace forgekv {

/**
 * Space-efficient probabilistic membership filter using Kirsch-Mitzenmacher
 * double hashing over two 64-bit FNV-1a + Murmur-mixed hashes.
 *
 * Guarantees zero false negatives: if possibly_contains(k) returns false,
 * the key is 100% absent from the SSTable and disk I/O can be skipped.
 */
class BloomFilter {
public:
    BloomFilter() = default;
    explicit BloomFilter(std::size_t expected_items,
                         double false_positive_rate = 0.01);

    /// Add a key to the Bloom filter bitset.
    void insert(const std::string& key);

    /// Return false if key is definitely absent; true if it might be present.
    bool possibly_contains(const std::string& key) const;

    /// Serialize as [num_bits (4B) | num_hashes k (4B) | bitset_bytes].
    std::vector<char> serialize() const;

    /// Reconstruct a BloomFilter from serialized bytes.
    static BloomFilter deserialize(const char* data, std::size_t size);

    uint32_t num_bits() const { return num_bits_; }
    uint32_t num_hashes() const { return num_hashes_; }
    bool empty() const { return num_bits_ == 0; }

private:
    BloomFilter(uint32_t num_bits,
                uint32_t num_hashes,
                std::vector<uint8_t> bits);

    static uint64_t hash64(const std::string& key, uint64_t seed);

    uint32_t num_bits_ = 0;
    uint32_t num_hashes_ = 0;
    std::vector<uint8_t> bits_;
};

}  // namespace forgekv
