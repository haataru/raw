#include "storage/table.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "buffer/flush_handler.hpp"
#include "core/config.hpp"
#include "query/executor.hpp"

namespace rawdb
{

Table::Table(std::string name, Schema schema) : name_(std::move(name)), schema_(std::move(schema))
{
    init_batches();
}

Table::Table(Table &&other) noexcept
    : rw_mtx(std::move(other.rw_mtx)),
      name_(std::move(other.name_)),
      schema_(std::move(other.schema_)),
      row_count_(other.row_count_.load()),
      file_(std::move(other.file_)),
      file_open_(other.file_open_),
      version_index_(std::move(other.version_index_)),
      current_lsn_(other.current_lsn_.load(std::memory_order_relaxed)),
      pages_(std::move(other.pages_)),
      indexes_(std::move(other.indexes_)),
      free_batches_(std::move(other.free_batches_)),
      flush_queue_(std::move(other.flush_queue_))
{
    active_batch_.store(other.active_batch_.load());
    other.active_batch_.store(nullptr);
}

auto Table::operator=(Table &&other) noexcept -> Table &
{
    if (this != &other) {
        name_ = std::move(other.name_);
        schema_ = std::move(other.schema_);
        row_count_.store(other.row_count_.load());
        file_ = std::move(other.file_);
        file_open_ = other.file_open_;
        version_index_ = std::move(other.version_index_);
        current_lsn_.store(other.current_lsn_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        pages_ = std::move(other.pages_);
        indexes_ = std::move(other.indexes_);

        active_batch_.store(other.active_batch_.load());
        other.active_batch_.store(nullptr);
        free_batches_ = std::move(other.free_batches_);
        flush_queue_ = std::move(other.flush_queue_);

        rw_mtx = std::move(other.rw_mtx);
    }
    return *this;
}

void Table::init_batches()
{
    for (size_t i = 0; i < kMaxBatches; ++i) {
        auto batch = std::make_unique<PendingBatch>();
        batch->col_data.resize(schema_.column_count());
        free_batches_.push_back(std::move(batch));
    }
}

auto Table::get_free_batch() -> std::unique_ptr<PendingBatch>
{
    std::unique_lock lock(pool_mtx_);
    pool_cv_.wait(lock, [this] { return !free_batches_.empty(); });
    auto batch = std::move(free_batches_.back());
    free_batches_.pop_back();
    return batch;
}

void Table::return_free_batch(std::unique_ptr<PendingBatch> batch)
{
    for (auto &cd : batch->col_data)
        cd.clear();
    batch->row_ts.clear();
    batch->row_count = 0;

    std::unique_lock lock(pool_mtx_);
    free_batches_.push_back(std::move(batch));
    lock.unlock();
    pool_cv_.notify_one();
}

void Table::push_flush_queue(std::unique_ptr<PendingBatch> batch)
{
    std::unique_lock lock(pool_mtx_);
    flush_queue_.push_back(std::move(batch));
    lock.unlock();
    if (flush_handler_) {
        flush_handler_->signal();
    }
}

auto Table::pop_flush_queue() -> std::unique_ptr<PendingBatch>
{
    std::unique_lock lock(pool_mtx_);
    if (flush_queue_.empty())
        return nullptr;
    auto batch = std::move(flush_queue_.front());
    flush_queue_.pop_front();
    return batch;
}

auto Table::row_count() const -> size_t { return row_count_.load(std::memory_order_acquire); }

auto Table::insert_row(Timestamp ts, const std::vector<ColumnData> &columns) -> StatusOr<RowId>
{
    if (columns.size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::shared_lock rw_lock(*rw_mtx);

    PendingBatch *active;
    while (true) {
        active = active_batch_.load(std::memory_order_acquire);
        if (!active) {
            rw_lock.unlock();
            auto new_batch = get_free_batch();
            rw_lock.lock();
            PendingBatch *expected = nullptr;
            if (active_batch_.compare_exchange_strong(expected,
                                                      new_batch.get(),
                                                      std::memory_order_release,
                                                      std::memory_order_acquire)) {
                new_batch.release();
            }
            else {
                return_free_batch(std::move(new_batch));
            }
            continue;
        }

        std::unique_lock batch_lock(active->mtx);
        if (active != active_batch_.load(std::memory_order_acquire)) {
            continue;
        }

        if (active->row_count == 0) {
            active->start_rid = row_count_.load(std::memory_order_relaxed);
        }

        RowId inserted_rid = active->start_rid + active->row_count;
        active->row_ts.push_back(ts);

        for (size_t i = 0; i < columns.size(); ++i) {
            auto *src = static_cast<const std::byte *>(columns[i].data);
            active->col_data[i].insert(active->col_data[i].end(), src, src + columns[i].size);
        }

        ++active->row_count;
        row_count_.fetch_add(1, std::memory_order_release);

        if (active->row_count >= kBatchSize || active->row_count >= config::kMaxPendingRows) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
        }

        return inserted_rid;
    }
}

auto Table::insert_row(TimestampAllocator &timestamps,
                       const std::vector<ColumnData> &columns) -> StatusOr<RowId>
{
    if (columns.size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::shared_lock rw_lock(*rw_mtx);

    PendingBatch *active;
    while (true) {
        active = active_batch_.load(std::memory_order_acquire);
        if (!active) {
            rw_lock.unlock();
            auto new_batch = get_free_batch();
            rw_lock.lock();
            PendingBatch *expected = nullptr;
            if (active_batch_.compare_exchange_strong(expected,
                                                      new_batch.get(),
                                                      std::memory_order_release,
                                                      std::memory_order_acquire)) {
                new_batch.release();
            }
            else {
                return_free_batch(std::move(new_batch));
            }
            continue;
        }

        std::unique_lock batch_lock(active->mtx);
        if (active != active_batch_.load(std::memory_order_acquire)) {
            continue;
        }

        if (active->row_count == 0) {
            active->start_rid = row_count_.load(std::memory_order_relaxed);
        }

        Timestamp ts = timestamps.allocate_ts();
        RowId inserted_rid = active->start_rid + active->row_count;
        active->row_ts.push_back(ts);

        for (size_t i = 0; i < columns.size(); ++i) {
            auto *src = static_cast<const std::byte *>(columns[i].data);
            active->col_data[i].insert(active->col_data[i].end(), src, src + columns[i].size);
        }

        ++active->row_count;
        row_count_.fetch_add(1, std::memory_order_release);

        if (active->row_count >= kBatchSize || active->row_count >= config::kMaxPendingRows) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
        }

        return inserted_rid;
    }
}

auto Table::insert_rows(Timestamp ts, const std::vector<std::vector<ColumnData>> &rows)
    -> StatusOr<std::vector<RowId>>
{
    if (rows.empty())
        return std::vector<RowId>{};
    if (rows[0].size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::shared_lock rw_lock(*rw_mtx);
    std::vector<RowId> results;
    results.reserve(rows.size());

    size_t rows_left = rows.size();
    size_t rows_processed = 0;

    PendingBatch *active;
    while (rows_left > 0) {
        active = active_batch_.load(std::memory_order_acquire);
        if (!active) {
            rw_lock.unlock();
            auto new_batch = get_free_batch();
            rw_lock.lock();
            PendingBatch *expected = nullptr;
            if (active_batch_.compare_exchange_strong(expected,
                                                      new_batch.get(),
                                                      std::memory_order_release,
                                                      std::memory_order_acquire)) {
                new_batch.release();
            }
            else {
                return_free_batch(std::move(new_batch));
            }
            continue;
        }

        std::unique_lock batch_lock(active->mtx);
        if (active != active_batch_.load(std::memory_order_acquire)) {
            continue;
        }

        if (active->row_count == 0) {
            active->start_rid = row_count_.load(std::memory_order_relaxed);
        }

        size_t current_rc = active->row_count;
        size_t available = kBatchSize - current_rc;
        size_t to_insert = std::min(rows_left, available);

        if (to_insert == 0) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
            continue;
        }

        RowId start_rid = active->start_rid + current_rc;
        for (size_t i = 0; i < to_insert; ++i) {
            results.push_back(start_rid + static_cast<RowId>(i));
            active->row_ts.push_back(ts);

            for (size_t c = 0; c < schema_.column_count(); ++c) {
                auto *src = static_cast<const std::byte *>(rows[rows_processed + i][c].data);
                active->col_data[c].insert(active->col_data[c].end(),
                                           src,
                                           src + rows[rows_processed + i][c].size);
            }
        }

        active->row_count += static_cast<uint32_t>(to_insert);
        row_count_.fetch_add(to_insert, std::memory_order_release);

        if (active->row_count >= kBatchSize || active->row_count >= config::kMaxPendingRows) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
        }

        rows_processed += to_insert;
        rows_left -= to_insert;
    }

    return results;
}

auto Table::insert_rows(TimestampAllocator &timestamps,
                        const std::vector<std::vector<ColumnData>> &rows)
    -> StatusOr<std::vector<RowId>>
{
    if (rows.empty())
        return std::vector<RowId>{};
    if (rows[0].size() != schema_.column_count()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::shared_lock rw_lock(*rw_mtx);
    std::vector<RowId> results;
    results.reserve(rows.size());

    size_t rows_left = rows.size();
    size_t rows_processed = 0;

    PendingBatch *active;
    while (rows_left > 0) {
        active = active_batch_.load(std::memory_order_acquire);
        if (!active) {
            rw_lock.unlock();
            auto new_batch = get_free_batch();
            rw_lock.lock();
            PendingBatch *expected = nullptr;
            if (active_batch_.compare_exchange_strong(expected,
                                                      new_batch.get(),
                                                      std::memory_order_release,
                                                      std::memory_order_acquire)) {
                new_batch.release();
            }
            else {
                return_free_batch(std::move(new_batch));
            }
            continue;
        }

        std::unique_lock batch_lock(active->mtx);
        if (active != active_batch_.load(std::memory_order_acquire)) {
            continue;
        }

        if (active->row_count == 0) {
            active->start_rid = row_count_.load(std::memory_order_relaxed);
        }

        size_t current_rc = active->row_count;
        size_t available = kBatchSize - current_rc;
        size_t to_insert = std::min(rows_left, available);

        if (to_insert == 0) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
            continue;
        }

        RowId start_rid = active->start_rid + current_rc;
        for (size_t i = 0; i < to_insert; ++i) {
            results.push_back(start_rid + static_cast<RowId>(i));
            active->row_ts.push_back(timestamps.allocate_ts());

            for (size_t c = 0; c < schema_.column_count(); ++c) {
                auto *src = static_cast<const std::byte *>(rows[rows_processed + i][c].data);
                active->col_data[c].insert(active->col_data[c].end(),
                                           src,
                                           src + rows[rows_processed + i][c].size);
            }
        }

        active->row_count += static_cast<uint32_t>(to_insert);
        row_count_.fetch_add(to_insert, std::memory_order_release);

        if (active->row_count >= kBatchSize || active->row_count >= config::kMaxPendingRows) {
            PendingBatch *expected = active;
            if (active_batch_.compare_exchange_strong(expected,
                                                      nullptr,
                                                      std::memory_order_acq_rel)) {
                std::unique_ptr<PendingBatch> full_batch(active);
                push_flush_queue(std::move(full_batch));
            }
        }

        rows_processed += to_insert;
        rows_left -= to_insert;
    }

    return results;
}

auto Table::flush_pending() -> Status
{
    PendingBatch *active;
    std::unique_ptr<PendingBatch> to_flush;

    {
        std::shared_lock rw_lock(*rw_mtx);
        active = active_batch_.load(std::memory_order_acquire);
        if (active) {
            std::unique_lock batch_lock(active->mtx);
            if (active == active_batch_.load(std::memory_order_acquire) && active->row_count > 0) {
                if (active_batch_.compare_exchange_strong(active,
                                                          nullptr,
                                                          std::memory_order_acq_rel)) {
                    to_flush.reset(active);
                }
            }
        }
    }

    if (to_flush) {
        push_flush_queue(std::move(to_flush));
    }

    while (auto batch = pop_flush_queue()) {
        if (auto s = write_batch_to_page(batch.get()); s.code != Status::kOk) {
            push_flush_queue(std::move(batch));
            return s;
        }
        return_free_batch(std::move(batch));
    }

    return Status::kOk;
}

auto Table::write_batch_to_page(PendingBatch *batch) -> Status
{
    size_t row_count = batch->row_count;
    if (row_count == 0)
        return Status::kOk;

    size_t col_count = schema_.column_count();
    size_t hdr_size = sizeof(BatchHeader) + col_count * sizeof(ColMeta);

    std::vector<size_t> col_page_sizes(col_count);
    size_t total_data_size = 0;
    for (size_t ci = 0; ci < col_count; ++ci) {
        if (schema_.columns[ci] == ColumnType::kVarChar) {
            size_t blob_size = 0;
            size_t pos = 0;
            const auto &src = batch->col_data[ci];
            for (size_t r = 0; r < row_count && pos + sizeof(uint32_t) <= src.size(); ++r) {
                uint32_t len;
                std::memcpy(&len, src.data() + pos, sizeof(uint32_t));
                pos += sizeof(uint32_t) + len;
                blob_size += len;
            }
            col_page_sizes[ci] = row_count * sizeof(uint32_t) + blob_size;
        }
        else {
            col_page_sizes[ci] = batch->col_data[ci].size();
        }
        size_t padded_data = (col_page_sizes[ci] + 7) & ~7ULL;
        total_data_size += padded_data;
    }

    size_t bm_bytes = (row_count + 7) / 8;
    size_t padded_bm = (bm_bytes + 7) & ~7ULL;
    size_t total_bm_size = col_count * padded_bm;
    size_t payload_size = hdr_size + total_data_size + total_bm_size;
    payload_size = (payload_size + 7) & ~7ULL;

    std::unique_lock lock(*rw_mtx);

    PageId page_id = file_.size();
    size_t total_size = PageHeader::kSize + payload_size;
    if (auto s = file_.resize(page_id + total_size); s.code != Status::kOk) {
        return s;
    }

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
            std::vector<uint32_t> cum_offsets(row_count);
            size_t pos = 0;
            size_t blob_start = off + row_count * sizeof(uint32_t);
            size_t blob_pos = blob_start;
            for (size_t r = 0; r < row_count; ++r) {
                uint32_t len;
                std::memcpy(&len, batch->col_data[ci].data() + pos, sizeof(uint32_t));
                pos += sizeof(uint32_t);
                cum_offsets[r] = (r == 0) ? len : cum_offsets[r - 1] + len;
                std::memcpy(payload + blob_pos, batch->col_data[ci].data() + pos, len);
                pos += len;
                blob_pos += len;
            }
            std::memcpy(payload + off, cum_offsets.data(), row_count * sizeof(uint32_t));
            off += (col_page_sizes[ci] + 7) & ~7ULL;
        }
        else {
            std::memcpy(payload + off, batch->col_data[ci].data(), col_page_sizes[ci]);
            off += (col_page_sizes[ci] + 7) & ~7ULL;
        }

        std::memset(payload + null_off, 0, bm_bytes);
        null_off += (bm_bytes + 7) & ~7ULL;
    }

    set_page_checksum(hdr, payload, payload_size);

    RowId start_rid = batch->start_rid;
    pages_.push_back({start_rid, row_count, page_id});

    std::vector<IndexEntry> entries;
    entries.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        Timestamp row_ts = (i < batch->row_ts.size()) ? batch->row_ts[i] : 0;
        entries.push_back({start_rid + static_cast<RowId>(i), row_ts, page_id, 0});
    }
    version_index_.insert_bulk(entries.data(), entries.size());

    // msync_sync_safe is intentionally omitted here to prevent blocking.
    // It is called in Table::sync() with a shared_lock.
    return Status::kOk;
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

void Table::commit_rows(const std::vector<RowId> &row_ids, TxId tx_id, Timestamp commit_ts)
{
    std::vector<RowId> index_rows;
    uint64_t target = tx_id | kTxIdFlag;

    // First update active batch
    {
        std::shared_lock rw_lock(*rw_mtx);
        PendingBatch *active = active_batch_.load(std::memory_order_acquire);
        if (active) {
            std::unique_lock batch_lock(active->mtx);
            for (auto rid : row_ids) {
                if (rid >= active->start_rid && rid < active->start_rid + active->row_count) {
                    size_t offset = rid - active->start_rid;
                    if (offset < active->row_ts.size() && active->row_ts[offset] == target) {
                        active->row_ts[offset] = commit_ts;
                    }
                }
            }
        }
    }

    // Next update flush queue
    {
        std::unique_lock pool_lock(pool_mtx_);
        for (auto &batch : flush_queue_) {
            std::unique_lock batch_lock(batch->mtx);
            for (auto rid : row_ids) {
                if (rid >= batch->start_rid && rid < batch->start_rid + batch->row_count) {
                    size_t offset = rid - batch->start_rid;
                    if (offset < batch->row_ts.size() && batch->row_ts[offset] == target) {
                        batch->row_ts[offset] = commit_ts;
                    }
                }
            }
        }
    }

    // Finally commit to version index for flushed pages
    std::unique_lock lock(*rw_mtx);
    for (auto rid : row_ids) {
        index_rows.push_back(rid);
    }
    if (!index_rows.empty()) {
        version_index_.commit_rows(index_rows, tx_id, commit_ts);
    }
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

auto Table::get_index_by_col(size_t col_idx) -> BTree *
{
    std::shared_lock lock(*rw_mtx);
    for (auto &idx : indexes_) {
        if (idx.column_idx == col_idx)
            return &idx.tree;
    }
    return nullptr;
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
