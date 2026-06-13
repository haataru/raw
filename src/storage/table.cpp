#include "storage/table.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/config.hpp"
#include "query/executor.hpp"

namespace rawdb
{

Table::Table(std::string name, Schema schema)
    : name_(std::move(name)), schema_(std::move(schema))
{
    pending_.col_data.resize(schema_.column_count());
}

Table::Table(Table &&other) noexcept
    : rw_mtx(std::move(other.rw_mtx)),
      name_(std::move(other.name_)),
      schema_(std::move(other.schema_)),
      row_count_(other.row_count_),
      file_(std::move(other.file_)),
      file_open_(other.file_open_),
      version_index_(std::move(other.version_index_)),
      current_lsn_(other.current_lsn_.load(std::memory_order_relaxed)),
      pages_(std::move(other.pages_)),
      indexes_(std::move(other.indexes_)),
      pending_(std::move(other.pending_))
{
}

auto Table::operator=(Table &&other) noexcept -> Table &
{
    if (this != &other) {
        name_ = std::move(other.name_);
        schema_ = std::move(other.schema_);
        row_count_ = other.row_count_;
        file_ = std::move(other.file_);
        file_open_ = other.file_open_;
        version_index_ = std::move(other.version_index_);
        current_lsn_.store(other.current_lsn_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        pages_ = std::move(other.pages_);
        indexes_ = std::move(other.indexes_);
        pending_ = std::move(other.pending_);
        rw_mtx = std::move(other.rw_mtx);
    }
    return *this;
}

auto Table::row_count() const -> size_t
{
    std::shared_lock lock(*rw_mtx);
    return row_count_;
}

auto Table::insert_row(Timestamp ts, const std::vector<ColumnData> &columns) -> StatusOr<RowId>
{
    if (columns.size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::unique_lock lock(*rw_mtx);

    if (pending_.row_count == 0) {
        pending_.start_rid = row_count_;
    }
    pending_.row_ts.push_back(ts);

    for (size_t i = 0; i < columns.size(); ++i) {
        auto *src = static_cast<const std::byte *>(columns[i].data);
        pending_.col_data[i].insert(pending_.col_data[i].end(), src, src + columns[i].size);
    }
    RowId inserted_rid = row_count_;
    ++pending_.row_count;
    ++row_count_;

    if (pending_.row_count >= kBatchSize || pending_.row_count >= config::kMaxPendingRows) {
        write_pending_to_page();
    }

    return inserted_rid;
}

auto Table::insert_row(TimestampAllocator &timestamps,
                       const std::vector<ColumnData> &columns) -> StatusOr<RowId>
{
    if (columns.size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::unique_lock lock(*rw_mtx);

    Timestamp ts = timestamps.allocate_ts();
    if (pending_.row_count == 0) {
        pending_.start_rid = row_count_;
    }
    pending_.row_ts.push_back(ts);

    for (size_t i = 0; i < columns.size(); ++i) {
        auto *src = static_cast<const std::byte *>(columns[i].data);
        pending_.col_data[i].insert(pending_.col_data[i].end(), src, src + columns[i].size);
    }
    RowId inserted_rid = row_count_;
    ++pending_.row_count;
    ++row_count_;

    if (pending_.row_count >= kBatchSize || pending_.row_count >= config::kMaxPendingRows) {
        write_pending_to_page();
    }

    return inserted_rid;
}

void Table::flush_pending()
{
    std::unique_lock lock(*rw_mtx);
    write_pending_to_page();
}

void Table::write_pending_to_page()
{
    size_t row_count = pending_.row_count;
    if (row_count == 0)
        return;

    size_t col_count = schema_.column_count();
    size_t hdr_size = sizeof(BatchHeader) + col_count * sizeof(ColMeta);

    // Compute per-column page sizes (VARCHAR needs cumulative offsets)
    std::vector<size_t> col_page_sizes(col_count);
    size_t total_data_size = 0;
    for (size_t ci = 0; ci < col_count; ++ci) {
        if (schema_.columns[ci] == ColumnType::kVarChar) {
            size_t blob_size = 0;
            size_t pos = 0;
            const auto &src = pending_.col_data[ci];
            for (size_t r = 0; r < row_count && pos + sizeof(uint32_t) <= src.size(); ++r) {
                uint32_t len;
                std::memcpy(&len, src.data() + pos, sizeof(uint32_t));
                pos += sizeof(uint32_t) + len;
                blob_size += len;
            }
            col_page_sizes[ci] = row_count * sizeof(uint32_t) + blob_size;
        }
        else {
            col_page_sizes[ci] = pending_.col_data[ci].size();
        }
        total_data_size += col_page_sizes[ci];
    }

    size_t bm_bytes = (row_count + 7) / 8;
    size_t total_bm_size = col_count * bm_bytes;
    size_t payload_size = hdr_size + total_data_size + total_bm_size;

    PageId page_id = file_.size();
    size_t total_size = PageHeader::kSize + payload_size;
    file_.resize(page_id + total_size);

    auto *base = file_.data() + page_id;
    auto &hdr = *reinterpret_cast<PageHeader *>(base);
    hdr.magic = kPageMagic;
    hdr.version = 0;
    hdr.type = PageType::Data;
    hdr.reserved = 0;
    hdr.row_count = static_cast<uint32_t>(row_count);
    hdr.data_size = static_cast<uint32_t>(payload_size);

    std::byte *payload = base + PageHeader::kSize;

    auto *bh = reinterpret_cast<BatchHeader *>(payload);
    bh->table_id = 0;
    bh->row_count = row_count;
    bh->col_count = col_count;
    bh->last_applied_lsn = current_lsn_.load(std::memory_order_acquire);

    size_t off = hdr_size;
    size_t null_off = hdr_size + total_data_size;
    for (size_t ci = 0; ci < col_count; ++ci) {
        ColMeta &meta = reinterpret_cast<ColMeta *>(payload + sizeof(BatchHeader))[ci];
        meta.type = schema_.columns[ci];
        meta.data_off = off;
        meta.data_size = col_page_sizes[ci];
        meta.nulls_off = null_off;

        if (schema_.columns[ci] == ColumnType::kVarChar) {
            // Reformat: [len0, data0, len1, data1, ...] → [cum_off0, cum_off1, ..., data0, data1,
            // ...]
            std::vector<uint32_t> cum_offsets(row_count);
            size_t pos = 0;
            size_t blob_start = off + row_count * sizeof(uint32_t);
            size_t blob_pos = blob_start;
            for (size_t r = 0; r < row_count; ++r) {
                uint32_t len;
                std::memcpy(&len, pending_.col_data[ci].data() + pos, sizeof(uint32_t));
                pos += sizeof(uint32_t);
                cum_offsets[r] = (r == 0) ? len : cum_offsets[r - 1] + len;
                std::memcpy(payload + blob_pos, pending_.col_data[ci].data() + pos, len);
                pos += len;
                blob_pos += len;
            }
            std::memcpy(payload + off, cum_offsets.data(), row_count * sizeof(uint32_t));
            off = blob_pos;
        }
        else {
            std::memcpy(payload + off, pending_.col_data[ci].data(), col_page_sizes[ci]);
            off += col_page_sizes[ci];
        }

        std::memset(payload + null_off, 0, bm_bytes);
        null_off += bm_bytes;
    }

    set_page_checksum(hdr, payload, payload_size);

    RowId start_rid = pending_.start_rid;
    pages_.push_back({start_rid, row_count, page_id});

    std::vector<IndexEntry> entries;
    entries.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        Timestamp row_ts = (i < pending_.row_ts.size()) ? pending_.row_ts[i] : 0;
        entries.push_back({start_rid + static_cast<RowId>(i), row_ts, page_id, 0});
    }
    version_index_.insert_bulk(entries.data(), entries.size());

    // Durability: sync page to disk immediately
    file_.msync_sync_safe();

    for (auto &cd : pending_.col_data) {
        cd.clear();
    }
    pending_.row_ts.clear();
    pending_.row_count = 0;
}

auto Table::open_file(const std::filesystem::path &db_path) -> Status
{
    if (file_open_)
        return Status::kOk;

    try {
        file_.open(db_path / (name_ + ".raw"));
        file_open_ = true;
        return Status::kOk;
    }
    catch (...) {
        return Status::kIoError;
    }
}

void Table::close_file()
{
    if (file_open_) {
        file_.close();
        file_open_ = false;
    }
}

auto Table::recover() -> Status
{
    if (!file_open_ || file_.size() == 0) {
        return Status::kOk;
    }

    size_t file_sz = file_.size();
    PageId off = 0;
    RowId start_row = 0;

    while (off + PageHeader::kSize <= file_sz) {
        auto &hdr = *reinterpret_cast<const PageHeader *>(file_.data() + off);
        if (hdr.magic != kPageMagic) {
            if (off > 0) {
                file_.resize(off);
            }
            break;
        }

        size_t total = PageHeader::kSize + hdr.data_size;
        if (off + total > file_sz) {
            file_.resize(off);
            break;
        }

        auto *payload = file_.data() + off + PageHeader::kSize;
        if (validate_page(hdr, payload) != Status::kOk) {
            file_.resize(off);
            break;
        }

        RowRange range{};
        range.start = start_row;
        range.count = hdr.row_count;
        range.page_id = off;
        pages_.push_back(range);

        start_row += hdr.row_count;
        off += static_cast<PageId>(total);
    }

    row_count_ = start_row;
    return Status::kOk;
}

auto Table::version_index_size() const -> size_t
{
    std::shared_lock lock(*rw_mtx);
    return version_index_.size();
}

auto Table::version_index_max_ts() const -> Timestamp
{
    std::shared_lock lock(*rw_mtx);
    return version_index_.max_timestamp();
}

void Table::insert_version_entries(const IndexEntry *entries, size_t count)
{
    std::unique_lock lock(*rw_mtx);
    version_index_.insert_bulk(entries, count);
}

void Table::commit_rows(const std::vector<RowId>& row_ids, TxId tx_id, Timestamp commit_ts)
{
    std::shared_lock lock(*rw_mtx);
    version_index_.commit_rows(row_ids, tx_id, commit_ts);
}

auto Table::search_version_index(RowId row_id, Timestamp max_ts) const -> StatusOr<uint64_t>
{
    std::shared_lock lock(*rw_mtx);
    return version_index_.search(row_id, max_ts);
}

auto Table::has_indexes() const -> bool
{
    std::shared_lock lock(*rw_mtx);
    return !indexes_.empty();
}

void Table::add_index(IndexInfo info)
{
    std::unique_lock lock(*rw_mtx);
    indexes_.push_back(std::move(info));
}

auto Table::save_vindex(const std::filesystem::path &db_path) -> Status
{
    std::shared_lock lock(*rw_mtx);
    auto path = db_path / (name_ + ".vindex");
    std::vector<std::byte> buf;
    version_index_.serialize(buf);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return Status::kIoError;
    f.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return f.good() ? Status::kOk : Status::kIoError;
}

auto Table::load_vindex(const std::filesystem::path &db_path) -> Status
{
    auto path = db_path / (name_ + ".vindex");
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return Status::kNotFound;
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(sz);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));
    auto r = version_index_.deserialize(buf.data(), buf.size());
    if (!r)
        return r.error();
    return Status::kOk;
}

void Table::rebuild_vindex_from_pages(TimestampAllocator &timestamps)
{
    version_index_ = VersionIndex{};
    std::vector<IndexEntry> entries;
    for (auto &p : pages_) {
        Timestamp page_ts = timestamps.allocate_ts();
        for (size_t k = 0; k < p.count; ++k) {
            entries.push_back({p.start + static_cast<RowId>(k), page_ts, p.page_id, 0});
        }
    }
    if (!entries.empty()) {
        version_index_.insert_bulk(entries.data(), entries.size());
    }
}

auto Table::sync() -> Status
{
    std::shared_lock lock(*rw_mtx);
    if (!file_.is_open() || file_.size() == 0) {
        return Status::kOk;
    }
    return file_.msync_sync_safe();
}

auto Table::prune_version_index(Timestamp cutoff_ts) -> size_t
{
    std::unique_lock lock(*rw_mtx);
    return version_index_.prune(cutoff_ts);
}

} // namespace rawdb
