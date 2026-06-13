#include <cstring>

#include "query/executor.hpp"

namespace rawdb
{

auto Executor::execute_insert(const InsertStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.table_name) {
            tid = static_cast<TableId>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected(Status::kNotFound);
    }

    auto &tbl = conn_.db().table(tid);
    auto &schema = tbl.schema();
    RowId start_rid = tbl.row_count();
    bool has_indexes = tbl.has_indexes();

    struct RowBufs
    {
        std::vector<std::vector<std::byte>> cols;
    };
    std::vector<RowBufs> saved;

    std::vector<ColumnData> cols(schema.column_count());
    std::vector<std::vector<std::byte>> bufs(schema.column_count());
    std::vector<std::vector<uint8_t>> nulls(schema.column_count());

    for (const auto &row : stmt.rows) {
        if (row.size() != schema.column_count()) {
            return std::unexpected(Status::kInvalidArgument);
        }

        if (has_indexes) {
            bufs = std::vector<std::vector<std::byte>>(schema.column_count());
        }

        for (size_t ci = 0; ci < row.size(); ++ci) {
            auto col_type = schema.columns[ci];
            const auto &val = row[ci];

            bufs[ci].clear();

            switch (col_type) {
                case ColumnType::kInt32: {
                    int32_t v = 0;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = static_cast<int32_t>(std::get<int64_t>(val.data));
                    }
                    bufs[ci].resize(sizeof(v));
                    std::memcpy(bufs[ci].data(), &v, sizeof(v));
                    break;
                }
                case ColumnType::kTimestamp:
                case ColumnType::kInt64: {
                    int64_t v = 0;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = std::get<int64_t>(val.data);
                    }
                    bufs[ci].resize(sizeof(v));
                    std::memcpy(bufs[ci].data(), &v, sizeof(v));
                    break;
                }
                case ColumnType::kFloat64: {
                    double v = 0.0;
                    if (std::holds_alternative<double>(val.data)) {
                        v = std::get<double>(val.data);
                    }
                    else if (std::holds_alternative<int64_t>(val.data)) {
                        v = static_cast<double>(std::get<int64_t>(val.data));
                    }
                    bufs[ci].resize(sizeof(v));
                    std::memcpy(bufs[ci].data(), &v, sizeof(v));
                    break;
                }
                case ColumnType::kVarChar: {
                    std::string_view s;
                    if (std::holds_alternative<std::string>(val.data)) {
                        s = std::get<std::string>(val.data);
                    }
                    else if (std::holds_alternative<int64_t>(val.data)) {
                        auto tmp = std::to_string(std::get<int64_t>(val.data));
                        s = tmp;
                    }
                    uint32_t end = static_cast<uint32_t>(s.size());
                    bufs[ci].resize(sizeof(uint32_t) + s.size());
                    std::memcpy(bufs[ci].data(), &end, sizeof(uint32_t));
                    std::memcpy(bufs[ci].data() + sizeof(uint32_t), s.data(), s.size());
                    break;
                }
                case ColumnType::kBool: {
                    bool v = false;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = std::get<int64_t>(val.data) != 0;
                    }
                    bufs[ci].resize(sizeof(v));
                    std::memcpy(bufs[ci].data(), &v, sizeof(v));
                    break;
                }
            }
        }

        for (size_t ci = 0; ci < schema.column_count(); ++ci) {
            cols[ci].type = schema.columns[ci];
            cols[ci].data = bufs[ci].data();
            cols[ci].size = bufs[ci].size();
            cols[ci].nulls = nulls[ci].empty() ? nullptr : nulls[ci].data();
        }

        auto st = conn_.db().insert(tid, cols, conn_.txn());
        if (!st) {
            return std::unexpected(st.error());
        }

        if (has_indexes) {
            saved.push_back({std::move(bufs)});
        }
    }

    if (has_indexes && !saved.empty()) {
        auto &table_ref = conn_.db().table(tid);
        for (size_t i = 0; i < saved.size(); ++i) {
            RowId rid = start_rid + static_cast<RowId>(i);
            Status idx_err = Status::kOk;
            table_ref.for_each_index([&](auto &idx) {
                if (idx_err != Status::kOk)
                    return;
                const auto &col_buf = saved[i].cols[idx.column_idx];
                auto col_type = schema.columns[idx.column_idx];
                const std::byte *key;
                size_t key_len;
                if (col_type == ColumnType::kVarChar) {
                    auto total = col_buf.size();
                    key_len = (total > sizeof(uint32_t)) ? total - sizeof(uint32_t) : 0;
                    key = col_buf.data() + sizeof(uint32_t);
                }
                else {
                    key = col_buf.data();
                    key_len = col_buf.size();
                }
                idx_err = idx.tree.insert(key, key_len, rid);
            });
            if (idx_err != Status::kOk)
                return std::unexpected(idx_err);
        }
    }

    QueryResult result;
    result.column_names = {"rows_inserted"};
    result.column_types = {ColumnType::kInt64};
    result.rows = {{std::to_string(stmt.rows.size())}};
    return result;
}

auto Executor::execute_delete(const DeleteStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.table_name) {
            tid = static_cast<TableId>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected(Status::kNotFound);
    }

    auto &tbl = conn_.db().table(tid);
    const auto &schema = tbl.schema();

    tbl.flush_pending();

    auto scan = read_table_columns(tbl);
    if (!scan)
        return std::unexpected(scan.error());
    size_t row_count = tbl.row_count();

    std::vector<RowId> target_rows;
    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    if (stmt.has_where) {
        auto matching = Filter::evaluate(scan->columns, schema, row_count, stmt.where.get());
        if (!matching)
            return std::unexpected(matching.error());
        for (auto row_idx : *matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(row_idx));
            }
        }
    }
    else {
        target_rows.reserve(row_count);
        for (size_t i = 0; i < row_count; ++i) {
            auto r = tbl.search_version_index(static_cast<RowId>(i), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(i));
            }
        }
    }

    size_t deleted = target_rows.size();

    if (!target_rows.empty()) {
        auto st = conn_.db().delete_rows(tid, std::move(target_rows), conn_.txn());
        if (st != Status::kOk)
            return std::unexpected(st);
    }

    QueryResult result;
    result.column_names = {"rows_deleted"};
    result.column_types = {ColumnType::kInt64};
    result.rows = {{std::to_string(deleted)}};
    return result;
}

static auto bytes_to_value(const ColumnData &col, ColumnType type, size_t row_idx, size_t total_rows) -> Value
{
    Value v;
    switch (type) {
        case ColumnType::kInt32:
            v.data = static_cast<int64_t>(static_cast<const int32_t *>(static_cast<const void *>(col.data))[row_idx]);
            break;
        case ColumnType::kTimestamp:
        case ColumnType::kInt64:
            v.data = static_cast<const int64_t *>(static_cast<const void *>(col.data))[row_idx];
            break;
        case ColumnType::kFloat64:
            v.data = static_cast<const double *>(static_cast<const void *>(col.data))[row_idx];
            break;
        case ColumnType::kBool:
            v.data = static_cast<int64_t>(static_cast<const bool *>(static_cast<const void *>(col.data))[row_idx]);
            break;
        case ColumnType::kVarChar: {
            auto *raw = static_cast<const std::byte *>(col.data);
            size_t off_bytes = total_rows * sizeof(uint32_t);
            if (col.size < off_bytes) {
                v.data = std::string("");
                break;
            }
            auto *offsets = reinterpret_cast<const uint32_t *>(static_cast<const void *>(raw));
            uint32_t start = (row_idx == 0) ? 0 : offsets[row_idx - 1];
            uint32_t end = offsets[row_idx];
            v.data = std::string(reinterpret_cast<const char *>(raw + off_bytes + start), end - start);
            break;
        }
    }
    return v;
}

auto Executor::execute_update(const UpdateStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.table_name) {
            tid = static_cast<TableId>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected(Status::kNotFound);
    }

    auto &tbl = conn_.db().table(tid);
    const auto &schema = tbl.schema();

    size_t target_col_idx = static_cast<size_t>(-1);
    for (size_t ci = 0; ci < schema.names.size(); ++ci) {
        if (schema.names[ci] == stmt.column_name) {
            target_col_idx = ci;
            break;
        }
    }
    if (target_col_idx == static_cast<size_t>(-1)) {
        return std::unexpected(Status::kNotFound);
    }

    tbl.flush_pending();
    auto scan = read_table_columns(tbl);
    if (!scan)
        return std::unexpected(scan.error());
    size_t row_count = tbl.row_count();

    std::vector<RowId> target_rows;
    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    if (stmt.has_where) {
        auto matching = Filter::evaluate(scan->columns, schema, row_count, stmt.where.get());
        if (!matching)
            return std::unexpected(matching.error());
        for (auto row_idx : *matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(row_idx));
            }
        }
    } else {
        target_rows.reserve(row_count);
        for (size_t i = 0; i < row_count; ++i) {
            auto r = tbl.search_version_index(static_cast<RowId>(i), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(i));
            }
        }
    }

    size_t updated = target_rows.size();

    if (updated > 0) {
        InsertStmt insert_stmt;
        insert_stmt.table_name = stmt.table_name;
        insert_stmt.rows.reserve(updated);

        for (auto rid : target_rows) {
            std::vector<Value> row_vals;
            row_vals.reserve(schema.column_count());
            for (size_t ci = 0; ci < schema.column_count(); ++ci) {
                if (ci == target_col_idx) {
                    row_vals.push_back(stmt.new_value);
                } else {
                    row_vals.push_back(bytes_to_value(scan->columns[ci], schema.columns[ci], rid, row_count));
                }
            }
            insert_stmt.rows.push_back(std::move(row_vals));
        }

        auto st = conn_.db().delete_rows(tid, target_rows, conn_.txn());
        if (st != Status::kOk)
            return std::unexpected(st);

        auto ins_res = execute_insert(insert_stmt);
        if (!ins_res)
            return std::unexpected(ins_res.error());
    }

    QueryResult result;
    result.column_names = {"rows_updated"};
    result.column_types = {ColumnType::kInt64};
    result.rows = {{std::to_string(updated)}};
    return result;
}

} // namespace rawdb
