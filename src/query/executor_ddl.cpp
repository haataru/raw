#include <cstring>

#include "query/executor.hpp"

namespace rawdb
{

static auto fixed_column_size(ColumnType type) -> size_t
{
    switch (type) {
        case ColumnType::kInt32:
            return 4;
        case ColumnType::kInt64:
            return 8;
        case ColumnType::kFloat64:
            return 8;
        case ColumnType::kBool:
            return 1;
        case ColumnType::kVarChar:
            return 0;
    }
    return 0;
}

auto Executor::execute_create(const CreateStmt &stmt) -> StatusOr<QueryResult>
{
    for (TableId i = 0; i < static_cast<TableId>(db_.table_count()); ++i) {
        if (db_.table(i).name() == stmt.table_name) {
            return std::unexpected(Status::kAlreadyExists);
        }
    }

    Schema schema;
    for (const auto &col : stmt.columns) {
        schema.columns.push_back(col.type);
        schema.names.push_back(col.name);
    }

    auto tid = db_.create_table(stmt.table_name, std::move(schema));
    if (!tid)
        return std::unexpected(tid.error());

    QueryResult result;
    result.column_names = {"created"};
    result.column_types = {ColumnType::kBool};
    result.rows = {{"true"}};
    return result;
}

auto Executor::execute_create_index(const CreateIndexStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(db_.table_count()); ++i) {
        if (db_.table(i).name() == stmt.table_name) {
            tid = i;
            found = true;
            break;
        }
    }
    if (!found)
        return std::unexpected(Status::kNotFound);

    auto &tbl = db_.table(tid);
    const auto &schema = tbl.schema();

    size_t col_idx = static_cast<size_t>(-1);
    for (size_t i = 0; i < schema.names.size(); ++i) {
        if (schema.names[i] == stmt.column_name) {
            col_idx = i;
            break;
        }
    }
    if (col_idx == static_cast<size_t>(-1)) {
        return std::unexpected(Status::kNotFound);
    }

    ColumnType col_type = schema.columns[col_idx];

    auto idx_path = db_.path() / (tbl.name() + "_" + std::to_string(col_idx) + ".idx");
    auto tree_r = BTree::create(idx_path, col_type);
    if (!tree_r)
        return std::unexpected(tree_r.error());

    IndexInfo info;
    info.name = stmt.index_name;
    info.column_name = stmt.column_name;
    info.column_idx = col_idx;
    info.column_type = col_type;
    info.tree = std::move(*tree_r);

    tbl.flush_pending();
    auto scan = read_table_columns(tbl);
    if (scan) {
        size_t row_count = tbl.row_count();
        const auto &cd = scan->columns[col_idx];

        if (col_type == ColumnType::kVarChar) {
            auto *offsets = reinterpret_cast<const uint32_t *>(static_cast<const void *>(cd.data));
            for (size_t ri = 0; ri < row_count; ++ri) {
                uint32_t end = offsets[ri];
                uint32_t start = (ri == 0) ? 0 : offsets[ri - 1];
                const std::byte *key = cd.data + row_count * sizeof(uint32_t) + start;
                size_t key_len = static_cast<size_t>(end - start);
                auto st = info.tree.insert(key, key_len, static_cast<RowId>(ri));
                if (st != Status::kOk) {
                    info.tree.close();
                    return std::unexpected(st);
                }
            }
        }
        else {
            size_t elem_size = fixed_column_size(col_type);
            for (size_t ri = 0; ri < row_count; ++ri) {
                const std::byte *key = cd.data + ri * elem_size;
                auto st = info.tree.insert(key, elem_size, static_cast<RowId>(ri));
                if (st != Status::kOk) {
                    info.tree.close();
                    return std::unexpected(st);
                }
            }
        }
    }

    tbl.add_index(std::move(info));

    QueryResult result;
    result.column_names = {"created"};
    result.column_types = {ColumnType::kBool};
    result.rows = {{"true"}};
    return result;
}

auto Executor::execute_vacuum(const VacuumStmt &stmt) -> StatusOr<QueryResult>
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
    if (!found)
        return std::unexpected(Status::kNotFound);

    auto st = db_.vacuum(tid);
    if (st != Status::kOk)
        return std::unexpected(st);

    QueryResult result;
    result.column_names = {"vacuumed"};
    result.column_types = {ColumnType::kBool};
    result.rows = {{"true"}};
    return result;
}

} // namespace rawdb
