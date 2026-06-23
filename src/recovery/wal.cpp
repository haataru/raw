#include "recovery/wal.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "storage/page.hpp" // for compute_page_checksum

namespace rawdb
{

auto WalWriter::open(const std::filesystem::path& db_path) -> Status
{
    wal_dir_ = db_path / "wal";
    std::filesystem::create_directories(wal_dir_);
    
    std::vector<std::filesystem::path> segments;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            segments.push_back(entry.path());
        }
    }
    std::sort(segments.begin(), segments.end());
    
    if (segments.empty()) {
        current_segment_start_lsn_ = 1;
        next_lsn_ = 1;
    } else {
        auto last_segment = segments.back();
        std::string filename = last_segment.stem().string();
        current_segment_start_lsn_ = std::stoull(filename);
        
        // Read the last segment to find next_lsn_
        std::ifstream f(last_segment, std::ios::binary);
        while (f) {
            WalRecordHeader hdr;
            if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
                if (hdr.magic == 0x57414C52 && hdr.lsn >= next_lsn_) {
                    next_lsn_ = hdr.lsn + 1;
                }
                f.seekg(hdr.payload_size, std::ios::cur);
            }
        }
    }
    
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "%020llu.log", static_cast<unsigned long long>(current_segment_start_lsn_));
    auto current_path = wal_dir_ / name_buf;
    file_.open(current_path, std::ios::binary | std::ios::app);
    if (!file_) return Status::kIoError;
    
    current_segment_size_ = static_cast<size_t>(file_.tellp());
    
    return Status::kOk;
}

void WalWriter::close()
{
    std::lock_guard lock(mtx_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void WalWriter::rotate_if_needed()
{
    constexpr size_t kMaxSegmentSize = 64 * 1024 * 1024;
    if (file_.is_open() && current_segment_size_ >= kMaxSegmentSize) {
        file_.flush();
        file_.close();
        
        current_segment_start_lsn_ = next_lsn_;
        char name_buf[32];
        std::snprintf(name_buf, sizeof(name_buf), "%020llu.log", static_cast<unsigned long long>(current_segment_start_lsn_));
        file_.open(wal_dir_ / name_buf, std::ios::binary | std::ios::app);
        current_segment_size_ = 0;
    }
}

void WalWriter::remove_segments_before(Lsn safe_lsn)
{
    std::lock_guard lock(mtx_);
    std::vector<std::filesystem::path> segments;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            segments.push_back(entry.path());
        }
    }
    std::sort(segments.begin(), segments.end());
    
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i + 1 < segments.size()) {
            std::string next_filename = segments[i+1].stem().string();
            Lsn next_start_lsn = std::stoull(next_filename);
            if (next_start_lsn <= safe_lsn) {
                std::error_code ec;
                std::filesystem::remove(segments[i], ec);
            }
        }
    }
}

auto WalWriter::current_lsn() const -> Lsn
{
    std::lock_guard lock(mtx_);
    return next_lsn_;
}

auto WalWriter::append_record(TxId tx_id, WalRecordType type, const std::vector<std::byte>& payload) -> StatusOr<Lsn>
{
    std::lock_guard lock(mtx_);
    rotate_if_needed();
    
    WalRecordHeader hdr;
    hdr.magic = 0x57414C52;
    hdr.lsn = next_lsn_;
    hdr.tx_id = tx_id;
    hdr.type = type;
    hdr.payload_size = static_cast<uint32_t>(payload.size());
    hdr.checksum = compute_page_checksum(payload.data(), payload.size());
    
    auto old_pos = file_.tellp();

    file_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!payload.empty()) {
        file_.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    if (file_.fail() || file_.bad()) {
        file_.clear();
        file_.seekp(old_pos); // Revert write
        return std::unexpected(Status{Status::kNoSpace, "failed to append wal record: no space left on device"});
    }
    
    next_lsn_++;
    current_segment_size_ += sizeof(hdr) + payload.size();
    
    return hdr.lsn;
}

auto WalWriter::append_begin(TxId tx_id) -> StatusOr<Lsn>
{
    return append_record(tx_id, WalRecordType::kBegin, {});
}

auto WalWriter::append_commit(TxId tx_id) -> StatusOr<Lsn>
{
    return append_record(tx_id, WalRecordType::kCommit, {});
}

auto WalWriter::append_rollback(TxId tx_id) -> StatusOr<Lsn>
{
    return append_record(tx_id, WalRecordType::kRollback, {});
}

auto WalWriter::append_insert(TxId tx_id, TableId table_id, const std::vector<ColumnData>& columns) -> StatusOr<Lsn>
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

auto WalWriter::append_insert_batch(TxId tx_id, TableId table_id, const std::vector<std::vector<ColumnData>>& rows) -> StatusOr<Lsn>
{
    if (rows.empty()) {
        std::lock_guard lock(mtx_);
        return next_lsn_ - 1;
    }

    std::vector<std::byte> payload;
    auto put = [&](const void* d, size_t n) {
        auto* p = static_cast<const std::byte*>(d);
        payload.insert(payload.end(), p, p + n);
    };
    
    put(&table_id, sizeof(table_id));
    uint32_t row_count = static_cast<uint32_t>(rows.size());
    put(&row_count, sizeof(row_count));
    uint32_t col_count = static_cast<uint32_t>(rows[0].size());
    put(&col_count, sizeof(col_count));
    
    for (const auto& row : rows) {
        for (const auto& col : row) {
            uint8_t t = static_cast<uint8_t>(col.type);
            put(&t, sizeof(t));
            uint32_t s = static_cast<uint32_t>(col.size);
            put(&s, sizeof(s));
            put(col.data, col.size);
        }
    }
    
    return append_record(tx_id, WalRecordType::kInsertBatch, payload);
}

auto WalWriter::append_delete(TxId tx_id, TableId table_id, const std::vector<RowId>& row_ids) -> StatusOr<Lsn>
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
    wal_dir_ = db_path / "wal";
    if (std::filesystem::exists(wal_dir_)) {
        for (const auto& entry : std::filesystem::directory_iterator(wal_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                segments_.push_back(entry.path());
            }
        }
        std::sort(segments_.begin(), segments_.end());
    }
    
    if (segments_.empty()) return Status::kNotFound;
    
    current_segment_idx_ = 0;
    return open_next_segment() ? Status::kOk : Status::kNotFound;
}

void WalReader::close()
{
    if (file_.is_open()) file_.close();
}

auto WalReader::open_next_segment() -> bool
{
    if (file_.is_open()) file_.close();
    
    while (current_segment_idx_ < segments_.size()) {
        file_.open(segments_[current_segment_idx_], std::ios::binary);
        current_segment_idx_++;
        if (file_.is_open()) return true;
    }
    return false;
}

auto WalReader::next() -> StatusOr<Record>
{
    while (true) {
        if (!file_.is_open() || file_.eof()) {
            if (!open_next_segment()) {
                return std::unexpected(Status::kNotFound);
            }
            continue;
        }

        Record rec;
        file_.read(reinterpret_cast<char*>(&rec.header), sizeof(rec.header));
        if (file_.gcount() < static_cast<std::streamsize>(sizeof(rec.header))) {
            if (file_.gcount() == 0) {
                if (!open_next_segment()) {
                    return std::unexpected(Status::kNotFound);
                }
                continue;
            }
            // Corrupt file (incomplete header)
            return std::unexpected(Status::kCorruptedData);
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
}

} // namespace rawdb
