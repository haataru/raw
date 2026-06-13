#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "core/error.hpp"
#include "db/database.hpp"
#include "query/executor.hpp"

using namespace rawdb;

static constexpr int kThreadCount = 8;
static constexpr int kInsertsPerThread = 100;
static constexpr int kSelectsPerThread = 10;

TEST(ConcurrencyTest, MultiThreadInsertSelect)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_concurrency";
    std::filesystem::remove_all(path);

    Database db;
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt64, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    auto tid = db.create_table("test", std::move(schema));
    ASSERT_TRUE(tid.has_value());

    std::atomic<int> total_inserts{0};
    std::atomic<int> total_selects{0};
    std::atomic<bool> error_happened{false};
    std::mutex error_mtx;
    std::string error_msg;
    std::barrier phase1_done{kThreadCount + 1};
    std::barrier phase2_done{kThreadCount + 1};

    // Phase 1: concurrent INSERT (no flush)
    auto inserter = [&](int seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        std::vector<std::byte> int_buf(sizeof(int64_t));
        for (int i = 0; i < kInsertsPerThread && !error_happened.load(); ++i) {
            int64_t val = static_cast<int64_t>(seed * 100000 + i);
            std::string name = "user_" + std::to_string(val);

            std::memcpy(int_buf.data(), &val, sizeof(val));

            auto name_buf = std::vector<std::byte>(sizeof(uint32_t) + name.size());
            uint32_t end = static_cast<uint32_t>(name.size());
            std::memcpy(name_buf.data(), &end, sizeof(uint32_t));
            std::memcpy(name_buf.data() + sizeof(uint32_t), name.data(), name.size());

            std::vector<ColumnData> cols(2);
            cols[0].type = ColumnType::kInt64;
            cols[0].data = int_buf.data();
            cols[0].size = sizeof(val);
            cols[0].nulls = nullptr;
            cols[1].type = ColumnType::kVarChar;
            cols[1].data = name_buf.data();
            cols[1].size = name_buf.size();
            cols[1].nulls = nullptr;

            auto st = db.insert(*tid, cols);
            if (!st) {
                std::lock_guard lk(error_mtx);
                if (!error_happened.exchange(true)) {
                    error_msg = "insert failed: " + std::string(status_message(st.error().code));
                }
                return;
            }
            total_inserts.fetch_add(1, std::memory_order_relaxed);
        }
        phase1_done.arrive_and_wait();
    };

    // Phase 2: concurrent SELECT
    auto selector = [&](int) {
        phase1_done.arrive_and_wait();
        Connection conn(db);
        Executor exec(conn);
        for (int i = 0; i < kSelectsPerThread && !error_happened.load(); ++i) {
            auto res = exec.execute("SELECT * FROM test WHERE id >= 0");
            if (!res.has_value()) {
                std::lock_guard lk(error_mtx);
                if (!error_happened.exchange(true)) {
                    error_msg = "SELECT failed: " + std::string(status_message(res.error().code));
                }
                return;
            }
            total_selects.fetch_add(1, std::memory_order_relaxed);
        }
        phase2_done.arrive_and_wait();
    };

    // Launch inserter threads
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (int i = 0; i < kThreadCount; ++i) {
            threads.emplace_back(inserter, i + 1);
        }
        phase1_done.arrive_and_wait();
        for (auto &t : threads) {
            if (t.joinable())
                t.join();
        }
    }

    ASSERT_FALSE(error_happened.load()) << error_msg;
    ASSERT_EQ(total_inserts.load(), kThreadCount * kInsertsPerThread);
    ASSERT_EQ(db.table(*tid).row_count(), static_cast<size_t>(total_inserts.load()));

    // Launch selector threads
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (int i = 0; i < kThreadCount; ++i) {
            threads.emplace_back(selector, i + 1);
        }
        phase1_done.arrive_and_wait(); // all 9 ready (8 selectors + main)
        phase2_done.arrive_and_wait(); // wait for all selectors done
        for (auto &t : threads) {
            if (t.joinable())
                t.join();
        }
    }

    ASSERT_FALSE(error_happened.load()) << error_msg;
    EXPECT_GT(total_selects.load(), 0);

    // Final verification
    Connection conn_test(db);
    auto sel = Executor(conn_test).execute("SELECT * FROM test");
    ASSERT_TRUE(sel.has_value());
    EXPECT_EQ(sel->rows.size(), static_cast<size_t>(total_inserts.load()));

    db.close();
    std::filesystem::remove_all(path);
}
