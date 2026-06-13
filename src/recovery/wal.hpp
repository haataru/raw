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

enum class WalRecordType : uint8_t {
    kBegin = 1,
    kCommit = 2,
    kRollback = 3,
    kInsert = 4,
    kDelete = 5
};

struct WalRecordHeader {
    uint32_t magic; // "WALR" = 0x57414C52
    Lsn lsn;
    TxId tx_id;
    WalRecordType type;
    uint32_t payload_size;
    uint32_t checksum;
};

class WalWriter {
public:
    WalWriter() = default;
    ~WalWriter() { close(); }

    auto open(const std::filesystem::path& db_path) -> Status;
    void close();

    void remove_segments_before(Lsn safe_lsn);
    auto current_lsn() const -> Lsn;

    auto append_begin(TxId tx_id) -> Lsn;
    auto append_commit(TxId tx_id) -> Lsn;
    auto append_rollback(TxId tx_id) -> Lsn;
    
    // For simplicity, we just pass raw serialized rows, or we serialize inside.
    auto append_insert(TxId tx_id, TableId table_id, const std::vector<ColumnData>& columns) -> Lsn;
    auto append_delete(TxId tx_id, TableId table_id, const std::vector<RowId>& row_ids) -> Lsn;

    auto flush() -> Status;

private:
    auto append_record(TxId tx_id, WalRecordType type, const std::vector<std::byte>& payload) -> Lsn;
    void rotate_if_needed();

    std::filesystem::path wal_dir_;
    mutable std::mutex mtx_;
    std::ofstream file_;
    Lsn next_lsn_{1};
    Lsn current_segment_start_lsn_{1};
    size_t current_segment_size_{0};
};

// WalReader is used during Recovery to read log sequentially
class WalReader {
public:
    auto open(const std::filesystem::path& db_path) -> Status;
    void close();

    struct Record {
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
