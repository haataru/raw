#include <gtest/gtest.h>

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"

using namespace rawdb;

TEST(TypesTest, StatusMessage)
{
    EXPECT_EQ(status_message(Status::kOk), "ok");
    EXPECT_EQ(status_message(Status::kNotFound), "not found");
    EXPECT_EQ(status_message(Status::kOutOfMemory), "out of memory");
    EXPECT_EQ(status_message(Status::kFatal), "fatal error");
}

TEST(TypesTest, ColumnTypeValues)
{
    EXPECT_EQ(static_cast<uint8_t>(ColumnType::kInt32), 0);
    EXPECT_EQ(static_cast<uint8_t>(ColumnType::kInt64), 1);
    EXPECT_EQ(static_cast<uint8_t>(ColumnType::kFloat64), 2);
    EXPECT_EQ(static_cast<uint8_t>(ColumnType::kBool), 3);
    EXPECT_EQ(static_cast<uint8_t>(ColumnType::kVarChar), 4);
}

TEST(ConfigTest, Constants)
{
    EXPECT_GT(config::kPageSize, 0);
    EXPECT_GT(config::kMaxPendingRows, 0);
    EXPECT_GT(config::kSyncIntervalMs, 0);
}

TEST(ErrorTest, StatusOrOk)
{
    StatusOr<int> result = 42;
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(ErrorTest, StatusOrError)
{
    StatusOr<int> result = std::unexpected(Status::kNotFound);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Status::kNotFound);
}
