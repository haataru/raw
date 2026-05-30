#include <gtest/gtest.h>

#include <thread>

#include "mvcc/version_index.hpp"

using namespace rawdb;

TEST(TimestampTest, AllocateMonotonic)
{
    TimestampAllocator ta;
    auto t1 = ta.allocate_ts();
    auto t2 = ta.allocate_ts();
    EXPECT_EQ(t1, 1);
    EXPECT_EQ(t2, 2);
    EXPECT_LT(t1, t2);
}

TEST(TimestampTest, CurrentIncreases)
{
    TimestampAllocator ta;
    auto c1 = ta.current();
    {
        auto _ = ta.allocate_ts();
        (void)_;
    }
    auto c2 = ta.current();
    EXPECT_EQ(c2, c1 + 1);
}

TEST(TimestampTest, ConcurrentAllocation)
{
    TimestampAllocator ta;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 1000;

    std::atomic<uint64_t> allocated{0};
    auto worker = [&]() {
        for (int i = 0; i < kPerThread; ++i) {
            auto ts = ta.allocate_ts();
            allocated.fetch_add(1, std::memory_order_relaxed);
            EXPECT_GT(ts, 0);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker);
    for (auto &t : threads)
        t.join();

    EXPECT_EQ(allocated.load(), kThreads * kPerThread);
}
