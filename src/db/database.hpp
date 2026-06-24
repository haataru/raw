#ifndef RAWDB_DB_DATABASE_HPP
#define RAWDB_DB_DATABASE_HPP

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer/flush_handler.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "db/transaction.hpp"
#include "mvcc/gc.hpp"
#include "recovery/wal.hpp"
#include "storage/table.hpp"

namespace rawdb
{

class Database
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    auto operator=(const Database &) -> Database & = delete;

    auto open(const std::filesystem::path &path,
              std::optional<uint64_t> recovery_target_time_ms = std::nullopt) -> Status;

    void close();

    auto create_table(const std::string &name, Schema schema) -> StatusOr<TableId>;

    auto insert(TableId table_id,
                const std::vector<ColumnData> &columns,
                const std::shared_ptr<Transaction> &txn = nullptr) -> StatusOr<RowId>;
    auto insert_batch(TableId table_id,
                      const std::vector<std::vector<ColumnData>> &rows,
                      const std::shared_ptr<Transaction> &txn = nullptr)
        -> StatusOr<std::vector<RowId>>;

    auto delete_rows(TableId table_id,
                     std::vector<RowId> row_ids,
                     const std::shared_ptr<Transaction> &txn = nullptr) -> Status;

    /// Compact table: remove tombstoned rows and rebuild indexes.
    auto vacuum(TableId table_id) -> Status;

    auto checkpoint() -> Status;

    auto start_backup(const std::filesystem::path &dest_path) -> Status;

    [[nodiscard]] auto table(TableId id) -> Table &
    {
        std::shared_lock lock(tables_mtx_);
        if (id >= tables_.size()) {
            throw std::out_of_range("Database::table: id=" + std::to_string(id) +
                                    " >= size=" + std::to_string(tables_.size()));
        }
        return tables_[id];
    }
    [[nodiscard]] auto table_count() const -> size_t
    {
        std::shared_lock lock(tables_mtx_);
        return tables_.size();
    }

    [[nodiscard]] auto tables_lock_shared() const { return std::shared_lock(tables_mtx_); }
    [[nodiscard]] auto tables_lock_unique() { return std::unique_lock(tables_mtx_); }

    [[nodiscard]] auto tables() -> std::deque<Table> & { return tables_; }
    [[nodiscard]] auto tables() const -> const std::deque<Table> & { return tables_; }

    [[nodiscard]] auto path() const -> const std::filesystem::path & { return path_; }

    auto next_ts() -> Timestamp { return timestamps_.allocate_ts(); }

    auto txn_manager() -> TransactionManager & { return *txn_manager_; }
    auto wal() -> WalWriter & { return wal_writer_; }

    void set_backup_interval(uint32_t seconds) { backup_interval_seconds_ = seconds; }
    void set_backup_retention(uint32_t seconds) { backup_retention_seconds_ = seconds; }
    [[nodiscard]] auto backup_interval_seconds() const -> uint32_t
    {
        return backup_interval_seconds_;
    }
    [[nodiscard]] auto backup_retention_seconds() const -> uint32_t
    {
        return backup_retention_seconds_;
    }

private:
    std::filesystem::path path_;
    mutable std::shared_mutex tables_mtx_;
    std::deque<Table> tables_;
    std::unique_ptr<FlushHandler> flush_handler_;
    TimestampAllocator timestamps_;
    GlobalWatermarks watermarks_;
    std::unique_ptr<GarbageCollector> gc_;
    std::unique_ptr<TransactionManager> txn_manager_;
    WalWriter wal_writer_;

    // Background Checkpointer
    void checkpointer_thread_func();
    std::unique_ptr<std::thread> checkpointer_thread_;
    std::atomic<bool> stop_checkpointer_{false};
    std::mutex checkpointer_mtx_;
    std::condition_variable checkpointer_cv_;
    std::mutex checkpoint_exec_mtx_;
    std::atomic<bool> backup_in_progress_{false};
    std::mutex fpw_mtx_;

    // Auto-Backup Scheduler
    uint32_t backup_interval_seconds_{0};
    uint32_t backup_retention_seconds_{0};
    void auto_backup_thread_func();
    void cleanup_old_backups();
    std::unique_ptr<std::thread> auto_backup_thread_;
    std::atomic<bool> stop_auto_backup_{false};
    std::mutex auto_backup_mtx_;
    std::condition_variable auto_backup_cv_;

    struct LoggedPageKey
    {
        TableId table_id;
        PageId page_id;
        int32_t file_id;
        bool operator==(const LoggedPageKey &o) const
        {
            return table_id == o.table_id && page_id == o.page_id && file_id == o.file_id;
        }
    };
    struct LoggedPageKeyHash
    {
        size_t operator()(const LoggedPageKey &k) const
        {
            return std::hash<uint64_t>{}(k.page_id) ^ std::hash<uint32_t>{}(k.table_id) ^
                   std::hash<int32_t>{}(k.file_id);
        }
    };
    std::unordered_set<LoggedPageKey, LoggedPageKeyHash> logged_pages_;

    void fpw_callback(TableId table_id,
                      PageId page_id,
                      int32_t file_id,
                      const std::byte *data,
                      size_t size);

    bool is_open_{false};
};

} // namespace rawdb

#endif // RAWDB_DB_DATABASE_HPP
