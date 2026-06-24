#ifndef RAWDB_RECOVERY_WAL_HPP
#define RAWDB_RECOVERY_WAL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"

namespace rawdb
{

enum class WalRecordType : uint8_t
{
    kBegin = 1,
    kCommit = 2,
    kRollback = 3,
    kInsert = 4,
    kDelete = 5,
    kInsertBatch = 6,
    kFullPageImage = 7
};

struct WalRecordHeader
{
    uint32_t magic; // "WALR" = 0x57414C52
    Lsn lsn;
    TxId tx_id;
    WalRecordType type;
    uint32_t payload_size;
    uint32_t checksum;
    uint64_t physical_time_ms;
};

class WalWriter
{
public:
    WalWriter() = default;
    ~WalWriter() { close(); }

    auto open(const std::filesystem::path &db_path) -> Status;
    void close();

    void remove_segments_before(Lsn safe_lsn);
    auto current_lsn() const -> Lsn;

    auto append_begin(TxId tx_id) -> StatusOr<Lsn>;
    auto append_commit(TxId tx_id, Timestamp commit_ts) -> StatusOr<Lsn>;
    auto append_rollback(TxId tx_id) -> StatusOr<Lsn>;

    // For simplicity, we just pass raw serialized rows, or we serialize inside.
    auto append_insert(TxId tx_id,
                       TableId table_id,
                       const std::vector<ColumnData> &columns) -> StatusOr<Lsn>;
    auto append_insert_batch(TxId tx_id,
                             TableId table_id,
                             const std::vector<std::vector<ColumnData>> &rows) -> StatusOr<Lsn>;
    auto append_delete(TxId tx_id,
                       TableId table_id,
                       const std::vector<RowId> &row_ids) -> StatusOr<Lsn>;

    // FPW: Full Page Write (table_id indicates table, page_id indicates offset, file_id: -1 for
    // data, >=0 for index)
    auto append_full_page(TableId table_id,
                          PageId page_id,
                          int32_t file_id,
                          const std::byte *data,
                          size_t size) -> StatusOr<Lsn>;

    auto flush() -> Status;

private:
    auto append_record(TxId tx_id,
                       WalRecordType type,
                       const std::vector<std::byte> &payload) -> StatusOr<Lsn>;
    void rotate_if_needed();

    std::filesystem::path wal_dir_;
    mutable std::mutex mtx_;
    std::ofstream file_;
    Lsn next_lsn_{1};
    Lsn current_segment_start_lsn_{1};
    size_t current_segment_size_{0};
};

// WalReader is used during Recovery to read log sequentially
class WalReader
{
public:
    auto open(const std::filesystem::path &db_path) -> Status;
    void close();

    struct Record
    {
        WalRecordHeader header;
        std::vector<std::byte> payload;
    };

    auto next() -> StatusOr<Record>;

private:
    auto open_next_segment() -> bool;

    std::filesystem::path wal_dir_;
    std::vector<std::filesystem::path> segments_;
    size_t current_segment_idx_{0};
    std::ifstream file_;
};

} // namespace rawdb

#endif // RAWDB_RECOVERY_WAL_HPP
