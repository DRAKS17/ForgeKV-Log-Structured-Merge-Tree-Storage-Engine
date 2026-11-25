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
enable_testing()

# Stage 1: SkipList MemTable
add_executable(test_memtable src/memtable.cpp tests/test_memtable.cpp)
add_test(NAME Stage1_MemTable COMMAND test_memtable)

# Remaining stages will be registered as each module is built:
# - Stage 2: test_wal
# - Stage 3: test_sstable
# - Stage 4: test_forgekv
# - Stage 5: test_compaction, forgekv_cli, bench_forgekv
