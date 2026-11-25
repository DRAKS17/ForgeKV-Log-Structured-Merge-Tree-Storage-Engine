#include "forgekv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string pad_key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "bench_key_%07d", i);
    return std::string(buf);
}

void cleanup_dir(const std::string& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

double percentile_us(const std::vector<double>& sorted_us, double pct) {
    if (sorted_us.empty()) {
        return 0.0;
    }
    const std::size_t idx = static_cast<std::size_t>(
        pct * static_cast<double>(sorted_us.size() - 1));
    return sorted_us[idx];
}

}  // namespace

int main() {
    const std::string bench_dir = "data/bench_db";
    fs::create_directories("data");
    cleanup_dir(bench_dir);

    constexpr int WRITE_N = 100000;
    constexpr int READ_N = 20000;
    constexpr int NEG_N = 50000;
    constexpr std::size_t PROD_FLUSH_BYTES = 4 * 1024 * 1024;  // 4 MB
    const std::string payload(100, 'V');  // 100-byte values

    // 1. Sequential Write Throughput (100,000 keys)
    double seq_ops_sec = 0.0;
    double seq_mb_sec = 0.0;
    {
        forgekv::ForgeKV db;
        db.open(bench_dir + "/seq", PROD_FLUSH_BYTES);

        const auto t0 = Clock::now();
        for (int i = 0; i < WRITE_N; ++i) {
            db.put(pad_key(i), payload);
        }
        const auto t1 = Clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double total_mb =
            static_cast<double>(WRITE_N) * (17.0 + payload.size()) / (1024.0 * 1024.0);
        seq_ops_sec = WRITE_N / secs;
        seq_mb_sec = total_mb / secs;
        db.close();
    }

    // 2. Random Write Throughput (100,000 keys)
    double rand_ops_sec = 0.0;
    std::vector<int> shuffled_ids(WRITE_N);
    for (int i = 0; i < WRITE_N; ++i) {
        shuffled_ids[i] = i;
    }
    std::mt19937 rng(1337);
    std::shuffle(shuffled_ids.begin(), shuffled_ids.end(), rng);

    {
        forgekv::ForgeKV db;
        db.open(bench_dir + "/rand", PROD_FLUSH_BYTES);

        const auto t0 = Clock::now();
        for (int id : shuffled_ids) {
            db.put(pad_key(id), payload);
        }
        const auto t1 = Clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        rand_ops_sec = WRITE_N / secs;

        // 3. Point Read Latency (20,000 random reads across MemTable + SSTables)
        std::vector<double> latencies_us;
        latencies_us.reserve(READ_N);
        std::uniform_int_distribution<int> dist(0, WRITE_N - 1);
        for (int i = 0; i < READ_N; ++i) {
            const std::string k = pad_key(dist(rng));
            const auto r0 = Clock::now();
            auto res = db.get(k);
            const auto r1 = Clock::now();
            if (!res.has_value()) {
                std::cerr << "Benchmark error: missing key " << k << "\n";
                return 1;
            }
            latencies_us.push_back(
                std::chrono::duration<double, std::micro>(r1 - r0).count());
        }
        std::sort(latencies_us.begin(), latencies_us.end());
        const double p50_us = percentile_us(latencies_us, 0.50);
        const double p95_us = percentile_us(latencies_us, 0.95);
        const double p99_us = percentile_us(latencies_us, 0.99);

        // Force flush into SSTables so negative lookups exercise disk SSTables + Bloom Filter.
        db.compact();

        // Re-seed smaller SSTables (4 SSTables without auto-compact) or use existing flushed SSTables:
        // Wait: let's measure negative lookups on the random DB (already flushed to SSTables)
        // AND build a multi-SSTable dataset to test Bloom ON vs OFF clearly!
        db.close();

        // 4. Negative Lookup Benchmark (50,000 absent keys): Bloom ON vs OFF
        forgekv::ForgeKV neg_db;
        neg_db.open(bench_dir + "/neg", 512 * 1024, 16);
        for (int i = 0; i < 40000; ++i) {
            neg_db.put(pad_key(i), payload);
        }
        // Close & reopen to ensure all SSTables are flushed on disk (flush remaining via compact? No, keep multiple SSTables!)
        // Leaving auto_compact_threshold = 16 keeps ~10 SSTables on disk for realistic negative lookups.

        neg_db.reset_read_stats();
        const auto bon0 = Clock::now();
        for (int i = 0; i < NEG_N; ++i) {
            auto r = neg_db.get("missing_key_" + std::to_string(i), /*use_bloom=*/true);
            if (r.has_value()) {
                return 1;
            }
        }
        const auto bon1 = Clock::now();
        const double bloom_on_ms =
            std::chrono::duration<double, std::milli>(bon1 - bon0).count();
        const auto stats_on = neg_db.stats();

        neg_db.reset_read_stats();
        const auto boff0 = Clock::now();
        for (int i = 0; i < NEG_N; ++i) {
            auto r = neg_db.get("missing_key_" + std::to_string(i), /*use_bloom=*/false);
            if (r.has_value()) {
                return 1;
            }
        }
        const auto boff1 = Clock::now();
        const double bloom_off_ms =
            std::chrono::duration<double, std::milli>(boff1 - boff0).count();
        const auto stats_off = neg_db.stats();

        const double avoided_pct =
            (stats_off.total_disk_reads > 0)
                ? 100.0 *
                      static_cast<double>(stats_off.total_disk_reads -
                                          stats_on.total_disk_reads) /
                      static_cast<double>(stats_off.total_disk_reads)
                : 0.0;
        const double speedup = bloom_off_ms / std::max(bloom_on_ms, 0.001);
        neg_db.close();

        // 5. Compaction Space Reclamation (50% overwrite/delete churn)
        forgekv::ForgeKV comp_db;
        comp_db.open(bench_dir + "/compact", 256 * 1024, 100);
        constexpr int COMP_N = 40000;
        for (int i = 0; i < COMP_N; ++i) {
            comp_db.put(pad_key(i), payload);
        }
        // Overwrite 25% and delete 25% (= 50% churn across SSTables).
        for (int i = 0; i < COMP_N / 2; ++i) {
            if (i % 2 == 0) {
                comp_db.del(pad_key(i));
            } else {
                comp_db.put(pad_key(i), std::string(100, 'U'));
            }
        }
        const uint64_t disk_before = comp_db.stats().total_disk_bytes;
        comp_db.compact();
        const uint64_t disk_after = comp_db.stats().total_disk_bytes;
        const double reclaim_pct =
            (disk_before > 0)
                ? 100.0 * static_cast<double>(disk_before - disk_after) /
                      static_cast<double>(disk_before)
                : 0.0;
        comp_db.close();

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n==================================================================\n";
        std::cout << "                   ForgeKV Benchmark Summary\n";
        std::cout << "==================================================================\n";
        std::cout << "Sequential Write (100k keys)  : " << seq_ops_sec << " ops/sec ("
                  << seq_mb_sec << " MB/sec)\n";
        std::cout << "Random Write (100k keys)      : " << rand_ops_sec << " ops/sec\n";
        std::cout << "Point Read Latency (20k ops)  : p50 = " << p50_us
                  << " us | p95 = " << p95_us << " us | p99 = " << p99_us << " us\n";
        std::cout << "Negative Lookups (50k keys)   :\n";
        std::cout << "  - Bloom OFF                 : " << bloom_off_ms << " ms ("
                  << stats_off.total_disk_reads << " disk reads)\n";
        std::cout << "  - Bloom ON                  : " << bloom_on_ms << " ms ("
                  << stats_on.total_disk_reads << " disk reads)\n";
        std::cout << "  - Savings & Speedup         : " << avoided_pct
                  << "% disk reads avoided (" << speedup << "x faster)\n";
        std::cout << "Compaction Space Reclamation  : " << disk_before << " B -> "
                  << disk_after << " B (" << reclaim_pct << "% reclaimed)\n";
        std::cout << "==================================================================\n";
    }

    cleanup_dir(bench_dir);
    return 0;
}
