#include "mvcc/gc.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <shared_mutex>
#include <thread>

#include "storage/table.hpp"

using namespace rawdb;

TEST(GCTest, StartStop)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    TimestampAllocator ta;
    GlobalWatermarks wm;
    GarbageCollector gc(tables, tables_mtx, ta, wm);

    EXPECT_FALSE(gc.is_running());

    gc.start();
    EXPECT_TRUE(gc.is_running());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    gc.stop();
    EXPECT_FALSE(gc.is_running());
}

TEST(GCTest, DoubleStartNoOp)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    TimestampAllocator ta;
    GlobalWatermarks wm;
    GarbageCollector gc(tables, tables_mtx, ta, wm);

    gc.start();
    gc.start();
    EXPECT_TRUE(gc.is_running());

    gc.stop();
}

TEST(GCTest, StopWithoutStart)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    TimestampAllocator ta;
    GlobalWatermarks wm;
    GarbageCollector gc(tables, tables_mtx, ta, wm);

    gc.stop();
    EXPECT_FALSE(gc.is_running());
}

TEST(GCTest, PruneViaGCLoop)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    tables.emplace_back("t1", Schema{});
    TimestampAllocator ta;

    IndexEntry entries[] = {
        {0, 100, 1, 0},
        {0, 70, 2, 0},
        {0, 50, 3, 0},
    };
    tables[0].insert_version_entries(entries, 3);
    EXPECT_EQ(tables[0].version_index_size(), 3);

    // Allocate enough timestamps so GC's cutoff >= 80
    for (int i = 0; i < 80; ++i) {
        auto _ = ta.allocate_ts();
        (void)_;
    }

    GlobalWatermarks wm;
    GarbageCollector gc(tables, tables_mtx, ta, wm);
    gc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(config::kGcIntervalMs + 200));
    gc.stop();

    EXPECT_EQ(tables[0].version_index_size(), 2);
    EXPECT_EQ(*tables[0].search_version_index(0, 75), 2);
}

TEST(GCTest, PruneEmptyTable)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    tables.emplace_back("t1", Schema{});
    TimestampAllocator ta;
    GlobalWatermarks wm;

    {
        auto _ = ta.allocate_ts();
        (void)_;
    } // ensure cutoff > 0

    GarbageCollector gc(tables, tables_mtx, ta, wm);
    gc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(config::kGcIntervalMs + 200));
    gc.stop();

    EXPECT_EQ(tables[0].row_count(), 0);
}
