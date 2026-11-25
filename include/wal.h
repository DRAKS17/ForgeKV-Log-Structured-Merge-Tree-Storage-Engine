#pragma once

#include "memtable.h"

#include <cstddef>
#include <fstream>
#include <string>

namespace forgekv {

/**
 * Append-only binary Write-Ahead Log (WAL) writer.
 *
 * Every record uses fixed-width little-endian framing:
 *   [crc32 (4B) | op_type (1B) | key_len (4B) | val_len (4B) | key | value]
 * where crc32 is computed over [op_type .. value].
 */
class WALWriter {
public:
    explicit WALWriter(const std::string& filepath);
    ~WALWriter();

    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;

    /// Append a PUT record and flush to the OS page cache.
    void append_put(const std::string& key, const std::string& value);

    /// Append a DELETE tombstone record (val_len = 0) and flush.
    void append_del(const std::string& key);

    /// Truncate the WAL to 0 bytes (called after MemTable flushes to an SSTable).
    void truncate();

    /// Flush and close the underlying file stream.
    void close();

    const std::string& filepath() const { return filepath_; }

private:
    void append_frame(OpType op, const std::string& key, const std::string& value);
    void open_append();

    std::string filepath_;
    std::ofstream stream_;
};

/**
 * Crash-safe WAL recovery reader.
 *
 * Replays all valid checksummed records into the target MemTable. If a crash
 * left a torn or corrupted trailing record, the corrupted tail is truncated.
 */
class WALReader {
public:
    /// Recover valid records from `filepath` into `memtable`. Returns record count.
    static std::size_t recover(const std::string& filepath, MemTable& memtable);
};

}  // namespace forgekv
