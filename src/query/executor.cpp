#include "query/executor.hpp"

namespace rawdb
{

Executor::Executor(Database &db) : db_(db) {}

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
    if (std::holds_alternative<InsertStmt>(stmt)) {
        return execute_insert(std::get<InsertStmt>(stmt));
    }
    if (std::holds_alternative<SelectStmt>(stmt)) {
        return execute_select(std::get<SelectStmt>(stmt));
    }
    if (std::holds_alternative<DeleteStmt>(stmt)) {
        return execute_delete(std::get<DeleteStmt>(stmt));
    }
    if (std::holds_alternative<CreateStmt>(stmt)) {
        return execute_create(std::get<CreateStmt>(stmt));
    }
    if (std::holds_alternative<CreateIndexStmt>(stmt)) {
        return execute_create_index(std::get<CreateIndexStmt>(stmt));
    }
    if (std::holds_alternative<VacuumStmt>(stmt)) {
        return execute_vacuum(std::get<VacuumStmt>(stmt));
    }
    return std::unexpected(Status::kNotSupported);
}

} // namespace rawdb
