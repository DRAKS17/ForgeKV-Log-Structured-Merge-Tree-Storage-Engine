cmake_minimum_required(VERSION 3.16)
project(ForgeKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
enable_testing()

# Stage 1: SkipList MemTable
add_executable(test_memtable src/memtable.cpp tests/test_memtable.cpp)
add_test(NAME Stage1_MemTable COMMAND test_memtable)

# Stage 2: Binary WAL & CRC32 Crash Recovery
add_executable(test_wal src/memtable.cpp src/wal.cpp tests/test_wal.cpp)
add_test(NAME Stage2_WAL COMMAND test_wal)

# Stage 3: Immutable SSTables & Sparse Index
add_executable(test_sstable src/memtable.cpp src/bloom_filter.cpp src/sstable.cpp tests/test_sstable.cpp)
add_test(NAME Stage3_SSTable COMMAND test_sstable)

# Stage 4: Bloom Filter & Unified ForgeKV Engine
add_executable(test_forgekv
    src/memtable.cpp
    src/wal.cpp
    src/bloom_filter.cpp
    src/sstable.cpp
    src/compaction.cpp
    src/forgekv.cpp
    tests/test_forgekv.cpp
)
target_link_libraries(test_forgekv PRIVATE Threads::Threads)
add_test(NAME Stage4_ForgeKV COMMAND test_forgekv)

# Stage 5: Compaction, Interactive CLI, and Benchmark Suite
add_executable(test_compaction
    src/memtable.cpp
    src/wal.cpp
    src/bloom_filter.cpp
    src/sstable.cpp
    src/compaction.cpp
    src/forgekv.cpp
    tests/test_compaction.cpp
)
target_link_libraries(test_compaction PRIVATE Threads::Threads)
add_test(NAME Stage5_Compaction COMMAND test_compaction)

add_executable(forgekv_cli
    src/memtable.cpp
    src/wal.cpp
    src/bloom_filter.cpp
    src/sstable.cpp
    src/compaction.cpp
    src/forgekv.cpp
    cli/forgekv_cli.cpp
)
target_link_libraries(forgekv_cli PRIVATE Threads::Threads)

add_executable(bench_forgekv
    src/memtable.cpp
    src/wal.cpp
    src/bloom_filter.cpp
    src/sstable.cpp
    src/compaction.cpp
    src/forgekv.cpp
    benchmarks/bench_forgekv.cpp
)
target_link_libraries(bench_forgekv PRIVATE Threads::Threads)
