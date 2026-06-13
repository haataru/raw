#include "storage/table.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace rawdb;

TEST(TableTest, CreateTable)
{
    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kFloat64, ColumnType::kVarChar};
    schema.names = {"id", "value", "name"};

    Table table("test", schema);
    EXPECT_EQ(table.name(), "test");
    EXPECT_EQ(table.schema().column_count(), 3);
    EXPECT_EQ(table.row_count(), 0);
}

TEST(TableTest, InsertRow)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_tbl_test";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    Table table("nums", schema);
    ASSERT_EQ(table.open_file(path), Status::kOk);

    ColumnData cd;
    int32_t val = 42;
    cd.type = ColumnType::kInt32;
    cd.data = reinterpret_cast<const std::byte *>(&val);
    cd.size = sizeof(val);
    cd.nulls = nullptr;

    EXPECT_TRUE(table.insert_row(1, {cd}).has_value());
    EXPECT_EQ(table.row_count(), 1);

    std::filesystem::remove_all(path);
}

TEST(TableTest, RowCountAfterInsert)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_tbl_rc";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    Table table("rc", schema);
    ASSERT_EQ(table.open_file(path), Status::kOk);

    ColumnData cd;
    for (int i = 0; i < 5; ++i) {
        int32_t val = i;
        cd.type = ColumnType::kInt32;
        cd.data = reinterpret_cast<const std::byte *>(&val);
        cd.size = sizeof(val);
        cd.nulls = nullptr;
        EXPECT_TRUE(table.insert_row(static_cast<Timestamp>(i + 1), {cd}).has_value());
    }
    EXPECT_EQ(table.row_count(), 5);

    std::filesystem::remove_all(path);
}

TEST(TableTest, WrongColumnCount)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_tbl_wcc";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kInt64};
    schema.names = {"a", "b"};
    Table table("wcc", schema);
    ASSERT_EQ(table.open_file(path), Status::kOk);

    ColumnData cd;
    int32_t val = 42;
    cd.type = ColumnType::kInt32;
    cd.data = reinterpret_cast<const std::byte *>(&val);
    cd.size = sizeof(val);
    cd.nulls = nullptr;

    EXPECT_EQ(table.insert_row(1, {cd}).error().code, Status::kInvalidArgument);

    std::filesystem::remove_all(path);
}

TEST(TableTest, VersionIndexAfterInsert)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_tbl_vi";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    Table table("vi", schema);
    ASSERT_EQ(table.open_file(path), Status::kOk);

    // Insert rows to trigger write_pending_to_page
    for (int i = 0; i < 10; ++i) {
        int32_t val = i;
        ColumnData cd;
        cd.type = ColumnType::kInt32;
        cd.data = reinterpret_cast<const std::byte *>(&val);
        cd.size = sizeof(val);
        cd.nulls = nullptr;

        EXPECT_TRUE(table.insert_row(static_cast<Timestamp>(i + 1), {cd}).has_value());
    }

    // Flush pending to ensure rows are in pages and version index
    table.flush_pending();

    EXPECT_GT(table.version_index_size(), 0);

    auto r = table.search_version_index(0, 100);
    ASSERT_TRUE(r.has_value());

    std::filesystem::remove_all(path);
}

TEST(TableTest, LookupNotFound)
{
    Schema schema;
    schema.columns = {ColumnType::kBool};
    Table table("flags", schema);

    auto page = table.lookup_page(0);
    EXPECT_FALSE(page.has_value());
    EXPECT_EQ(page.error(), Status::kNotFound);
}
