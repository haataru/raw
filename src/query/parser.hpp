#ifndef RAWDB_QUERY_PARSER_HPP
#define RAWDB_QUERY_PARSER_HPP

#include <string>
#include <variant>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"

namespace rawdb
{

enum class TokenType
{
    kIdentifier,
    kNumber,
    kString,
    kStar,
    kComma,
    kLParen,
    kRParen,
    kSemicolon,
    kEq,
    kNe,
    kLt,
    kGt,
    kLe,
    kGe,
    kInsert,
    kInto,
    kValues,
    kSelect,
    kFrom,
    kWhere,
    kDelete,
    kCreate,
    kTable,
    kIndex,
    kOn,
    kOrder,
    kBy,
    kAsc,
    kDesc,
    kLimit,
    kVacuum,
    kUpdate,
    kSet,
    kBeginTxn,
    kCommitTxn,
    kRollbackTxn,
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
    kGroup,
    kJoin,
    kDot,
    kEnd
};

struct Token
{
    TokenType type;
    std::string text;
};

// ──────────────────────────────────────────────
// AST
// ──────────────────────────────────────────────

enum class AggFunc : uint8_t
{
    kNone,
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax
};

struct ColumnRef
{
    std::string table; // Optional table prefix
    std::string name;
    AggFunc func{AggFunc::kNone};
    bool is_star{false};
};

struct JoinClause
{
    std::string table_name;
    ColumnRef left_col;
    ColumnRef right_col;
};

struct Value
{
    std::variant<int64_t, double, std::string> data;
};

enum class CmpOp : uint8_t
{
    kEq,
    kNe,
    kLt,
    kGt,
    kLe,
    kGe
};

struct Predicate
{
    ColumnRef column;
    CmpOp op;
    Value value;
};

struct InsertStmt
{
    std::string table_name;
    std::vector<std::vector<Value>> rows;
};

struct OrderBy
{
    ColumnRef column;
    bool asc{true};
};

struct SelectStmt
{
    std::vector<ColumnRef> columns; // empty = *
    std::string table_name;
    Predicate where;
    bool has_where{false};
    std::vector<ColumnRef> group_by;
    JoinClause join_clause{};
    bool has_join{false};
    OrderBy order_by{};
    bool has_order_by{false};
    size_t limit_count{0};
    bool has_limit{false};
};

struct DeleteStmt
{
    std::string table_name;
    Predicate where;
    bool has_where{false};
};

struct UpdateStmt
{
    std::string table_name;
    std::string column_name;
    Value new_value;
    Predicate where;
    bool has_where{false};
};

struct ColumnDef
{
    std::string name;
    ColumnType type;
};

struct CreateStmt
{
    std::string table_name;
    std::vector<ColumnDef> columns;
};

struct CreateIndexStmt
{
    std::string index_name;
    std::string table_name;
    std::string column_name;
};

struct VacuumStmt
{
    std::string table_name;
};

struct BeginStmt {};
struct CommitStmt {};
struct RollbackStmt {};

using Statement =
    std::variant<InsertStmt, SelectStmt, DeleteStmt, UpdateStmt, CreateStmt, CreateIndexStmt, VacuumStmt, BeginStmt, CommitStmt, RollbackStmt>;

// ──────────────────────────────────────────────
// Parser
// ──────────────────────────────────────────────

class Parser
{
public:
    [[nodiscard]] auto parse(std::string_view sql) -> StatusOr<Statement>;

private:
    std::string input_;
    size_t pos_{0};
    Token current_{};

    void next_token();
    auto peek() const -> TokenType { return current_.type; }
    auto consume(TokenType expected) -> Status;
    auto expect(TokenType expected) -> Status;

    // Grammar rules
    auto parse_statement() -> StatusOr<Statement>;
    auto parse_insert() -> StatusOr<InsertStmt>;
    auto parse_select() -> StatusOr<SelectStmt>;
    auto parse_delete() -> StatusOr<DeleteStmt>;
    auto parse_update() -> StatusOr<UpdateStmt>;
    auto parse_create() -> StatusOr<Statement>;
    auto parse_create_index() -> StatusOr<CreateIndexStmt>;
    auto parse_vacuum() -> StatusOr<VacuumStmt>;
    auto parse_begin() -> StatusOr<BeginStmt>;
    auto parse_commit() -> StatusOr<CommitStmt>;
    auto parse_rollback() -> StatusOr<RollbackStmt>;
    auto parse_values() -> StatusOr<std::vector<Value>>;
    auto parse_value() -> StatusOr<Value>;
    auto parse_predicate() -> StatusOr<Predicate>;
    auto parse_column_ref() -> StatusOr<ColumnRef>;
};

} // namespace rawdb

#endif // RAWDB_QUERY_PARSER_HPP
