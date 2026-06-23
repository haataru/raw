#include <cstring>
#include <ctime>

#include "query/executor.hpp"
#include "storage/page.hpp"

namespace rawdb
{

auto Executor::value_to_string(const ColumnData &col,
                               ColumnType type,
                               size_t row_index,
                               size_t total_rows) -> std::string
{
    if (col.nulls) {
        bool is_null = (col.nulls[row_index / 8] >> (row_index % 8)) & 1;
        if (is_null)
            return "NULL";
    }

    switch (type) {
        case ColumnType::kInt32: {
            auto *arr = static_cast<const int32_t *>(static_cast<const void *>(col.data));
            return std::to_string(arr[row_index]);
        }
        case ColumnType::kInt64: {
            auto *arr = static_cast<const int64_t *>(static_cast<const void *>(col.data));
            return std::to_string(arr[row_index]);
        }
        case ColumnType::kTimestamp: {
            auto *arr = static_cast<const int64_t *>(static_cast<const void *>(col.data));
            int64_t ts = arr[row_index];
            std::time_t t = ts / 1000;
#ifdef _WIN32
            struct tm tm_buf;
            gmtime_s(&tm_buf, &t);
            struct tm* tm_ptr = &tm_buf;
#else
            struct tm tm_buf;
            gmtime_r(&t, &tm_buf);
            struct tm* tm_ptr = &tm_buf;
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_ptr);
            return std::string(buf);
        }
        case ColumnType::kFloat64: {
            auto *arr = static_cast<const double *>(static_cast<const void *>(col.data));
            return std::to_string(arr[row_index]);
        }
        case ColumnType::kBool: {
            auto *arr = static_cast<const bool *>(static_cast<const void *>(col.data));
            return arr[row_index] ? "true" : "false";
        }
        case ColumnType::kVarChar: {
            auto *raw = static_cast<const std::byte *>(col.data);
            size_t off_bytes = total_rows * sizeof(uint32_t);
            if (col.size < off_bytes)
                return "";
            auto *offsets = reinterpret_cast<const uint32_t *>(static_cast<const void *>(raw));
            uint32_t start = (row_index == 0) ? 0 : offsets[row_index - 1];
            uint32_t end = offsets[row_index];
            auto *blob = raw + off_bytes;
            if (end > col.size - off_bytes)
                return "";
            return std::string(reinterpret_cast<const char *>(blob + start), end - start);
        }
    }
    return "";
}

auto Executor::read_table_columns(Table &table) -> StatusOr<TableScanResult>
{
    const auto &schema = table.schema();
    if (schema.column_count() == 0) {
        TableScanResult empty;
        return empty;
    }

    auto lock = table.lock_shared();

    TableScanResult scan;
    scan.col_data.resize(schema.column_count());
    scan.col_nulls.resize(schema.column_count());
    scan.columns.resize(schema.column_count());
    auto &file = table.file();
    if (!file.is_open() || file.size() == 0) {
        return scan;
    }

    size_t total_rows = table.row_count();
    for (size_t ci = 0; ci < schema.column_count(); ++ci) {
        if (schema.columns[ci] == ColumnType::kVarChar && total_rows > 0) {
            scan.col_data[ci].resize(total_rows * sizeof(uint32_t));
        }
    }

    PageId offset = 0;
    size_t rows_seen = 0;
    while (offset + PageHeader::kSize <= file.size()) {
        auto page = table.read_page(offset);
        if (!page)
            break;

        const auto &hdr = *reinterpret_cast<const PageHeader *>(page->data());
        const std::byte *payload = page->data() + PageHeader::kSize;
        size_t row_count = hdr.row_count;

        auto *bh = reinterpret_cast<const BatchHeader *>(payload);
        auto *metas = reinterpret_cast<const ColMeta *>(bh + 1);

        for (size_t ci = 0; ci < schema.column_count() && ci < bh->col_count; ++ci) {
            ColumnType col_type = schema.columns[ci];
            const std::byte *src = payload + metas[ci].data_off;
            size_t src_size = metas[ci].data_size;

            if (col_type == ColumnType::kVarChar) {
                size_t off_bytes = row_count * sizeof(uint32_t);
                if (src_size <= off_bytes)
                    continue;

                auto *page_offs =
                    reinterpret_cast<const uint32_t *>(static_cast<const void *>(src));
                uint32_t adj =
                    (rows_seen == 0)
                        ? 0
                        : *reinterpret_cast<const uint32_t *>(scan.col_data[ci].data() +
                                                              (rows_seen - 1) * sizeof(uint32_t));

                size_t needed = (rows_seen + row_count) * sizeof(uint32_t);
                if (scan.col_data[ci].size() < needed) {
                    scan.col_data[ci].resize(needed);
                }

                auto *go = reinterpret_cast<uint32_t *>(
                    static_cast<void *>(scan.col_data[ci].data() + rows_seen * sizeof(uint32_t)));
                for (size_t r = 0; r < row_count; ++r) {
                    go[r] = page_offs[r] + adj;
                }

                const std::byte *blob_src = src + off_bytes;
                size_t blob_size = src_size - off_bytes;
                scan.col_data[ci].insert(scan.col_data[ci].end(), blob_src, blob_src + blob_size);
            }
            else {
                size_t old_size = scan.col_data[ci].size();
                scan.col_data[ci].resize(old_size + src_size);
                std::memcpy(scan.col_data[ci].data() + old_size, src, src_size);
            }

            if (metas[ci].nulls_off != 0) {
                size_t bm_bytes = (row_count + 7) / 8;
                const uint8_t *bm_src =
                    reinterpret_cast<const uint8_t *>(payload + metas[ci].nulls_off);
                size_t old_ns = scan.col_nulls[ci].size();
                scan.col_nulls[ci].resize(old_ns + bm_bytes);
                std::memcpy(scan.col_nulls[ci].data() + old_ns, bm_src, bm_bytes);
            }
        }

        rows_seen += row_count;
        offset += static_cast<PageId>(PageHeader::kSize + hdr.data_size);
    }

    for (size_t ci = 0; ci < schema.column_count(); ++ci) {
        scan.columns[ci].type = schema.columns[ci];
        scan.columns[ci].data = scan.col_data[ci].data();
        scan.columns[ci].size = scan.col_data[ci].size();
        scan.columns[ci].nulls = scan.col_nulls[ci].empty() ? nullptr : scan.col_nulls[ci].data();
    }

    scan.row_count = rows_seen;
    return scan;
}

auto Executor::index_lookup(Table &tbl, const Predicate &pred) -> std::optional<std::vector<RowId>>
{
    if (pred.op != CmpOp::kEq)
        return std::nullopt;

    std::optional<std::vector<RowId>> result;
    tbl.for_each_index([&](auto &idx) {
        if (result)
            return;
        if (idx.column_name != pred.column.name)
            return;

        std::vector<std::byte> key_buf;
        switch (idx.column_type) {
            case ColumnType::kInt32: {
                int32_t v = std::holds_alternative<int64_t>(pred.value.data)
                                ? static_cast<int32_t>(std::get<int64_t>(pred.value.data))
                                : 0;
                key_buf.resize(sizeof(v));
                std::memcpy(key_buf.data(), &v, sizeof(v));
                break;
            }
            case ColumnType::kTimestamp:
            case ColumnType::kInt64: {
                int64_t v = std::holds_alternative<int64_t>(pred.value.data)
                                ? std::get<int64_t>(pred.value.data)
                                : 0;
                key_buf.resize(sizeof(v));
                std::memcpy(key_buf.data(), &v, sizeof(v));
                break;
            }
            case ColumnType::kFloat64: {
                double v = 0.0;
                if (std::holds_alternative<double>(pred.value.data))
                    v = std::get<double>(pred.value.data);
                else if (std::holds_alternative<int64_t>(pred.value.data))
                    v = static_cast<double>(std::get<int64_t>(pred.value.data));
                key_buf.resize(sizeof(v));
                std::memcpy(key_buf.data(), &v, sizeof(v));
                break;
            }
            case ColumnType::kBool: {
                bool v = std::holds_alternative<int64_t>(pred.value.data)
                             ? (std::get<int64_t>(pred.value.data) != 0)
                             : false;
                key_buf.push_back(std::byte{static_cast<unsigned char>(v ? 1 : 0)});
                break;
            }
            case ColumnType::kVarChar: {
                auto s = std::get<std::string>(pred.value.data);
                key_buf.assign(reinterpret_cast<const std::byte *>(s.data()),
                               reinterpret_cast<const std::byte *>(s.data() + s.size()));
                break;
            }
        }

        auto r = idx.tree.search(key_buf.data(), key_buf.size());
        if (r) {
            result = std::move(*r);
        }
    });
    return result;
}

} // namespace rawdb
