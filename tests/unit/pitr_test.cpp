#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "db/database.hpp"
#include "query/executor.hpp"

using namespace rawdb;

class PITRTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = std::filesystem::temp_directory_path() / "rawdb_pitr_test_db";
        std::error_code ec;
        std::filesystem::remove_all(db_path_, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(db_path_, ec);

        // Clean all test backups
        auto parent_dir = db_path_.parent_path();
        for (const auto &entry : std::filesystem::directory_iterator(parent_dir)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (name.starts_with("backup_")) {
                    std::filesystem::remove_all(entry.path(), ec);
                }
            }
        }
    }

    std::filesystem::path db_path_;
};

TEST_F(PITRTest, PointInTimeRecovery)
{
    uint64_t target_time_ms = 0;
    auto backup_path = db_path_.parent_path() / "backup_test_pitr";

    // First phase: create db and insert initial data
    {
        Database db;
        ASSERT_EQ(db.open(db_path_), Status::kOk);

        Connection conn(db);
        Executor exec(conn);

        ASSERT_TRUE(exec.execute("CREATE TABLE test_pitr (id int32, val int32);").has_value());

        // Take a base backup
        std::error_code ec;
        std::filesystem::remove_all(backup_path, ec);
        ASSERT_EQ(db.start_backup(backup_path), Status::kOk);

        ASSERT_EQ(conn.begin(), Status::kOk);
        ASSERT_TRUE(exec.execute("INSERT INTO test_pitr VALUES (1, 100);").has_value());
        ASSERT_EQ(conn.commit(), Status::kOk);

        auto res1 = exec.execute("SELECT id FROM test_pitr;");
        std::cout << "After first insert, rows: " << res1.value().rows.size() << std::endl;

        // Wait a small amount of time to ensure different timestamps
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Capture time after first insert
        target_time_ms =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ASSERT_EQ(conn.begin(), Status::kOk);
        ASSERT_TRUE(exec.execute("INSERT INTO test_pitr VALUES (2, 200);").has_value());
        ASSERT_EQ(conn.commit(), Status::kOk);

        auto res = exec.execute("SELECT id FROM test_pitr;");
        std::cout << "After second insert, rows: " << res.value().rows.size() << std::endl;
        ASSERT_TRUE(res.has_value());
        ASSERT_EQ(res->rows.size(), 2);
    }

    // Simulate restoring from the backup
    // But we need the WAL directory from the current db_path_ because it contains the future
    // operations!
    auto current_wal_dir = db_path_ / "wal";
    auto temp_wal_dir = std::filesystem::temp_directory_path() / "temp_pitr_wal";
    std::error_code ec;
    std::filesystem::remove_all(temp_wal_dir, ec);
    std::filesystem::copy(current_wal_dir,
                          temp_wal_dir,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);

    std::filesystem::remove_all(db_path_, ec);
    std::filesystem::copy(backup_path, db_path_, std::filesystem::copy_options::recursive, ec);

    // Put the WAL directory back so PITR can replay the history
    std::filesystem::remove_all(db_path_ / "wal", ec);
    std::filesystem::copy(temp_wal_dir,
                          db_path_ / "wal",
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    std::filesystem::remove_all(temp_wal_dir, ec);

    // Second phase: open with target time, should replay up to the first row
    {
        Database db;
        ASSERT_EQ(db.open(db_path_, target_time_ms), Status::kOk);

        Connection conn(db);
        Executor exec(conn);

        auto res = exec.execute("SELECT id FROM test_pitr;");
        ASSERT_TRUE(res.has_value());
        ASSERT_EQ(res->rows.size(), 1);
        ASSERT_EQ(res->rows[0][0], "1");
    }
}

TEST_F(PITRTest, ConfigBackupCommands)
{
    Database db;
    ASSERT_EQ(db.open(db_path_), Status::kOk);

    Connection conn(db);
    Executor exec(conn);

    auto res1 = exec.execute("SET BACKUP INTERVAL TO 120;");
    ASSERT_TRUE(res1.has_value());
    ASSERT_EQ(db.backup_interval_seconds(), 120);

    auto res2 = exec.execute("SET BACKUP RETENTION TO 3600;");
    ASSERT_TRUE(res2.has_value());
    ASSERT_EQ(db.backup_retention_seconds(), 3600);
}
