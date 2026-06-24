#include <algorithm>
#include <cstring>

#include "query/executor.hpp"
#include "storage/table.hpp"

namespace rawdb
{

void Table::clear_indexes()
{
    std::unique_lock lock(*rw_mtx);
    for (auto &idx : indexes_) {
        idx.tree.close();
    }
    indexes_.clear();
}

auto Table::vacuum(TimestampAllocator &timestamps) -> StatusOr<size_t>
{
    std::vector<RowId> alive;
    size_t new_rc = 0;

    flush_pending();

    {
        std::unique_lock lock(*rw_mtx);

        size_t old_rc = row_count_.load();
        if (old_rc == 0)
            return size_t{0};

        Timestamp probe = timestamps.allocate_ts();
        Timestamp max_ts = std::max(probe, version_index_.max_timestamp());

        alive.reserve(old_rc);
        for (RowId rid = 0; rid < static_cast<RowId>(old_rc); ++rid) {
            auto off = version_index_.search(rid, max_ts);
            if (off && *off != kNotFoundPage) {
                alive.push_back(rid);
            }
        }

        new_rc = alive.size();
        if (new_rc == 0) {
            row_count_.store(0);
            pages_.clear();
            version_index_ = VersionIndex{};
            file_.resize(0);
            return size_t{0};
        }
    }

    std::vector<size_t> all_cols(schema_.column_count());
    for (size_t i = 0; i < schema_.column_count(); ++i)
        all_cols[i] = i;

    auto scan_r = read_rows(alive, all_cols);
    if (!scan_r)
        return std::unexpected(scan_r.error());
    auto &scan = *scan_r;

    std::vector<std::vector<std::byte>> pending_data(schema_.column_count());
    for (size_t ci = 0; ci < schema_.column_count(); ++ci) {
        if (schema_.columns[ci] == ColumnType::kVarChar) {
            const auto &raw = scan.col_data[ci];
            size_t off_bytes = new_rc * sizeof(uint32_t);
            auto *offsets =
                reinterpret_cast<const uint32_t *>(static_cast<const void *>(raw.data()));
            for (size_t ri = 0; ri < new_rc; ++ri) {
                uint32_t prev = (ri == 0) ? 0 : offsets[ri - 1];
                uint32_t len = offsets[ri] - prev;
                auto *len_b = reinterpret_cast<const std::byte *>(&len);
                pending_data[ci].insert(pending_data[ci].end(), len_b, len_b + sizeof(uint32_t));
                auto *blob = raw.data() + off_bytes + prev;
                pending_data[ci].insert(pending_data[ci].end(), blob, blob + len);
            }
        }
        else {
            pending_data[ci] = std::move(scan.col_data[ci]);
        }
    }

    {
        std::unique_lock lock(*rw_mtx);
        row_count_.store(0);
        pages_.clear();
        version_index_ = VersionIndex{};
        file_.resize(0);

        auto batch_ptr = std::make_unique<PendingBatch>();
        batch_ptr->col_data.resize(schema_.column_count());

        size_t written = 0;
        while (written < new_rc) {
            size_t batch = std::min(kBatchSize, new_rc - written);
            Timestamp batch_ts = timestamps.allocate_ts();

            batch_ptr->start_rid = written;
            batch_ptr->row_count = 0;
            batch_ptr->row_ts.assign(batch, batch_ts);
            for (auto &cd : batch_ptr->col_data)
                cd.clear();

            for (size_t ci = 0; ci < schema_.column_count(); ++ci) {
                ColumnType ct = schema_.columns[ci];
                if (ct == ColumnType::kVarChar) {
                    size_t byte_start = 0;
                    for (size_t ri = 0; ri < written; ++ri) {
                        uint32_t len;
                        std::memcpy(&len, pending_data[ci].data() + byte_start, sizeof(uint32_t));
                        byte_start += sizeof(uint32_t) + len;
                    }
                    size_t byte_end = byte_start;
                    for (size_t ri = written; ri < written + batch; ++ri) {
                        uint32_t len;
                        std::memcpy(&len, pending_data[ci].data() + byte_end, sizeof(uint32_t));
                        byte_end += sizeof(uint32_t) + len;
                    }
                    batch_ptr->col_data[ci].assign(
                        pending_data[ci].begin() + static_cast<ptrdiff_t>(byte_start),
                        pending_data[ci].begin() + static_cast<ptrdiff_t>(byte_end));
                }
                else {
                    size_t es = 0;
                    switch (ct) {
                        case ColumnType::kInt32:
                            es = 4;
                            break;
                        case ColumnType::kTimestamp:
                        case ColumnType::kInt64:
                            es = 8;
                            break;
                        case ColumnType::kFloat64:
                            es = 8;
                            break;
                        case ColumnType::kBool:
                            es = 1;
                            break;
                        default:
                            break;
                    }
                    auto start_it = pending_data[ci].begin() + static_cast<ptrdiff_t>(written * es);
                    auto end_it = start_it + static_cast<ptrdiff_t>(batch * es);
                    batch_ptr->col_data[ci].assign(start_it, end_it);
                }
            }

            batch_ptr->row_count = batch;
            // Temporarily unlock because write_batch_to_page takes unique_lock internally
            // Wait, write_batch_to_page expects NO lock is held!
            lock.unlock();
            write_batch_to_page(batch_ptr.get());
            lock.lock();
            written += batch;
            row_count_.store(written);
        }
    }

    return new_rc;
}

} // namespace rawdb
