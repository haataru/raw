#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

#include "rawdb.h"

TEST(CapiTest, OpenClose)
{
    auto path = (std::filesystem::temp_directory_path() / "rawdb_capi").string();
    std::filesystem::remove_all(path);

    auto *db = rawdb_open(path.c_str());
    ASSERT_NE(db, nullptr);
    rawdb_close(db);
    std::filesystem::remove_all(path);
}

TEST(CapiTest, InsertAndSelect)
{
    auto path = (std::filesystem::temp_directory_path() / "rawdb_capi_is").string();
    std::filesystem::remove_all(path);

    auto *db = rawdb_open(path.c_str());
    ASSERT_NE(db, nullptr);

    rawdb_result_t *res = nullptr;

    // Create table
    int rc = rawdb_execute(db, "CREATE TABLE users (id INT32, name VARCHAR)", &res);
    EXPECT_EQ(rc, 0) << rawdb_error_message(db);
    ASSERT_NE(res, nullptr);
    rawdb_result_free(res);
    res = nullptr;

    // INSERT via SQL
    rc = rawdb_execute(db, "INSERT INTO users VALUES (1, 'alice')", &res);
    EXPECT_EQ(rc, 0) << rawdb_error_message(db);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(rawdb_result_row_count(res), 1);
    EXPECT_EQ(rawdb_result_col_count(res), 1);
    EXPECT_STREQ(rawdb_result_column_name(res, 0), "rows_inserted");
    EXPECT_STREQ(rawdb_result_value(res, 0, 0), "1");
    rawdb_result_free(res);
    res = nullptr;

    // SELECT
    rc = rawdb_execute(db, "SELECT * FROM users", &res);
    EXPECT_EQ(rc, 0) << rawdb_error_message(db);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(rawdb_result_row_count(res), 1);
    EXPECT_EQ(rawdb_result_col_count(res), 2);
    EXPECT_STREQ(rawdb_result_column_name(res, 0), "id");
    EXPECT_STREQ(rawdb_result_column_name(res, 1), "name");
    EXPECT_STREQ(rawdb_result_value(res, 0, 0), "1");
    EXPECT_STREQ(rawdb_result_value(res, 0, 1), "alice");
    rawdb_result_free(res);
    res = nullptr;

    // Unknown table
    rc = rawdb_execute(db, "SELECT * FROM nonexistent", &res);
    EXPECT_NE(rc, 0);

    rawdb_close(db);
    std::filesystem::remove_all(path);
}

TEST(CapiTest, Delete)
{
    auto path = (std::filesystem::temp_directory_path() / "rawdb_capi_del").string();
    std::filesystem::remove_all(path);

    auto *db = rawdb_open(path.c_str());
    ASSERT_NE(db, nullptr);

    rawdb_result_t *res = nullptr;

    rawdb_execute(db, "CREATE TABLE t (id INT32)", &res);
    rawdb_result_free(res);
    res = nullptr;

    rawdb_execute(db, "INSERT INTO t VALUES (10)", &res);
    rawdb_result_free(res);
    res = nullptr;

    rawdb_execute(db, "INSERT INTO t VALUES (20)", &res);
    rawdb_result_free(res);
    res = nullptr;

    rawdb_execute(db, "INSERT INTO t VALUES (30)", &res);
    rawdb_result_free(res);
    res = nullptr;

    // DELETE
    int rc = rawdb_execute(db, "DELETE FROM t WHERE id < 30", &res);
    EXPECT_EQ(rc, 0) << rawdb_error_message(db);
    ASSERT_NE(res, nullptr);
    EXPECT_STREQ(rawdb_result_value(res, 0, 0), "2");
    rawdb_result_free(res);
    res = nullptr;

    rc = rawdb_execute(db, "SELECT * FROM t", &res);
    EXPECT_EQ(rc, 0) << rawdb_error_message(db);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(rawdb_result_row_count(res), 1);
    EXPECT_STREQ(rawdb_result_value(res, 0, 0), "30");
    rawdb_result_free(res);
    res = nullptr;

    rawdb_close(db);
    std::filesystem::remove_all(path);
}
