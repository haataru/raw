#include "buffer/flush_handler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <shared_mutex>
#include <thread>

#include "core/types.hpp"
#include "storage/table.hpp"

using namespace rawdb;

TEST(FlushHandlerTest, SignalTriggersMsync)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    tables.emplace_back("test", Schema{});

    FlushHandler handler(tables, tables_mtx);
    EXPECT_TRUE(handler.is_running());

    handler.signal();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // FlushHandler just msyncs — nothing to verify but no crash
}

TEST(FlushHandlerTest, FlushAllForShutdown)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    tables.emplace_back("data", Schema{});

    FlushHandler handler(tables, tables_mtx);
    // flush_all should complete quickly
    handler.flush_all();
}

TEST(FlushHandlerTest, EmptyFlushNoOp)
{
    std::deque<Table> tables;
    std::shared_mutex tables_mtx;
    tables.emplace_back("empty", Schema{});

    FlushHandler handler(tables, tables_mtx);
    handler.signal();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(tables[0].row_count(), 0);
}
