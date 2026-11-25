#include "sstable.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace forgekv {

namespace {
constexpr std::size_t DATA_RECORD_HEADER_SIZE = 9;  // op(1) + key_len(4) + val_len(4)
constexpr std::size_t FOOTER_SIZE = 36;             // 4*uint64_t + uint32_t magic
}  // namespace

// -----------------------------------------------------------------------------
// SSTableBuilder
// -----------------------------------------------------------------------------

void SSTableBuilder::build_from_memtable(const std::string& filepath,
                                         const MemTable& memtable) {
    build_from_entries(filepath, memtable.all_entries());
}

void SSTableBuilder::build_from_entries(
    const std::string& filepath,
    const std::vector<std::pair<std::string, ValueEntry>>& entries) {
    namespace fs = std::filesystem;

    const std::string tmp_path = filepath + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to open temp SSTable file: " + tmp_path);
    }

    std::vector<SparseIndexEntry> sparse_index;
    uint64_t current_offset = 0;

    // 1. Write Data Block sequentially and sample every 16th key into Sparse Index.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& key = entries[i].first;
        const ValueEntry& val_entry = entries[i].second;
        if (key.empty()) {
            throw std::invalid_argument("SSTable cannot store an empty key");
        }

        if (i % SPARSE_INDEX_INTERVAL == 0) {
            sparse_index.push_back(SparseIndexEntry{key, current_offset});
        }

        const uint8_t op = static_cast<uint8_t>(
            val_entry.is_deleted ? OpType::DELETE : OpType::PUT);
        const uint32_t key_len = static_cast<uint32_t>(key.size());
        const uint32_t val_len = val_entry.is_deleted
                                     ? 0U
                                     : static_cast<uint32_t>(val_entry.value.size());

        char rec_hdr[DATA_RECORD_HEADER_SIZE];
        rec_hdr[0] = static_cast<char>(op);
        encode_u32(rec_hdr + 1, key_len);
        encode_u32(rec_hdr + 5, val_len);

        out.write(rec_hdr, DATA_RECORD_HEADER_SIZE);
        out.write(key.data(), static_cast<std::streamsize>(key_len));
        if (val_len > 0) {
            out.write(val_entry.value.data(), static_cast<std::streamsize>(val_len));
        }

        current_offset += DATA_RECORD_HEADER_SIZE + key_len + val_len;
    }

    // 2. Write Sparse Index Block.
    const uint64_t index_offset = current_offset;
    for (const auto& idx_entry : sparse_index) {
        const uint32_t key_len = static_cast<uint32_t>(idx_entry.key.size());
        char len_buf[4];
        encode_u32(len_buf, key_len);
        char off_buf[8];
        encode_u64(off_buf, idx_entry.offset);

        out.write(len_buf, 4);
        out.write(idx_entry.key.data(), static_cast<std::streamsize>(key_len));
        out.write(off_buf, 8);

        current_offset += 4 + key_len + 8;
    }
    const uint64_t index_size = current_offset - index_offset;

    // 3. Bloom Filter section (populated in Stage 4; 0 bytes for now).
    const uint64_t bloom_offset = 0;
    const uint64_t bloom_size = 0;

    // 4. Write fixed 36-byte Footer.
    char footer[FOOTER_SIZE];
    encode_u64(footer + 0, index_offset);
    encode_u64(footer + 8, index_size);
    encode_u64(footer + 16, bloom_offset);
    encode_u64(footer + 24, bloom_size);
    encode_u32(footer + 32, SSTABLE_MAGIC);

    out.write(footer, FOOTER_SIZE);
    out.flush();
    if (!out.good()) {
        out.close();
        std::error_code ec;
        fs::remove(tmp_path, ec);
        throw std::runtime_error("Failed writing SSTable data to: " + tmp_path);
    }
    out.close();

    // Crash-safe atomic swap: replace target only after .tmp is completely written.
    std::error_code ec;
    fs::rename(tmp_path, filepath, ec);
    if (ec) {
        // Fallback if target already exists on platforms where rename does not overwrite.
        fs::remove(filepath, ec);
        ec.clear();
        fs::rename(tmp_path, filepath, ec);
        if (ec) {
            throw std::runtime_error("Failed renaming temp SSTable to: " + filepath);
        }
    }
}

// -----------------------------------------------------------------------------
// SSTableReader::Iterator
// -----------------------------------------------------------------------------

SSTableReader::Iterator::Iterator(const std::string& filepath,
                                  uint64_t data_end_offset)
    : stream_(filepath, std::ios::binary | std::ios::in),
      offset_(0),
      data_end_offset_(data_end_offset) {
    if (!stream_.is_open()) {
        throw std::runtime_error("Failed to open SSTable for iteration: " + filepath);
    }
    read_current_record();
}

void SSTableReader::Iterator::read_current_record() {
    if (offset_ >= data_end_offset_) {
        valid_ = false;
        return;
    }

    char hdr[DATA_RECORD_HEADER_SIZE];
    if (!stream_.read(hdr, DATA_RECORD_HEADER_SIZE)) {
        valid_ = false;
        return;
    }

    const uint8_t raw_op = static_cast<uint8_t>(hdr[0]);
    const uint32_t key_len = decode_u32(hdr + 1);
    const uint32_t val_len = decode_u32(hdr + 5);

    current_key_.resize(key_len);
    if (key_len > 0 &&
        !stream_.read(&current_key_[0], static_cast<std::streamsize>(key_len))) {
        valid_ = false;
        return;
    }

    current_entry_.is_deleted = (raw_op == static_cast<uint8_t>(OpType::DELETE));
    if (val_len > 0) {
        current_entry_.value.resize(val_len);
        if (!stream_.read(&current_entry_.value[0],
                          static_cast<std::streamsize>(val_len))) {
            valid_ = false;
            return;
        }
    } else {
        current_entry_.value.clear();
    }

    offset_ += DATA_RECORD_HEADER_SIZE + key_len + val_len;
    valid_ = true;
}

void SSTableReader::Iterator::next() {
    if (!valid_) {
        return;
    }
    read_current_record();
}

void SSTableReader::Iterator::seek(uint64_t start_offset,
                                   const std::string& target_key) {
    stream_.clear();
    offset_ = start_offset;
    stream_.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
    read_current_record();
    while (valid_ && current_key_ < target_key) {
        next();
    }
}

// -----------------------------------------------------------------------------
// SSTableReader
// -----------------------------------------------------------------------------

SSTableReader::SSTableReader(std::string filepath,
                             uint64_t sequence_number,
                             uint64_t file_size,
                             uint64_t index_offset,
                             std::vector<SparseIndexEntry> sparse_index)
    : filepath_(std::move(filepath)),
      sequence_number_(sequence_number),
      file_size_(file_size),
      index_offset_(index_offset),
      sparse_index_(std::move(sparse_index)) {}

std::unique_ptr<SSTableReader> SSTableReader::open(const std::string& filepath,
                                                   uint64_t sequence_number) {
    namespace fs = std::filesystem;
    if (!fs::exists(filepath)) {
        throw std::runtime_error("SSTable file does not exist: " + filepath);
    }

    const uint64_t file_size = fs::file_size(filepath);
    if (file_size < FOOTER_SIZE) {
        throw std::runtime_error("Corrupted SSTable (smaller than 36B footer): " +
                                 filepath);
    }

    std::ifstream in(filepath, std::ios::binary | std::ios::in);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open SSTable: " + filepath);
    }

    // Read and validate 36-byte footer.
    in.seekg(static_cast<std::streamoff>(file_size - FOOTER_SIZE), std::ios::beg);
    char footer[FOOTER_SIZE];
    if (!in.read(footer, FOOTER_SIZE)) {
        throw std::runtime_error("Failed reading SSTable footer: " + filepath);
    }

    const uint64_t index_offset = decode_u64(footer + 0);
    const uint64_t index_size = decode_u64(footer + 8);
    // bloom_offset (+16) and bloom_size (+24) will be loaded in Stage 4.
    const uint32_t magic = decode_u32(footer + 32);

    if (magic != SSTABLE_MAGIC) {
        throw std::runtime_error("Invalid SSTable magic number in: " + filepath);
    }
    if (index_offset + index_size > file_size - FOOTER_SIZE) {
        throw std::runtime_error("Corrupted SSTable sparse index bounds in: " +
                                 filepath);
    }

    // Load Sparse Index into RAM.
    std::vector<SparseIndexEntry> sparse_index;
    if (index_size > 0) {
        std::vector<char> idx_buf(static_cast<std::size_t>(index_size));
        in.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
        if (!in.read(idx_buf.data(), static_cast<std::streamsize>(index_size))) {
            throw std::runtime_error("Failed reading Sparse Index from: " + filepath);
        }

        std::size_t pos = 0;
        while (pos < idx_buf.size()) {
            if (pos + 4 > idx_buf.size()) {
                throw std::runtime_error("Corrupted Sparse Index key_len in: " +
                                         filepath);
            }
            const uint32_t klen = decode_u32(idx_buf.data() + pos);
            pos += 4;
            if (pos + klen + 8 > idx_buf.size()) {
                throw std::runtime_error("Corrupted Sparse Index entry in: " +
                                         filepath);
            }
            std::string key(idx_buf.data() + pos, klen);
            pos += klen;
            const uint64_t data_off = decode_u64(idx_buf.data() + pos);
            pos += 8;
            sparse_index.push_back(SparseIndexEntry{std::move(key), data_off});
        }
    }

    return std::unique_ptr<SSTableReader>(new SSTableReader(
        filepath, sequence_number, file_size, index_offset, std::move(sparse_index)));
}

uint64_t SSTableReader::find_start_offset(const std::string& key,
                                          std::size_t* out_idx) const {
    if (sparse_index_.empty()) {
        if (out_idx) {
            *out_idx = 0;
        }
        return 0;
    }

    // Find first sparse entry whose key is strictly greater than `key`.
    auto it = std::upper_bound(
        sparse_index_.begin(), sparse_index_.end(), key,
        [](const std::string& target, const SparseIndexEntry& entry) {
            return target < entry.key;
        });

    std::size_t idx = 0;
    if (it != sparse_index_.begin()) {
        idx = static_cast<std::size_t>((it - sparse_index_.begin()) - 1);
    }
    if (out_idx) {
        *out_idx = idx;
    }
    return sparse_index_[idx].offset;
}

std::optional<ValueEntry> SSTableReader::get(const std::string& key) const {
    if (key.empty() || sparse_index_.empty()) {
        return std::nullopt;
    }
    if (key < sparse_index_.front().key) {
        return std::nullopt;
    }

    std::size_t idx = 0;
    const uint64_t start_offset = find_start_offset(key, &idx);
    const uint64_t end_offset = (idx + 1 < sparse_index_.size())
                                    ? sparse_index_[idx + 1].offset
                                    : index_offset_;

    if (end_offset <= start_offset) {
        return std::nullopt;
    }

    const uint64_t window_size = end_offset - start_offset;
    std::ifstream in(filepath_, std::ios::binary | std::ios::in);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open SSTable for read: " + filepath_);
    }

    in.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
    std::vector<char> block(static_cast<std::size_t>(window_size));
    if (!in.read(block.data(), static_cast<std::streamsize>(window_size))) {
        throw std::runtime_error("Failed reading SSTable block: " + filepath_);
    }
    disk_reads_.fetch_add(1);

    std::size_t pos = 0;
    while (pos + DATA_RECORD_HEADER_SIZE <= block.size()) {
        const uint8_t raw_op = static_cast<uint8_t>(block[pos]);
        const uint32_t klen = decode_u32(block.data() + pos + 1);
        const uint32_t vlen = decode_u32(block.data() + pos + 5);
        pos += DATA_RECORD_HEADER_SIZE;

        if (pos + klen + vlen > block.size()) {
            break;
        }

        // Compare key directly before allocating value if it does not match.
        const int cmp = std::string_view(block.data() + pos, klen).compare(key);
        if (cmp == 0) {
            ValueEntry entry;
            entry.is_deleted = (raw_op == static_cast<uint8_t>(OpType::DELETE));
            if (!entry.is_deleted && vlen > 0) {
                entry.value.assign(block.data() + pos + klen, vlen);
            }
            return entry;
        }
        if (cmp > 0) {
            // Data Block is strictly sorted, so `key` cannot appear later.
            return std::nullopt;
        }
        pos += klen + vlen;
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, ValueEntry>> SSTableReader::scan(
    const std::string& start_key, const std::string& end_key) const {
    std::vector<std::pair<std::string, ValueEntry>> results;
    if (start_key > end_key || sparse_index_.empty()) {
        return results;
    }

    auto it = new_iterator_at(start_key);
    while (it->valid() && it->key() <= end_key) {
        results.emplace_back(it->key(), it->entry());
        it->next();
    }
    return results;
}

std::unique_ptr<SSTableReader::Iterator> SSTableReader::new_iterator() const {
    return std::make_unique<Iterator>(filepath_, index_offset_);
}

std::unique_ptr<SSTableReader::Iterator> SSTableReader::new_iterator_at(
    const std::string& start_key) const {
    auto it = std::make_unique<Iterator>(filepath_, index_offset_);
    if (!sparse_index_.empty()) {
        const uint64_t start_offset = find_start_offset(start_key);
        it->seek(start_offset, start_key);
    }
    return it;
}

}  // namespace forgekv
