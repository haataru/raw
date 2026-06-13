#ifndef RAWDB_QUERY_EXECUTOR_HPP
#define RAWDB_QUERY_EXECUTOR_HPP

#include <string>
#include <vector>

#include "core/error.hpp"
#include "db/database.hpp"
#include "query/connection.hpp"
#include "query/parser.hpp"
#include "storage/table.hpp"

namespace rawdb
{

struct QueryResult
{
    std::vector<std::string> column_names;
    std::vector<ColumnType> column_types;
    /// Row-major: result[row][col]
    std::vector<std::vector<std::string>> rows;
};

/// Owns the underlying memory; provides ColumnData views into it.
struct TableScanResult
{
    std::vector<std::vector<std::byte>> col_data;
    std::vector<std::vector<uint8_t>> col_nulls;
    std::vector<ColumnData> columns;
};

class Filter
{
public:
    [[nodiscard]] static auto evaluate(const std::vector<ColumnData> &columns,
                                       const Schema &schema,
                                       size_t row_count,
                                       const ExprNode *expr) -> StatusOr<std::vector<size_t>>;

    [[nodiscard]] static auto match_count(const std::vector<ColumnData> &columns,
                                          const Schema &schema,
                                          size_t row_count,
                                          const Predicate &pred) -> StatusOr<size_t>;

private:
    static auto get_column_index(const Schema &schema, std::string_view name) -> StatusOr<size_t>;
};

class Executor
{
public:
    explicit Executor(Connection &conn) : conn_(conn) {}

    auto execute(const Statement &stmt) -> StatusOr<QueryResult>;

    /// Convenience: parse + execute.
    auto execute(std::string_view sql) -> StatusOr<QueryResult>;

    static auto read_table_columns(Table &table) -> StatusOr<TableScanResult>;

    static auto value_to_string(const ColumnData &col,
                                ColumnType type,
                                size_t row_index,
                                size_t total_rows) -> std::string;

    static auto index_lookup(Table &tbl,
                             const Predicate &pred) -> std::optional<std::vector<RowId>>;

private:
    auto execute_insert(const InsertStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_select(const SelectStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_join(const SelectStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_delete(const DeleteStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_update(const UpdateStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_create(const CreateStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_create_index(const CreateIndexStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_vacuum(const VacuumStmt &stmt) -> StatusOr<QueryResult>;
    
    auto execute_begin(const BeginStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_commit(const CommitStmt &stmt) -> StatusOr<QueryResult>;
    auto execute_rollback(const RollbackStmt &stmt) -> StatusOr<QueryResult>;

    Connection &conn_;
};

} // namespace rawdb

#endif // RAWDB_QUERY_EXECUTOR_HPP
