#include <algorithm>
#include <cstring>

#include "query/executor.hpp"
#include "storage/table.hpp"

namespace rawdb
{

static size_t column_elem_size(ColumnType ct)
{
    switch (ct) {
        case ColumnType::kInt32:
            return 4;
        case ColumnType::kTimestamp:
        case ColumnType::kInt64:
            return 8;
        case ColumnType::kFloat64:
            return 8;
        case ColumnType::kBool:
            return 1;
        default:
            return 0;
    }
}

auto Table::read_rows(const std::vector<RowId> &row_ids,
                      const std::vector<size_t> &col_indices) -> StatusOr<TableScanResult>
{
    size_t N = row_ids.size();
    if (N == 0 || col_indices.empty()) {
        return TableScanResult{};
    }

    std::shared_lock lock(*rw_mtx);

    TableScanResult scan;
    scan.col_data.resize(schema_.column_count());
    scan.col_nulls.resize(schema_.column_count());
    scan.columns.resize(schema_.column_count());

    for (size_t ci : col_indices) {
        ColumnType ct = schema_.columns[ci];
        if (ct == ColumnType::kVarChar) {
            scan.col_data[ci].resize(N * sizeof(uint32_t));
        }
        else {
            scan.col_data[ci].resize(N * column_elem_size(ct));
        }
    }

    std::vector<uint32_t> varchar_cum(schema_.column_count(), 0);
    PageId cached_pid = kNotFoundPage;
    const std::byte *cached_page_ptr = nullptr;

    for (size_t i = 0; i < N; ++i) {
        RowId rid = row_ids[i];

        auto it =
            std::upper_bound(pages_.begin(), pages_.end(), rid, [](RowId r, const RowRange &rg) {
                return r < rg.start;
            });
        if (it != pages_.begin()) {
            --it;
        }
        else {
            it = pages_.end();
        }

        if (it == pages_.end() || rid < it->start || rid >= it->start + it->count) {
            // Not in flushed pages, check pending batches
            auto read_from_batch = [&](PendingBatch *b) {
                std::unique_lock b_lock(b->mtx);
                if (rid < b->start_rid || rid >= b->start_rid + b->row_count)
                    return false;

                size_t local = rid - b->start_rid;
                for (size_t ci : col_indices) {
                    ColumnType ct = schema_.columns[ci];
                    if (ct == ColumnType::kVarChar) {
                        uint32_t len = 0;
                        size_t pos = 0;
                        for (size_t r = 0; r <= local; ++r) {
                            std::memcpy(&len, b->col_data[ci].data() + pos, sizeof(uint32_t));
                            pos += sizeof(uint32_t);
                            if (r == local)
                                break;
                            pos += len;
                        }

                        varchar_cum[ci] += len;
                        std::memcpy(scan.col_data[ci].data() + i * sizeof(uint32_t),
                                    &varchar_cum[ci],
                                    sizeof(uint32_t));

                        if (len > 0) {
                            scan.col_data[ci].insert(scan.col_data[ci].end(),
                                                     b->col_data[ci].data() + pos,
                                                     b->col_data[ci].data() + pos + len);
                        }
                    }
                    else {
                        size_t es = column_elem_size(ct);
                        std::memcpy(scan.col_data[ci].data() + i * es,
                                    b->col_data[ci].data() + local * es,
                                    es);
                    }
                }
                return true;
            };

            PendingBatch *active = active_batch_.load(std::memory_order_acquire);
            if (active && read_from_batch(active))
                continue;

            auto *self = const_cast<Table *>(this);
            std::unique_lock pool_lock(self->pool_mtx_);
            bool found = false;
            for (auto &b : self->flush_queue_) {
                if (read_from_batch(b.get())) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // If STILL not found, we must just write 0/null to avoid crash
                for (size_t ci : col_indices) {
                    ColumnType ct = schema_.columns[ci];
                    if (ct == ColumnType::kVarChar) {
                        std::memcpy(scan.col_data[ci].data() + i * sizeof(uint32_t),
                                    &varchar_cum[ci],
                                    sizeof(uint32_t));
                    }
                    else {
                        size_t es = column_elem_size(ct);
                        std::memset(scan.col_data[ci].data() + i * es, 0, es);
                    }
                }
            }
            continue;
        }
        size_t local = static_cast<size_t>(rid - it->start);

        if (it->page_id != cached_pid) {
            cached_page_ptr = file_.data() + it->page_id;
            cached_pid = it->page_id;
        }

        auto *payload = cached_page_ptr + PageHeader::kSize;
        auto *bh = reinterpret_cast<const BatchHeader *>(payload);
        auto *metas = reinterpret_cast<const ColMeta *>(bh + 1);

        for (size_t ci : col_indices) {
            ColumnType ct = schema_.columns[ci];
            bool missing = (ci >= bh->col_count);
            const std::byte *src = missing ? nullptr : payload + metas[ci].data_off;

            if (ct == ColumnType::kVarChar) {
                uint32_t prev = 0;
                uint32_t end = 0;
                size_t off_bytes = bh->row_count * sizeof(uint32_t);

                if (!missing && metas[ci].data_size > off_bytes) {
                    auto *page_offs =
                        reinterpret_cast<const uint32_t *>(static_cast<const void *>(src));
                    prev = (local == 0) ? 0 : page_offs[local - 1];
                    end = page_offs[local];
                }

                uint32_t len = end - prev;

                varchar_cum[ci] += len;
                std::memcpy(scan.col_data[ci].data() + i * sizeof(uint32_t),
                            &varchar_cum[ci],
                            sizeof(uint32_t));

                if (len > 0) {
                    const std::byte *blob_src = src + off_bytes + prev;
                    scan.col_data[ci].insert(scan.col_data[ci].end(), blob_src, blob_src + len);
                }
            }
            else {
                size_t es = column_elem_size(ct);
                if (missing) {
                    std::memset(scan.col_data[ci].data() + i * es, 0, es);
                }
                else {
                    std::memcpy(scan.col_data[ci].data() + i * es, src + local * es, es);
                }
            }
        }
    }

    for (size_t ci = 0; ci < schema_.column_count(); ++ci) {
        scan.columns[ci].type = schema_.columns[ci];
        scan.columns[ci].data = scan.col_data[ci].data();
        scan.columns[ci].size = scan.col_data[ci].size();
        scan.columns[ci].nulls = nullptr;
    }

    return scan;
}

auto Table::read_page_header(PageId page_id) const -> StatusOr<PageHeader>
{
    if (page_id + PageHeader::kSize > file_.size()) {
        return std::unexpected(Status::kNotFound);
    }
    auto &hdr = *reinterpret_cast<const PageHeader *>(file_.data() + page_id);
    return hdr;
}

auto Table::read_page(PageId page_id) const -> StatusOr<std::vector<std::byte>>
{
    if (page_id + PageHeader::kSize > file_.size()) {
        return std::unexpected(Status::kNotFound);
    }
    auto &hdr = *reinterpret_cast<const PageHeader *>(file_.data() + page_id);

    size_t total = PageHeader::kSize + hdr.data_size;
    if (page_id + total > file_.size()) {
        return std::unexpected(Status::kCorruptedData);
    }

    std::vector<std::byte> buf(total);
    std::memcpy(buf.data(), file_.data() + page_id, total);

    auto st = validate_page(*reinterpret_cast<const PageHeader *>(buf.data()),
                            buf.data() + PageHeader::kSize);
    if (st != Status::kOk) {
        return std::unexpected(st);
    }

    return buf;
}

auto Table::lookup_page(RowId row_id) const -> StatusOr<PageId>
{
    std::shared_lock lock(*rw_mtx);
    for (const auto &r : pages_) {
        if (row_id >= r.start && row_id < r.start + r.count) {
            return r.page_id;
        }
    }
    return std::unexpected(Status::kNotFound);
}

} // namespace rawdb
