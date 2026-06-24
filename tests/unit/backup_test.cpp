#include <gtest/gtest.h>

#include <filesystem>

#include "rawdb.h"

class BackupTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::remove_all("test_backup_db");
        std::filesystem::remove_all("test_backup_dest");
    }

    void TearDown() override
    {
        std::filesystem::remove_all("test_backup_db");
        std::filesystem::remove_all("test_backup_dest");
    }
};

TEST_F(BackupTest, BasicBackupAndRestore)
{
    {
        auto db = rawdb_open("test_backup_db");
        ASSERT_NE(db, nullptr);

        rawdb_execute(db, "CREATE TABLE t1 (id INT, val VARCHAR);", nullptr);
        rawdb_execute(db, "INSERT INTO t1 VALUES (1, 'hello'), (2, 'world');", nullptr);

        // Take backup
        rawdb_result_t *res = nullptr;
        int status_code = rawdb_execute(db, "BACKUP TO 'test_backup_dest';", &res);
        ASSERT_EQ(status_code, 0);
        ASSERT_NE(res, nullptr);

        const char *status = rawdb_result_value(res, 0, 0);
        EXPECT_STREQ(status, "BACKUP SUCCESS");
        rawdb_result_free(res);

        rawdb_execute(db, "INSERT INTO t1 VALUES (3, 'after_backup');", nullptr);
        rawdb_close(db);
    }

    {
        // Now open the backup destination
        auto db = rawdb_open("test_backup_dest");
        ASSERT_NE(db, nullptr);

        rawdb_result_t *res = nullptr;
        int status_code = rawdb_execute(db, "SELECT id, val FROM t1;", &res);
        ASSERT_EQ(status_code, 0);
        ASSERT_NE(res, nullptr);
        EXPECT_EQ(rawdb_result_row_count(res), 2);
        EXPECT_STREQ(rawdb_result_value(res, 0, 0), "1");
        EXPECT_STREQ(rawdb_result_value(res, 0, 1), "hello");
        EXPECT_STREQ(rawdb_result_value(res, 1, 0), "2");
        EXPECT_STREQ(rawdb_result_value(res, 1, 1), "world");
        rawdb_result_free(res);
        rawdb_close(db);
    }
}
