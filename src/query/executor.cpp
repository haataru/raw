#include "query/executor.hpp"

namespace rawdb
{

auto Executor::execute(std::string_view sql) -> StatusOr<QueryResult>
{
    Parser parser;
    auto stmt = parser.parse(sql);
    if (!stmt)
        return std::unexpected(stmt.error());
    return execute(*stmt);
}

auto Executor::execute(const Statement &stmt) -> StatusOr<QueryResult>
{
    return std::visit(
        [this](auto &&s) -> StatusOr<QueryResult> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, InsertStmt>) {
                return execute_insert(s);
            }
            else if constexpr (std::is_same_v<T, SelectStmt>) {
                return execute_select(s);
            }
            else if constexpr (std::is_same_v<T, DeleteStmt>) {
                return execute_delete(s);
            }
            else if constexpr (std::is_same_v<T, UpdateStmt>) {
                return execute_update(s);
            }
            else if constexpr (std::is_same_v<T, CreateStmt>) {
                return execute_create(s);
            }
            else if constexpr (std::is_same_v<T, CreateIndexStmt>) {
                return execute_create_index(s);
            }
            else if constexpr (std::is_same_v<T, VacuumStmt>) {
                return execute_vacuum(s);
            }
            else if constexpr (std::is_same_v<T, BeginStmt>) {
                return execute_begin(s);
            }
            else if constexpr (std::is_same_v<T, CommitStmt>) {
                return execute_commit(s);
            }
            else if constexpr (std::is_same_v<T, RollbackStmt>) {
                return execute_rollback(s);
            }
            else if constexpr (std::is_same_v<T, BackupStmt>) {
                return execute_backup(s);
            }
            else if constexpr (std::is_same_v<T, ConfigStmt>) {
                return execute_config(s);
            }
            else {
                return std::unexpected(Status::kInvalidArgument);
            }
        },
        stmt);
}

auto Executor::execute_begin(const BeginStmt &) -> StatusOr<QueryResult>
{
    auto st = conn_.begin();
    if (st != Status::kOk)
        return std::unexpected(st);
    QueryResult r;
    return r;
}

auto Executor::execute_commit(const CommitStmt &) -> StatusOr<QueryResult>
{
    auto st = conn_.commit();
    if (st != Status::kOk)
        return std::unexpected(st);
    QueryResult r;
    return r;
}

auto Executor::execute_rollback(const RollbackStmt &) -> StatusOr<QueryResult>
{
    auto st = conn_.rollback();
    if (st != Status::kOk)
        return std::unexpected(st);
    return QueryResult{{"status"}, {ColumnType::kVarChar}, {{"OK"}}};
}

auto Executor::execute_backup(const BackupStmt &stmt) -> StatusOr<QueryResult>
{
    if (auto s = conn_.db().start_backup(stmt.path); s.code != Status::kOk) {
        return std::unexpected(s);
    }
    return QueryResult{{"status"}, {ColumnType::kVarChar}, {{"BACKUP SUCCESS"}}};
}

auto Executor::execute_config(const ConfigStmt &stmt) -> StatusOr<QueryResult>
{
    if (stmt.key == "backup_interval") {
        conn_.db().set_backup_interval(stmt.value);
    }
    else if (stmt.key == "backup_retention") {
        conn_.db().set_backup_retention(stmt.value);
    }
    else {
        return std::unexpected(Status{Status::kInvalidArgument, "Unknown config key"});
    }
    return QueryResult{{"status"}, {ColumnType::kVarChar}, {{"CONFIG UPDATED"}}};
}

} // namespace rawdb
