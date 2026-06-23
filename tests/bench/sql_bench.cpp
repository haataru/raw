#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "db/database.hpp"
#include "mvcc/version_index.hpp"
#include "query/connection.hpp"
#include "query/executor.hpp"

namespace rawdb
{
namespace
{

namespace fs = std::filesystem;
using clock = std::chrono::steady_clock;

static auto ns_to_ms(int64_t ns) -> double { return static_cast<double>(ns) / 1e6; }
static auto ns_to_us(int64_t ns) -> double { return static_cast<double>(ns) / 1e3; }

// ── SQL benchmarks (single DB: INSERT → SELECT fullscan → INDEX + lookup) ──

static auto run_sql_benchmarks(size_t kRowCount) -> void
{
    auto path = fs::temp_directory_path() / "rawdb_bench_sql";
    fs::remove_all(path);

    Database db;
    auto st = db.open(path);
    if (st != Status::kOk) {
        std::cerr << "open failed\n";
        std::exit(1);
    }

    Connection conn(db);
    Executor exec(conn);
    {
        auto r = exec.execute("CREATE TABLE t (id INT64)");
        if (!r) {
            std::cerr << "CREATE TABLE failed\n";
            std::exit(1);
        }
    }

    // size_t kRowCount is passed as argument
    constexpr int kScanIters = 10;
    constexpr size_t kLookupQueries = 1000;

    // ── INSERT ──
    {
        auto start = clock::now();
        constexpr size_t kBatchSize = 10000;
        for (size_t i = 0; i < kRowCount; i += kBatchSize) {
            exec.execute("BEGIN");
            std::string sql = "INSERT INTO t VALUES ";
            for (size_t j = 0; j < kBatchSize && (i + j) < kRowCount; ++j) {
                if (j > 0) sql += ", ";
                sql += "(" + std::to_string(static_cast<int64_t>(i + j)) + ")";
            }
            auto r = exec.execute(sql);
            if (!r) {
                std::cerr << "INSERT failed at " << i << "\n";
                std::exit(1);
            }
            exec.execute("COMMIT");
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        double sec = static_cast<double>(ns) / 1e9;
        std::cout << "bench_insert: " << kRowCount << " rows, "
                  << static_cast<double>(kRowCount) / sec << " rps, "
                  << ns_to_us(static_cast<int64_t>(ns / static_cast<int64_t>(kRowCount)))
                  << " us/op\n";
    }

    // ── SELECT full scan ──
    {
        // warmup
        auto r = exec.execute("SELECT * FROM t");
        if (!r) {
            std::cerr << "SELECT warmup failed\n";
            std::exit(1);
        }

        auto start = clock::now();
        for (int i = 0; i < kScanIters; ++i) {
            r = exec.execute("SELECT * FROM t");
            if (!r) {
                std::cerr << "SELECT failed\n";
                std::exit(1);
            }
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        double sec = static_cast<double>(ns) / 1e9;
        std::cout << "bench_select_fullscan: " << kRowCount << " rows, "
                  << static_cast<double>(kRowCount * kScanIters) / sec << " rows/sec, "
                  << ns_to_ms(static_cast<int64_t>(ns / static_cast<int64_t>(kScanIters)))
                  << " ms/query\n";
    }

    // ── SELECT with SIMD Filter ──
    {
        // warmup
        auto r = exec.execute("SELECT * FROM t WHERE id > 50000");
        if (!r) {
            std::cerr << "SELECT filter warmup failed\n";
            std::exit(1);
        }

        auto start = clock::now();
        for (int i = 0; i < kScanIters; ++i) {
            r = exec.execute("SELECT * FROM t WHERE id > 50000");
            if (!r) {
                std::cerr << "SELECT filter failed\n";
                std::exit(1);
            }
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        double sec = static_cast<double>(ns) / 1e9;
        std::cout << "bench_select_filter: " << kRowCount << " rows, "
                  << static_cast<double>(kRowCount * kScanIters) / sec << " rows/sec, "
                  << ns_to_ms(static_cast<int64_t>(ns / kScanIters)) << " ms/query\n";
    }

    // ── CREATE INDEX + SELECT index lookup (detailed timing) ──
    {
        auto r = exec.execute("CREATE INDEX idx_id ON t (id)");
        if (!r) {
            std::cerr << "CREATE INDEX failed\n";
            std::exit(1);
        }

        constexpr int kTimedQueries = 100;
        int64_t parse_ns = 0, btree_ns = 0, read_ns = 0, version_ns = 0, format_ns = 0;

        Parser parser;
        auto &tbl = db.table(0);

        for (size_t i = 0; i < kTimedQueries; ++i) {
            auto idx_val = static_cast<int64_t>(i % kRowCount);
            auto sql = "SELECT * FROM t WHERE id = " + std::to_string(idx_val);

            auto t0 = clock::now();
            auto stmt = parser.parse(sql);
            auto t1 = clock::now();

            auto &select = std::get<SelectStmt>(*stmt);

            auto t2 = clock::now();
            auto index_rids = Executor::index_lookup(tbl, select.where->pred);
            auto t3 = clock::now();

            if (!index_rids) {
                std::cerr << "index lookup failed at " << i << "\n";
                std::exit(1);
            }

            tbl.flush_pending();
            Timestamp read_ts = db.next_ts();

            auto t4 = clock::now();
            std::vector<RowId> visible;
            for (auto rid : *index_rids) {
                auto vr = tbl.search_version_index(rid, read_ts);
                if (!vr || *vr != Table::kNotFoundPage)
                    visible.push_back(rid);
            }
            auto t5 = clock::now();

            auto scan = tbl.read_rows(visible, {0});
            auto t6 = clock::now();

            if (scan) {
                for (size_t ri = 0; ri < visible.size(); ++ri) {
                    Executor::value_to_string(scan->columns[0],
                                              ColumnType::kInt64,
                                              ri,
                                              visible.size());
                }
            }
            auto t7 = clock::now();

            parse_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            btree_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
            version_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count();
            read_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t6 - t5).count();
            format_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t7 - t6).count();
        }

        int64_t total_ns = parse_ns + btree_ns + version_ns + read_ns + format_ns;
        auto per = [](int64_t ns, int cnt) { return ns_to_us(ns / cnt); };
        std::cout << "bench_index_lookup_detail (" << kTimedQueries << " queries):\n"
                  << "  parse:      " << per(parse_ns, kTimedQueries) << " us/op\n"
                  << "  btree:      " << per(btree_ns, kTimedQueries) << " us/op\n"
                  << "  version_idx:" << per(version_ns, kTimedQueries) << " us/op\n"
                  << "  read_rows:  " << per(read_ns, kTimedQueries) << " us/op\n"
                  << "  format:     " << per(format_ns, kTimedQueries) << " us/op\n"
                  << "  total:      " << per(total_ns, kTimedQueries) << " us/op ("
                  << 1'000'000'000 / (total_ns / kTimedQueries) << " qps)\n";

        // bulk via exec.execute() for comparison
        auto start = clock::now();
        for (size_t i = 0; i < kLookupQueries; ++i) {
            auto idx_val = static_cast<int64_t>(i % kRowCount);
            r = exec.execute("SELECT * FROM t WHERE id = " + std::to_string(idx_val));
            if (!r) {
                std::cerr << "INDEX lookup failed at " << i << "\n";
                std::exit(1);
            }
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        double sec = static_cast<double>(ns) / 1e9;
        std::cout << "bench_index_lookup (exec.execute): " << kLookupQueries << " queries, "
                  << static_cast<double>(kLookupQueries) / sec << " qps, "
                  << ns_to_us(static_cast<int64_t>(ns / static_cast<int64_t>(kLookupQueries)))
                  << " us/op\n";
    }

    db.close();
    fs::remove_all(path);
}

// ── Concurrent mix: 8 threads, INSERT + SELECT ──

static auto run_concurrent_mix() -> void
{
    auto path = fs::temp_directory_path() / "rawdb_bench_concurrent";
    fs::remove_all(path);

    Database db;
    auto st = db.open(path);
    if (st != Status::kOk) {
        std::cerr << "open failed\n";
        std::exit(1);
    }

    {
        Connection conn(db);
        Executor exec(conn);
        auto r = exec.execute("CREATE TABLE t (id INT64)");
        if (!r) {
            std::cerr << "CREATE TABLE failed\n";
            std::exit(1);
        }
    }

    constexpr size_t kInsertsPerThread = 50;
    constexpr size_t kSelectsPerThread = 10;

    std::atomic<size_t> insert_ok{0};
    std::atomic<size_t> select_ok{0};

    auto worker = [&](int tid) {
        Connection conn(db);
        Executor exec(conn);
        for (size_t i = 0; i < kInsertsPerThread; ++i) {
            auto r = exec.execute(
                "INSERT INTO t VALUES (" +
                std::to_string(static_cast<int64_t>(tid) * 100000 + static_cast<int64_t>(i)) + ")");
            if (r)
                insert_ok.fetch_add(1, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kSelectsPerThread; ++i) {
            auto r = exec.execute("SELECT * FROM t WHERE id >= 0");
            if (r)
                select_ok.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto start = clock::now();
    {
        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int t = 0; t < 8; ++t)
            threads.emplace_back(worker, t);
        for (auto &th : threads)
            th.join();
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    double sec = static_cast<double>(ns) / 1e9;
    size_t total_ops = insert_ok.load() + select_ok.load();

    std::cout << "bench_concurrent_mix: 8 threads, " << insert_ok.load() << " inserts + "
              << select_ok.load() << " selects, " << static_cast<double>(total_ops) / sec
              << " ops/sec, " << ns_to_ms(static_cast<int64_t>(ns)) << " ms total\n";

    db.close();
    fs::remove_all(path);
}

// ── VersionIndex::prune: 1M entries ──

static auto run_version_index_prune() -> void
{
    VersionIndex vi;
    constexpr size_t kCount = 1'000'000;

    std::vector<IndexEntry> entries;
    entries.reserve(kCount);
    for (uint64_t row = 0; row < 100'000; ++row) {
        for (Timestamp ts = 10; ts >= 1; --ts) {
            entries.push_back({row, ts, row * 100, 0});
        }
    }
    vi.insert_bulk(entries.data(), entries.size());

    auto start = clock::now();
    vi.prune(5);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();

    std::cout << "bench_version_index_prune: " << kCount << " entries, "
              << ns_to_ms(static_cast<int64_t>(ns)) << " ms, kept " << vi.size() << " entries\n";
}

} // namespace
} // namespace rawdb

auto main() -> int
{
    using namespace rawdb;

    std::cout << "=== rawDB Benchmarks ===\n\n";

    run_sql_benchmarks(1'000'000);
    std::cout << "\n";
    run_sql_benchmarks(10'000'000);
    std::cout << "\n";
    run_concurrent_mix();
    std::cout << "\n";
    run_version_index_prune();
    std::cout << "\nDone.\n";

    return 0;
}
