#include <cstring>

#include "query/executor.hpp"

namespace rawdb
{

auto Executor::execute_insert(const InsertStmt &stmt) -> StatusOr<QueryResult>
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
    auto &schema = tbl.schema();
    RowId start_rid = tbl.row_count();
    bool has_indexes = tbl.has_indexes();

    struct RowBufs
    {
        std::vector<std::vector<std::byte>> cols;
    };
    std::vector<RowBufs> saved;

    for (const auto &row : stmt.rows) {
        if (row.size() != schema.column_count()) {
            return std::unexpected(Status::kInvalidArgument);
        }

        std::vector<ColumnData> cols;
        std::vector<std::vector<std::byte>> bufs(schema.column_count());
        std::vector<std::vector<uint8_t>> nulls(schema.column_count());

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
            ColumnData cd;
            cd.type = schema.columns[ci];
            cd.data = bufs[ci].data();
            cd.size = bufs[ci].size();
            cd.nulls = nulls[ci].empty() ? nullptr : nulls[ci].data();
            cols.push_back(cd);
        }

        auto st = db_.insert(tid, cols);
        if (st != Status::kOk) {
            return std::unexpected(st);
        }

        if (has_indexes) {
            saved.push_back({std::move(bufs)});
        }
    }

    if (has_indexes && !saved.empty()) {
        auto &table_ref = db_.table(tid);
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

    auto scan = read_table_columns(tbl);
    if (!scan)
        return std::unexpected(scan.error());
    size_t row_count = tbl.row_count();

    std::vector<RowId> target_rows;
    if (stmt.has_where) {
        auto matching = Filter::evaluate(scan->columns, schema, row_count, stmt.where);
        if (!matching)
            return std::unexpected(matching.error());
        Timestamp read_ts = db_.next_ts();
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
            target_rows.push_back(static_cast<RowId>(i));
        }
    }

    size_t deleted = target_rows.size();

    if (!target_rows.empty()) {
        auto st = db_.delete_rows(tid, std::move(target_rows));
        if (st != Status::kOk)
            return std::unexpected(st);
    }

    QueryResult result;
    result.column_names = {"rows_deleted"};
    result.column_types = {ColumnType::kInt64};
    result.rows = {{std::to_string(deleted)}};
    return result;
}

} // namespace rawdb
