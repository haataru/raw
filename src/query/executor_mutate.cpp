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

    std::vector<std::vector<ColumnData>> batch_cols(stmt.rows.size(), std::vector<ColumnData>(schema.column_count()));
    std::vector<std::byte> arena;
    // Guess 16 bytes per column per row
    arena.reserve(stmt.rows.size() * schema.column_count() * 16);

    for (size_t ri = 0; ri < stmt.rows.size(); ++ri) {
        const auto &row = stmt.rows[ri];
        if (row.size() != schema.column_count()) {
            return std::unexpected(Status::kInvalidArgument);
        }

        for (size_t ci = 0; ci < schema.column_count(); ++ci) {
            auto col_type = schema.columns[ci];
            const auto &val = row[ci];

            size_t start_off = arena.size();
            switch (col_type) {
                case ColumnType::kInt32: {
                    int32_t v = 0;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = static_cast<int32_t>(std::get<int64_t>(val.data));
                    }
                    auto* p = reinterpret_cast<std::byte*>(&v);
                    arena.insert(arena.end(), p, p + sizeof(v));
                    break;
                }
                case ColumnType::kTimestamp:
                case ColumnType::kInt64: {
                    int64_t v = 0;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = std::get<int64_t>(val.data);
                    }
                    auto* p = reinterpret_cast<std::byte*>(&v);
                    arena.insert(arena.end(), p, p + sizeof(v));
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
                    auto* p = reinterpret_cast<std::byte*>(&v);
                    arena.insert(arena.end(), p, p + sizeof(v));
                    break;
                }
                case ColumnType::kVarChar: {
                    std::string_view s;
                    std::string tmp;
                    if (std::holds_alternative<std::string>(val.data)) {
                        s = std::get<std::string>(val.data);
                    }
                    else if (std::holds_alternative<int64_t>(val.data)) {
                        tmp = std::to_string(std::get<int64_t>(val.data));
                        s = tmp;
                    }
                    uint32_t end = static_cast<uint32_t>(s.size());
                    auto* p_end = reinterpret_cast<std::byte*>(&end);
                    arena.insert(arena.end(), p_end, p_end + sizeof(uint32_t));
                    auto* p_str = reinterpret_cast<const std::byte*>(s.data());
                    arena.insert(arena.end(), p_str, p_str + s.size());
                    break;
                }
                case ColumnType::kBool: {
                    bool v = false;
                    if (std::holds_alternative<int64_t>(val.data)) {
                        v = std::get<int64_t>(val.data) != 0;
                    }
                    auto* p = reinterpret_cast<std::byte*>(&v);
                    arena.insert(arena.end(), p, p + sizeof(v));
                    break;
                }
            }
            batch_cols[ri][ci].type = col_type;
            batch_cols[ri][ci].size = arena.size() - start_off;
            // Note: pointer into arena is dangerous if arena reallocates!
            // We reserved enough, but if it reallocates, pointers break.
            // Actually, we must resolve pointers AFTER the loop!
            batch_cols[ri][ci].data = reinterpret_cast<const std::byte*>(start_off);
            batch_cols[ri][ci].nulls = nullptr;
        }
    }

    // Fixup pointers now that arena is fully populated
    for (size_t ri = 0; ri < stmt.rows.size(); ++ri) {
        for (size_t ci = 0; ci < schema.column_count(); ++ci) {
            size_t off = reinterpret_cast<size_t>(batch_cols[ri][ci].data);
            batch_cols[ri][ci].data = arena.data() + off;
        }
    }

    auto st = conn_.db().insert_batch(tid, batch_cols, conn_.txn());
    if (!st) {
        return std::unexpected(st.error());
    }

    if (has_indexes) {
        auto &table_ref = conn_.db().table(tid);
        for (size_t i = 0; i < stmt.rows.size(); ++i) {
            RowId rid = start_rid + static_cast<RowId>(i);
            Status idx_err = Status::kOk;
            table_ref.for_each_index([&](auto &idx) {
                if (idx_err != Status::kOk)
                    return;
                
                auto col_type = schema.columns[idx.column_idx];
                const std::byte *key;
                size_t key_len;
                
                const auto& col_data = batch_cols[i][idx.column_idx];
                
                if (col_type == ColumnType::kVarChar) {
                    auto total = col_data.size;
                    key_len = (total > sizeof(uint32_t)) ? total - sizeof(uint32_t) : 0;
                    key = static_cast<const std::byte*>(col_data.data) + sizeof(uint32_t);
                }
                else {
                    key = static_cast<const std::byte*>(col_data.data);
                    key_len = col_data.size;
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
    size_t row_count = scan->row_count;

    std::vector<RowId> target_rows;
    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    if (stmt.has_where) {
        auto matching = Filter::evaluate(scan->columns, schema, row_count, stmt.where.get());
        if (!matching)
            return std::unexpected(matching.error());
        for (auto row_idx : *matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (r && *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(row_idx));
            }
        }
    }
    else {
        target_rows.reserve(row_count);
        for (size_t i = 0; i < row_count; ++i) {
            auto r = tbl.search_version_index(static_cast<RowId>(i), read_ts);
            if (r && *r != Table::kNotFoundPage) {
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
    size_t row_count = scan->row_count;

    std::vector<RowId> target_rows;
    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    if (stmt.has_where) {
        auto matching = Filter::evaluate(scan->columns, schema, row_count, stmt.where.get());
        if (!matching)
            return std::unexpected(matching.error());
        for (auto row_idx : *matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (r && *r != Table::kNotFoundPage) {
                target_rows.push_back(static_cast<RowId>(row_idx));
            }
        }
    } else {
        target_rows.reserve(row_count);
        for (size_t i = 0; i < row_count; ++i) {
            auto r = tbl.search_version_index(static_cast<RowId>(i), read_ts);
            if (r && *r != Table::kNotFoundPage) {
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
