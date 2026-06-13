#include "recovery/wal.hpp"

#include <cstring>
#include <iostream>

#include "storage/page.hpp" // for compute_page_checksum

namespace rawdb
{

auto WalWriter::open(const std::filesystem::path& db_path) -> Status
{
    path_ = db_path / "rawdb.wal";
    
    // Scan existing WAL to find max LSN
    WalReader reader;
    if (reader.open(db_path) == Status::kOk) {
        while (true) {
            auto r = reader.next();
            if (!r) break;
            if (r->header.lsn >= next_lsn_) {
                next_lsn_ = r->header.lsn + 1;
            }
        }
        reader.close();
    }
    
    file_.open(path_, std::ios::binary | std::ios::app);
    if (!file_) return Status::kIoError;
    
    return Status::kOk;
}

void WalWriter::close()
{
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

auto WalWriter::append_record(TxId tx_id, WalRecordType type, const std::vector<std::byte>& payload) -> Lsn
{
    std::lock_guard lock(mtx_);
    
    WalRecordHeader hdr;
    hdr.magic = 0x57414C52;
    hdr.lsn = next_lsn_++;
    hdr.tx_id = tx_id;
    hdr.type = type;
    hdr.payload_size = static_cast<uint32_t>(payload.size());
    hdr.checksum = compute_page_checksum(payload.data(), payload.size());
    
    file_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!payload.empty()) {
        file_.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    
    return hdr.lsn;
}

auto WalWriter::append_begin(TxId tx_id) -> Lsn
{
    return append_record(tx_id, WalRecordType::kBegin, {});
}

auto WalWriter::append_commit(TxId tx_id) -> Lsn
{
    return append_record(tx_id, WalRecordType::kCommit, {});
}

auto WalWriter::append_rollback(TxId tx_id) -> Lsn
{
    return append_record(tx_id, WalRecordType::kRollback, {});
}

auto WalWriter::append_insert(TxId tx_id, TableId table_id, const std::vector<ColumnData>& columns) -> Lsn
{
    std::vector<std::byte> payload;
    auto put = [&](const void* d, size_t n) {
        auto* p = static_cast<const std::byte*>(d);
        payload.insert(payload.end(), p, p + n);
    };
    
    put(&table_id, sizeof(table_id));
    uint32_t col_count = static_cast<uint32_t>(columns.size());
    put(&col_count, sizeof(col_count));
    
    for (const auto& col : columns) {
        uint8_t t = static_cast<uint8_t>(col.type);
        put(&t, sizeof(t));
        uint32_t s = static_cast<uint32_t>(col.size);
        put(&s, sizeof(s));
        put(col.data, col.size);
    }
    
    return append_record(tx_id, WalRecordType::kInsert, payload);
}

auto WalWriter::append_delete(TxId tx_id, TableId table_id, const std::vector<RowId>& row_ids) -> Lsn
{
    std::vector<std::byte> payload;
    auto put = [&](const void* d, size_t n) {
        auto* p = static_cast<const std::byte*>(d);
        payload.insert(payload.end(), p, p + n);
    };
    
    put(&table_id, sizeof(table_id));
    uint32_t row_count = static_cast<uint32_t>(row_ids.size());
    put(&row_count, sizeof(row_count));
    
    for (RowId rid : row_ids) {
        put(&rid, sizeof(rid));
    }
    
    return append_record(tx_id, WalRecordType::kDelete, payload);
}

auto WalWriter::flush() -> Status
{
    std::lock_guard lock(mtx_);
    if (!file_) return Status::kIoError;
    file_.flush();
    return file_.good() ? Status::kOk : Status::kIoError;
}

// ── WalReader ──

auto WalReader::open(const std::filesystem::path& db_path) -> Status
{
    file_.open(db_path / "rawdb.wal", std::ios::binary);
    if (!file_) return Status::kNotFound;
    return Status::kOk;
}

void WalReader::close()
{
    if (file_.is_open()) file_.close();
}

auto WalReader::next() -> StatusOr<Record>
{
    if (!file_.is_open() || file_.eof()) return std::unexpected(Status::kNotFound);

    Record rec;
    file_.read(reinterpret_cast<char*>(&rec.header), sizeof(rec.header));
    if (file_.gcount() < static_cast<std::streamsize>(sizeof(rec.header))) {
        return std::unexpected(Status::kNotFound);
    }

    if (rec.header.magic != 0x57414C52) {
        return std::unexpected(Status::kCorruptedData);
    }

    if (rec.header.payload_size > 0) {
        rec.payload.resize(rec.header.payload_size);
        file_.read(reinterpret_cast<char*>(rec.payload.data()), rec.header.payload_size);
        if (file_.gcount() < static_cast<std::streamsize>(rec.header.payload_size)) {
            return std::unexpected(Status::kCorruptedData);
        }
        
        uint32_t csum = compute_page_checksum(rec.payload.data(), rec.payload.size());
        if (csum != rec.header.checksum) {
            return std::unexpected(Status::kCorruptedData);
        }
    }

    return rec;
}

} // namespace rawdb
