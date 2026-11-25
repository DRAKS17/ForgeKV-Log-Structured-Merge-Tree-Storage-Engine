# ForgeKV — Log-Structured Merge-Tree (LSM) Storage Engine

[![ForgeKV CI](https://github.com/DRAKS17/ForgeKV-Log-Structured-Merge-Tree-Storage-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/DRAKS17/ForgeKV-Log-Structured-Merge-Tree-Storage-Engine/actions/workflows/ci.yml)

**ForgeKV** is an embedded, thread-safe **Log-Structured Merge-Tree (LSM-Tree)** key-value storage engine built from scratch in **modern C++17** using **only the C++ Standard Library (zero external dependencies)**.

It implements the core storage architecture used in production engines such as RocksDB, LevelDB, and Cassandra: sequential Write-Ahead Logging (WAL) with IEEE 802.3 CRC32 crash recovery, a probabilistic SkipList MemTable, immutable disk SSTables with Sparse Indexes, Kirsch-Mitzenmacher double-hashed Bloom filters, and Min-Heap K-Way Merge compaction.

---

## Architecture

### Write & Compaction Path
```
Client (PUT / DEL)
   │
   ▼
[ std::shared_mutex (Exclusive Writer Lock) ]
   │
   ├──► 1. Append CRC32 binary frame ──► wal.log (Sequential Disk Append)
   │
   └──► 2. Insert / Tombstone O(log N) ─► SkipList MemTable (RAM)
                                              │
                             (size >= flush_threshold)
                                              ▼
                                 Atomic Flush (.tmp -> rename)
                                              │
                                              ▼
                                 Immutable sst_XXXXXX.sst
                                 (Data + Sparse Index + Bloom + 36B Footer)
                                              │
                             (SSTable count >= 4)
                                              ▼
                           Min-Heap K-Way Merge Compaction
                           (Deduplicate keys, purge bottom tombstones)
```

### Read Path (`GET` / `SCAN`)
```
Client (GET key)
   │
   ▼
[ std::shared_mutex (Shared Concurrent Reader Lock) ]
   │
   ├──► 1. Check Active SkipList MemTable (RAM)
   │         ├─ Found live value  ──► Return value
   │         └─ Found tombstone   ──► Return nullopt (stop)
   │
   └──► 2. Probe SSTables (Newest ──► Oldest)
             ├─ BloomFilter.possibly_contains(key) == false ──► Skip disk I/O
             └─ Might exist ──► Binary Search Sparse Index (every 16th key)
                                     │
                                     ▼
                                1 Bounded Sequential Disk Read
```

---

## Key Features & Engineering Highlights

* **Zero External Dependencies:** SkipList, IEEE 802.3 CRC32, FNV-1a/Murmur double-hashed Bloom filter, little-endian binary framing, K-way compactor, CLI, test suite, and benchmark harness are all written in pure C++17 STL.
* **Crash-Safe Durability:**
  * Every mutation is written to `wal.log` with a CRC32 checksum before entering the MemTable.
  * On restart, `WALReader::recover` validates frame lengths and checksums and cleanly truncates torn/corrupted trailing records from mid-write crashes.
  * SSTables are written to `.tmp` files and atomically swapped in via `std::filesystem::rename`.
* **Thread-Safe Concurrency:** Uses `std::shared_mutex` so multiple reader threads (`get`, `scan`) run concurrently while writers (`put`, `del`, `compact`) take exclusive locks.
* **Tombstone Semantics & Bottom-Level Purge:** `del(key)` writes an explicit tombstone marker (`OpType::DELETE`) so deleted keys never resurrect from older SSTables, and tombstones are purged only when compacting all SSTables down to the bottom level.

---

## Binary Disk Formats

### 1. Write-Ahead Log (`wal.log`) Frame (Little-Endian)
| Field | Size | Description |
| :--- | :--- | :--- |
| `crc32` | 4 B | IEEE 802.3 CRC32 over `[op_type .. val_bytes]` |
| `op_type` | 1 B | `0x00` = `PUT`, `0x01` = `DELETE` (tombstone) |
| `key_len` | 4 B | Byte length of key (`> 0`) |
| `val_len` | 4 B | Byte length of value (`0` for `DELETE`) |
| `key_bytes` | `key_len` B | Raw key bytes (binary-safe via `std::ios::binary`) |
| `val_bytes` | `val_len` B | Raw value bytes |

### 2. Immutable SSTable (`.sst`) 4-Section Layout
1. **Data Block:** Sorted records `[op_type (1B) | key_len (4B) | val_len (4B) | key | value]`
2. **Sparse Index:** Every 16th key `[key_len (4B) | key | data_file_offset (8B)]`
3. **Bloom Filter:** `[num_bits (4B) | num_hashes k (4B) | bitset_bytes]`
4. **Fixed 36-Byte Footer:** `[index_offset (8B) | index_size (8B) | bloom_offset (8B) | bloom_size (8B) | magic (4B: 0x464B5631)]`

---

## Measured Benchmark Results

Measured with `benchmarks/bench_forgekv.cpp` compiled with `g++ -std=c++17 -O3` on Windows (MinGW-w64):

```text
==================================================================
                   ForgeKV Benchmark Summary
==================================================================
Sequential Write (100k keys)  : 126810.39 ops/sec (14.15 MB/sec)
Random Write (100k keys)      : 126370.89 ops/sec
Point Read Latency (20k ops)  : p50 = 26.40 us | p95 = 39.90 us | p99 = 73.10 us
Negative Lookups (50k keys)   :
  - Bloom OFF                 : 2404.06 ms (100000 disk reads)
  - Bloom ON                  : 89.81 ms (1067 disk reads)
  - Savings & Speedup         : 98.93% disk reads avoided (26.77x faster)
Compaction Space Reclamation  : 6675329 B -> 3870363 B (42.02% reclaimed)
==================================================================
```

---

## Quick Start

### Option A: Build & Run with CMake
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Option B: Direct One-Command `g++` Build
Run the benchmark suite:
```bash
g++ -std=c++17 -O3 -Wall -Wextra -pthread -Iinclude \
  src/memtable.cpp src/wal.cpp src/bloom_filter.cpp src/sstable.cpp \
  src/compaction.cpp src/forgekv.cpp benchmarks/bench_forgekv.cpp \
  -o bench_forgekv && ./bench_forgekv
```

Run the interactive CLI:
```bash
g++ -std=c++17 -O3 -Wall -Wextra -pthread -Iinclude \
  src/memtable.cpp src/wal.cpp src/bloom_filter.cpp src/sstable.cpp \
  src/compaction.cpp src/forgekv.cpp cli/forgekv_cli.cpp \
  -o forgekv_cli && ./forgekv_cli
```

Example CLI session:
```text
forgekv> PUT user:1 Daksh Shaparia
OK
forgekv> GET user:1
Daksh Shaparia
forgekv> SCAN user:0 user:9
user:1 => Daksh Shaparia
forgekv> STATS
forgekv> COMPACT
forgekv> DEL user:1
OK
forgekv> EXIT
```
