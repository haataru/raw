#ifndef RAWDB_DB_DATABASE_HPP
#define RAWDB_DB_DATABASE_HPP

#include <deque>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
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

    auto open(const std::filesystem::path &path) -> Status;

    void close();

    auto create_table(const std::string &name, Schema schema) -> StatusOr<TableId>;

    auto insert(TableId table_id, const std::vector<ColumnData> &columns, const std::shared_ptr<Transaction>& txn = nullptr) -> StatusOr<RowId>;

    auto delete_rows(TableId table_id, std::vector<RowId> row_ids, const std::shared_ptr<Transaction>& txn = nullptr) -> Status;

    /// Compact table: remove tombstoned rows and rebuild indexes.
    auto vacuum(TableId table_id) -> Status;

    auto checkpoint() -> Status;

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

    auto txn_manager() -> TransactionManager& { return *txn_manager_; }
    auto wal() -> WalWriter& { return wal_writer_; }

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
    
    bool is_open_{false};
};

} // namespace rawdb

#endif // RAWDB_DB_DATABASE_HPP
