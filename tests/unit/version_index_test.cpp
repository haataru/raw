#include "mvcc/version_index.hpp"

#include <gtest/gtest.h>

using namespace rawdb;

TEST(VersionIndexTest, SearchFound)
{
    VersionIndex idx;

    IndexEntry entries[] = {
        {0, 10, 100, 8},
        {0, 20, 200, 8},
        {1, 15, 300, 8},
    };
    idx.insert_bulk(entries, 3);

    auto off = idx.search(0, 15);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, 100);

    off = idx.search(0, 20);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, 200);

    off = idx.search(1, 20);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, 300);
}

TEST(VersionIndexTest, SearchNotFound)
{
    VersionIndex idx;

    IndexEntry entries[] = {
        {0, 10, 100, 8},
    };
    idx.insert_bulk(entries, 1);

    auto off = idx.search(0, 5);
    EXPECT_FALSE(off.has_value());
    EXPECT_EQ(off.error(), Status::kNotFound);
}

TEST(VersionIndexTest, SearchNonExistentRow)
{
    VersionIndex idx;
    IndexEntry entries[] = {{0, 10, 100, 8}};
    idx.insert_bulk(entries, 1);

    auto off = idx.search(42, 100);
    EXPECT_FALSE(off.has_value());
}

TEST(VersionIndexTest, EmptyIndex)
{
    VersionIndex idx;
    auto off = idx.search(0, 100);
    EXPECT_FALSE(off.has_value());
}

TEST(VersionIndexTest, BulkInsertMultipleBatches)
{
    VersionIndex idx;

    IndexEntry batch1[] = {
        {0, 10, 100, 8},
        {2, 30, 300, 8},
    };
    idx.insert_bulk(batch1, 2);

    IndexEntry batch2[] = {
        {1, 20, 200, 8},
        {3, 40, 400, 8},
    };
    idx.insert_bulk(batch2, 2);

    EXPECT_EQ(idx.size(), 4);

    EXPECT_EQ(*idx.search(0, 20), 100);
    EXPECT_EQ(*idx.search(1, 20), 200);
    EXPECT_EQ(*idx.search(2, 30), 300);
    EXPECT_EQ(*idx.search(3, 40), 400);
}

TEST(VersionIndexTest, PruneKeepsKeeperAndNewer)
{
    VersionIndex idx;
    IndexEntry entries[] = {
        {0, 100, 1, 0},
        {0, 80, 2, 0},
        {0, 50, 3, 0},
        {0, 30, 4, 0},
        {1, 200, 5, 0},
    };
    idx.insert_bulk(entries, 5); // sorted: (0,100),(0,80),(0,50),(0,30),(1,200)
    EXPECT_EQ(idx.size(), 5);

    idx.prune(75);
    /* Row 0: keeper = ts=50 (newest with ts <= 75)
         keep: ts=100, ts=80, ts=50
       Row 1: ts=200 > 75, no keeper, keep all */
    ASSERT_EQ(idx.size(), 4);

    EXPECT_EQ(*idx.search(0, 55), 3);
    EXPECT_EQ(*idx.search(0, 75), 3);
    EXPECT_EQ(*idx.search(0, 90), 2);
    EXPECT_EQ(*idx.search(1, 200), 5);
}

TEST(VersionIndexTest, PruneKeepsAllWhenAllAboveCutoff)
{
    VersionIndex idx;
    IndexEntry entries[] = {
        {0, 100, 1, 0},
        {0, 80, 2, 0},
    };
    idx.insert_bulk(entries, 2);
    idx.prune(75);
    EXPECT_EQ(idx.size(), 2);
}

TEST(VersionIndexTest, PruneRemovesOlderThanKeeper)
{
    VersionIndex idx;
    IndexEntry entries[] = {
        {0, 100, 1, 0},
        {0, 70, 2, 0},
        {0, 50, 3, 0},
        {0, 30, 4, 0},
    };
    idx.insert_bulk(entries, 4);
    idx.prune(80);
    /* keeper = ts=70 (newest with ts <= 80)
       keep: ts=100, ts=70 */
    ASSERT_EQ(idx.size(), 2);

    EXPECT_FALSE(idx.search(0, 65).has_value());
    EXPECT_EQ(*idx.search(0, 75), 2);
}

TEST(VersionIndexTest, PruneEmptyIndex)
{
    VersionIndex idx;
    idx.prune(100);
    EXPECT_EQ(idx.size(), 0);
}

TEST(VersionIndexTest, PruneKeepsTombstone)
{
    VersionIndex idx;
    IndexEntry entries[] = {
        {0, 100, VersionIndex::kNotFound, 0},
        {0, 70, 1, 0},
        {0, 50, 2, 0},
    };
    idx.insert_bulk(entries, 3);
    idx.prune(80);
    /* keeper = ts=70 (newest with ts <= 80)
       keep: ts=100 (tombstone), ts=70 */
    ASSERT_EQ(idx.size(), 2);

    EXPECT_TRUE(idx.search(0, 100).has_value());
    EXPECT_EQ(*idx.search(0, 75), 1);
    EXPECT_FALSE(idx.search(0, 60).has_value());
}
