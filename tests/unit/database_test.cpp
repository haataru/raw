#include "db/database.hpp"

#include <gtest/gtest.h>

#include <cstring>

using namespace rawdb;

TEST(DatabaseTest, OpenClose)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_db_test";
    std::filesystem::remove_all(path);

    EXPECT_EQ(db.open(path), Status::kOk);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(DatabaseTest, CreateTable)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_createtbl";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kFloat64, ColumnType::kVarChar};
    schema.names = {"id", "value", "name"};

    auto tid = db.create_table("test", schema);
    ASSERT_TRUE(tid.has_value());
    EXPECT_EQ(*tid, 0);
    EXPECT_EQ(db.table_count(), 1);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(DatabaseTest, InsertAndFlush)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_insert";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kInt64};
    schema.names = {"a", "b"};
    auto tid = *db.create_table("t", schema);

    // Insert 5 rows one by one
    for (int i = 1; i <= 5; ++i) {
        int32_t a = i;
        int64_t b = i * 10;
        std::vector<ColumnData> cols(2);
        cols[0].type = ColumnType::kInt32;
        cols[0].data = reinterpret_cast<const std::byte *>(&a);
        cols[0].size = sizeof(a);
        cols[0].nulls = nullptr;
        cols[1].type = ColumnType::kInt64;
        cols[1].data = reinterpret_cast<const std::byte *>(&b);
        cols[1].size = sizeof(b);
        cols[1].nulls = nullptr;
        EXPECT_TRUE(db.insert(tid, cols).has_value());
    }

    EXPECT_EQ(db.table(tid).row_count(), 5);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(DatabaseTest, InsertMultipleBatches)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_multi";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    auto tid = *db.create_table("nums", schema);

    for (int batch = 0; batch < 5; ++batch) {
        for (int i = 0; i < 10; ++i) {
            int32_t val = batch * 10 + i;
            std::vector<ColumnData> cols(1);
            cols[0].type = ColumnType::kInt32;
            cols[0].data = reinterpret_cast<const std::byte *>(&val);
            cols[0].size = sizeof(val);
            cols[0].nulls = nullptr;
            EXPECT_TRUE(db.insert(tid, cols).has_value());
        }
    }

    EXPECT_EQ(db.table(tid).row_count(), 50);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(DatabaseTest, InsertWrongColumnCount)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_wrongcols";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kInt64};
    schema.names = {"a", "b"};
    auto tid = *db.create_table("t", schema);

    int32_t val = 42;
    std::vector<ColumnData> cols(1);
    cols[0].type = ColumnType::kInt32;
    cols[0].data = reinterpret_cast<const std::byte *>(&val);
    cols[0].size = sizeof(val);
    cols[0].nulls = nullptr;

    EXPECT_EQ(db.insert(tid, cols).error().code, Status::kInvalidArgument);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(DatabaseTest, InsertInvalidTableId)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_invalidtbl";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    int32_t val = 42;
    std::vector<ColumnData> cols(1);
    cols[0].type = ColumnType::kInt32;
    cols[0].data = reinterpret_cast<const std::byte *>(&val);
    cols[0].size = sizeof(val);
    cols[0].nulls = nullptr;

    EXPECT_EQ(db.insert(999, cols).error().code, Status::kInvalidArgument);

    db.close();
    std::filesystem::remove_all(path);
}
