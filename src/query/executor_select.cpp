#include <algorithm>
#include <cstring>

#include "query/executor.hpp"

namespace rawdb
{

static auto compare_column_values(const ColumnData &col,
                                  ColumnType type,
                                  size_t a,
                                  size_t b,
                                  size_t total_rows) -> int
{
    switch (type) {
        case ColumnType::kInt32: {
            auto *arr = static_cast<const int32_t *>(static_cast<const void *>(col.data));
            return (arr[a] < arr[b]) ? -1 : (arr[a] > arr[b] ? 1 : 0);
        }
        case ColumnType::kInt64: {
            auto *arr = static_cast<const int64_t *>(static_cast<const void *>(col.data));
            return (arr[a] < arr[b]) ? -1 : (arr[a] > arr[b] ? 1 : 0);
        }
        case ColumnType::kFloat64: {
            auto *arr = static_cast<const double *>(static_cast<const void *>(col.data));
            if (arr[a] < arr[b])
                return -1;
            if (arr[a] > arr[b])
                return 1;
            return 0;
        }
        case ColumnType::kBool: {
            auto *arr = static_cast<const bool *>(static_cast<const void *>(col.data));
            return static_cast<int>(arr[a]) - static_cast<int>(arr[b]);
        }
        case ColumnType::kVarChar: {
            auto *raw = static_cast<const std::byte *>(col.data);
            size_t off_bytes = total_rows * sizeof(uint32_t);
            if (col.size < off_bytes)
                return 0;
            auto *offsets = reinterpret_cast<const uint32_t *>(static_cast<const void *>(raw));
            uint32_t a_start = (a == 0) ? 0 : offsets[a - 1];
            uint32_t a_end = offsets[a];
            uint32_t b_start = (b == 0) ? 0 : offsets[b - 1];
            uint32_t b_end = offsets[b];
            size_t a_len = a_end - a_start;
            size_t b_len = b_end - b_start;
            const char *a_str = reinterpret_cast<const char *>(raw + off_bytes + a_start);
            const char *b_str = reinterpret_cast<const char *>(raw + off_bytes + b_start);
            auto min_len = std::min(a_len, b_len);
            int cmp = std::memcmp(a_str, b_str, min_len);
            if (cmp != 0)
                return cmp;
            return (a_len < b_len) ? -1 : (a_len > b_len ? 1 : 0);
        }
    }
    return 0;
}

auto Executor::execute_select(const SelectStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(db_.table_count()); ++i) {
        if (db_.table(i).name() == stmt.table_name) {
            tid = static_cast<TableId>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected(Status::kNotFound);
    }

    auto &tbl = db_.table(tid);
    const auto &schema = tbl.schema();

    tbl.flush_pending();
    Timestamp read_ts = db_.next_ts();
    size_t row_count = tbl.row_count();

    QueryResult result;
    if (stmt.columns.empty()) {
        for (const auto &name : schema.names) {
            result.column_names.push_back(name);
        }
        result.column_types = schema.columns;
    }
    else {
        for (const auto &col : stmt.columns) {
            result.column_names.push_back(col.name);
            for (size_t ci = 0; ci < schema.names.size(); ++ci) {
                if (schema.names[ci] == col.name) {
                    result.column_types.push_back(schema.columns[ci]);
                    break;
                }
            }
        }
    }

    std::vector<size_t> col_indices;
    for (const auto &name : result.column_names) {
        for (size_t ci = 0; ci < schema.names.size(); ++ci) {
            if (schema.names[ci] == name) {
                col_indices.push_back(ci);
                break;
            }
        }
    }

    size_t order_col = static_cast<size_t>(-1);
    bool have_order_col = false;
    if (stmt.has_order_by) {
        for (size_t ci = 0; ci < schema.names.size(); ++ci) {
            if (schema.names[ci] == stmt.order_by.column.name) {
                order_col = ci;
                have_order_col = true;
                break;
            }
        }
    }

    std::vector<RowId> visible;
    bool index_used = false;
    TableScanResult full_scan;

    if (!stmt.has_order_by && stmt.has_where && stmt.where.op == CmpOp::kEq) {
        auto index_rids = index_lookup(tbl, stmt.where);
        if (index_rids.has_value()) {
            index_used = true;
            for (auto rid : *index_rids) {
                if (static_cast<size_t>(rid) >= row_count)
                    continue;
                auto r = tbl.search_version_index(rid, read_ts);
                if (!r || *r != Table::kNotFoundPage) {
                    visible.push_back(rid);
                }
            }
        }
    }

    if (!index_used) {
        auto scan_r = read_table_columns(tbl);
        if (!scan_r)
            return std::unexpected(scan_r.error());
        full_scan = std::move(*scan_r);

        std::vector<size_t> matching;
        if (stmt.has_where) {
            auto filtered = Filter::evaluate(full_scan.columns, schema, row_count, stmt.where);
            if (!filtered)
                return std::unexpected(filtered.error());
            matching = std::move(*filtered);
        }
        else {
            matching.resize(row_count);
            for (size_t i = 0; i < row_count; ++i)
                matching[i] = i;
        }

        for (auto row_idx : matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                visible.push_back(static_cast<RowId>(row_idx));
            }
        }

        if (stmt.has_order_by && !visible.empty() && have_order_col &&
            order_col < full_scan.columns.size()) {
            std::sort(visible.begin(), visible.end(), [&](RowId a, RowId b) -> bool {
                int cmp = compare_column_values(full_scan.columns[order_col],
                                                schema.columns[order_col],
                                                a,
                                                b,
                                                row_count);
                return stmt.order_by.asc ? (cmp < 0) : (cmp > 0);
            });
        }
    }

    if (stmt.has_limit && visible.size() > stmt.limit_count) {
        visible.resize(stmt.limit_count);
    }

    if (visible.empty())
        return result;

    auto final_scan = tbl.read_rows(visible, col_indices);
    if (!final_scan)
        return std::unexpected(final_scan.error());

    result.rows.reserve(visible.size());
    for (size_t ri = 0; ri < visible.size(); ++ri) {
        std::vector<std::string> row;
        for (size_t ci = 0; ci < col_indices.size(); ++ci) {
            size_t col_idx = col_indices[ci];
            auto val = value_to_string(final_scan->columns[col_idx],
                                       schema.columns[col_idx],
                                       ri,
                                       visible.size());
            row.push_back(std::move(val));
        }
        result.rows.push_back(std::move(row));
    }

    return result;
}

} // namespace rawdb
