#ifndef RAWDB_STORAGE_TABLE_HPP
#define RAWDB_STORAGE_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <queue>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"
#include "index/btree.hpp"
#include "memory/mmap_file.hpp"
#include "mvcc/version_index.hpp"
#include "storage/page.hpp"

namespace rawdb
{

class FlushHandler;

struct Schema
{
    std::vector<ColumnType> columns;
    std::vector<std::string> names;

    [[nodiscard]] auto column_count() const -> size_t { return columns.size(); }
};

struct TableScanResult;

class Table
{
public:
    Table() = default;

    explicit Table(std::string name, Schema schema);

    Table(const Table &) = delete;
    auto operator=(const Table &) -> Table & = delete;
    Table(Table &&other) noexcept;
    auto operator=(Table &&other) noexcept -> Table &;

    [[nodiscard]] auto name() const -> const std::string & { return name_; }
    [[nodiscard]] auto schema() const -> const Schema & { return schema_; }
    [[nodiscard]] auto row_count() const -> size_t;

    auto insert_row(Timestamp ts, const std::vector<ColumnData> &columns) -> StatusOr<RowId>;

    auto insert_row(TimestampAllocator &timestamps,
                    const std::vector<ColumnData> &columns) -> StatusOr<RowId>;

    auto insert_rows(Timestamp ts, const std::vector<std::vector<ColumnData>> &rows)
        -> StatusOr<std::vector<RowId>>;
    auto insert_rows(TimestampAllocator &timestamps,
                     const std::vector<std::vector<ColumnData>> &rows)
        -> StatusOr<std::vector<RowId>>;

    auto flush_pending() -> Status;

    // ── Page read ──

    [[nodiscard]] auto read_page(PageId page_id) const -> StatusOr<std::vector<std::byte>>;
    [[nodiscard]] auto read_page_header(PageId page_id) const -> StatusOr<PageHeader>;

    [[nodiscard]] auto lookup_page(RowId row_id) const -> StatusOr<PageId>;

    auto open_file(const std::filesystem::path &db_path) -> Status;
    void close_file();

    [[nodiscard]] auto recover() -> Status;

    /// Indexes must be rebuilt after this call (RowIds change).
    auto prune_version_index(Timestamp cutoff_ts) -> size_t;

    auto read_rows(const std::vector<RowId> &row_ids,
                   const std::vector<size_t> &col_indices) -> StatusOr<TableScanResult>;

    /// Locked VersionIndex operations.
    [[nodiscard]] auto search_version_index(RowId row_id,
                                            Timestamp max_ts) const -> StatusOr<uint64_t>;
    [[nodiscard]] auto version_index_size() const -> size_t;
    [[nodiscard]] auto version_index_max_ts() const -> Timestamp;
    auto insert_version_entries(const IndexEntry *entries, size_t count) -> void;
    void commit_rows(const std::vector<RowId> &row_ids, TxId tx_id, Timestamp commit_ts);

    [[nodiscard]] auto file() -> MmapFile & { return file_; }
    [[nodiscard]] auto file() const -> const MmapFile & { return file_; }

    auto sync() -> Status;

    auto save_vindex(const std::filesystem::path &db_path) -> Status;

    auto load_vindex(const std::filesystem::path &db_path) -> Status;

    void rebuild_vindex_from_pages(TimestampAllocator &timestamps);

    /// Indexes must be rebuilt after this call (RowIds change).
    auto vacuum(TimestampAllocator &timestamps) -> StatusOr<size_t>;

    void clear_indexes();

    /// Lock for concurrent access (shared_mutex).
    mutable std::unique_ptr<std::shared_mutex> rw_mtx{std::make_unique<std::shared_mutex>()};
    [[nodiscard]] auto lock_shared() const { return std::shared_lock(*rw_mtx); }
    [[nodiscard]] auto lock_unique() { return std::unique_lock(*rw_mtx); }

    [[nodiscard]] auto has_indexes() const -> bool;

    void set_lsn(Lsn lsn) { current_lsn_.store(lsn, std::memory_order_release); }
    [[nodiscard]] auto lsn() const -> Lsn { return current_lsn_.load(std::memory_order_acquire); }

    void set_flush_handler(FlushHandler *handler) { flush_handler_ = handler; }

    using FPWCallback = std::function<void(PageId, const std::byte *, size_t)>;
    void set_fpw_callback(FPWCallback cb) { fpw_callback_ = std::move(cb); }

    template <typename F>
    auto for_each_index(F &&f) -> void
    {
        std::shared_lock lock(*rw_mtx);
        for (auto &idx : indexes_) {
            f(idx);
        }
    }

    template <typename F>
    auto for_each_index(F &&f) const -> void
    {
        std::shared_lock lock(*rw_mtx);
        for (const auto &idx : indexes_) {
            f(idx);
        }
    }

    auto add_index(IndexInfo info) -> void;
    auto get_index_by_col(size_t col_idx) -> BTree *;

    static constexpr PageId kNotFoundPage = static_cast<PageId>(-1);
    static constexpr size_t kBatchSize = 8192; // max rows per page

private:
    std::string name_;
    Schema schema_;
    std::atomic<size_t> row_count_{0};
    MmapFile file_;
    bool file_open_{false};
    VersionIndex version_index_;
    std::atomic<Lsn> current_lsn_{0};

    struct RowRange
    {
        RowId start;
        size_t count;
        PageId page_id;
    };
    std::vector<RowRange> pages_;
    std::vector<IndexInfo> indexes_;

    // ── Pending batch buffer ──
    struct alignas(64) PendingBatch
    {
        std::mutex mtx;
        std::vector<std::vector<std::byte>> col_data;
        std::vector<Timestamp> row_ts;
        size_t row_count = 0;
        RowId start_rid = 0;

        PendingBatch() = default;
        PendingBatch(const PendingBatch &) = delete;
        auto operator=(const PendingBatch &) -> PendingBatch & = delete;
    };

    std::atomic<PendingBatch *> active_batch_{nullptr};
    std::vector<std::unique_ptr<PendingBatch>> free_batches_;
    std::deque<std::unique_ptr<PendingBatch>> flush_queue_;
    std::mutex pool_mtx_;
    std::condition_variable pool_cv_;

    static constexpr size_t kMaxBatches = 64;

    void init_batches();
    auto get_free_batch() -> std::unique_ptr<PendingBatch>;
    void push_flush_queue(std::unique_ptr<PendingBatch> batch);

    auto write_batch_to_page(PendingBatch *batch) -> Status;

public:
    auto pop_flush_queue() -> std::unique_ptr<PendingBatch>;
    void return_free_batch(std::unique_ptr<PendingBatch> batch);

private:
    FlushHandler *flush_handler_{nullptr};
    FPWCallback fpw_callback_;
};

} // namespace rawdb

#endif // RAWDB_STORAGE_TABLE_HPP
