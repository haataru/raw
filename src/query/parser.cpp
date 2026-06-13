#include "query/parser.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace rawdb
{

// ──────────────────────────────────────────────
// Tokenizer
// ──────────────────────────────────────────────

static auto is_keyword(std::string_view s) -> TokenType
{
    if (s == "insert")
        return TokenType::kInsert;
    if (s == "into")
        return TokenType::kInto;
    if (s == "values")
        return TokenType::kValues;
    if (s == "select")
        return TokenType::kSelect;
    if (s == "from")
        return TokenType::kFrom;
    if (s == "where")
        return TokenType::kWhere;
    if (s == "delete")
        return TokenType::kDelete;
    if (s == "create")
        return TokenType::kCreate;
    if (s == "table")
        return TokenType::kTable;
    if (s == "index")
        return TokenType::kIndex;
    if (s == "on")
        return TokenType::kOn;
    if (s == "order")
        return TokenType::kOrder;
    if (s == "by")
        return TokenType::kBy;
    if (s == "asc")
        return TokenType::kAsc;
    if (s == "desc")
        return TokenType::kDesc;
    if (s == "limit")
        return TokenType::kLimit;
    if (s == "vacuum")
        return TokenType::kVacuum;
    if (s == "update")
        return TokenType::kUpdate;
    if (s == "set")
        return TokenType::kSet;
    return TokenType::kIdentifier;
}

void Parser::next_token()
{
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
    }

    if (pos_ >= input_.size()) {
        current_ = {TokenType::kEnd, ""};
        return;
    }

    char c = input_[pos_];

    switch (c) {
        case '*':
            ++pos_;
            current_ = {TokenType::kStar, "*"};
            return;
        case ',':
            ++pos_;
            current_ = {TokenType::kComma, ","};
            return;
        case '(':
            ++pos_;
            current_ = {TokenType::kLParen, "("};
            return;
        case ')':
            ++pos_;
            current_ = {TokenType::kRParen, ")"};
            return;
        case ';':
            ++pos_;
            current_ = {TokenType::kSemicolon, ";"};
            return;
    }

    if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
        pos_ += 2;
        current_ = {TokenType::kNe, "!="};
        return;
    }
    if (c == '<' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
        pos_ += 2;
        current_ = {TokenType::kLe, "<="};
        return;
    }
    if (c == '>' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
        pos_ += 2;
        current_ = {TokenType::kGe, ">="};
        return;
    }

    if (c == '=') {
        ++pos_;
        current_ = {TokenType::kEq, "="};
        return;
    }
    if (c == '<') {
        ++pos_;
        current_ = {TokenType::kLt, "<"};
        return;
    }
    if (c == '>') {
        ++pos_;
        current_ = {TokenType::kGt, ">"};
        return;
    }

    if (c == '\'') {
        ++pos_;
        size_t start = pos_;
        while (pos_ < input_.size() && input_[pos_] != '\'') {
            ++pos_;
        }
        current_ = {TokenType::kString, input_.substr(start, pos_ - start)};
        if (pos_ < input_.size())
            ++pos_;
        return;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t start = pos_;
        while (pos_ < input_.size() &&
               (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
            ++pos_;
        }
        std::string word = input_.substr(start, pos_ - start);
        // Normalize to lowercase for keyword matching
        for (auto &ch : word)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto kw = is_keyword(word);
        if (kw != TokenType::kIdentifier) {
            current_ = {kw, word};
        }
        else {
            current_ = {TokenType::kIdentifier, input_.substr(start, pos_ - start)};
        }
        return;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
        size_t start = pos_;
        if (c == '-')
            ++pos_;
        while (pos_ < input_.size() &&
               (std::isdigit(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '.')) {
            ++pos_;
        }
        current_ = {TokenType::kNumber, input_.substr(start, pos_ - start)};
        return;
    }

    ++pos_;
    next_token();
}

auto Parser::consume(TokenType expected) -> Status
{
    if (current_.type != expected) {
        return Status::kInvalidArgument;
    }
    next_token();
    return Status::kOk;
}

auto Parser::expect(TokenType expected) -> Status
{
    if (current_.type != expected) {
        return Status::kInvalidArgument;
    }
    return Status::kOk;
}

// ──────────────────────────────────────────────
// Parser
// ──────────────────────────────────────────────

auto Parser::parse(std::string_view sql) -> StatusOr<Statement>
{
    input_ = std::string(sql);
    pos_ = 0;
    next_token();
    auto stmt = parse_statement();
    if (!stmt)
        return stmt;
    if (peek() == TokenType::kSemicolon) {
        next_token();
    }
    if (peek() != TokenType::kEnd) {
        return std::unexpected(Status::kInvalidArgument);
    }
    return stmt;
}

auto Parser::parse_statement() -> StatusOr<Statement>
{
    switch (peek()) {
        case TokenType::kInsert:
            return parse_insert();
        case TokenType::kSelect:
            return parse_select();
        case TokenType::kDelete:
            return parse_delete();
        case TokenType::kUpdate:
            return parse_update();
        case TokenType::kCreate:
            return parse_create();
        case TokenType::kVacuum:
            return parse_vacuum();
        default:
            return std::unexpected(Status::kInvalidArgument);
    }
}

auto Parser::parse_insert() -> StatusOr<InsertStmt>
{
    auto st = consume(TokenType::kInsert);
    if (st != Status::kOk)
        return std::unexpected(st);

    st = consume(TokenType::kInto);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    InsertStmt stmt;
    stmt.table_name = current_.text;
    next_token();

    st = consume(TokenType::kValues);
    if (st != Status::kOk)
        return std::unexpected(st);

    do {
        auto row = parse_values();
        if (!row)
            return std::unexpected(row.error());
        stmt.rows.push_back(std::move(*row));
    } while (peek() == TokenType::kComma && (consume(TokenType::kComma), true));

    return stmt;
}

auto Parser::parse_values() -> StatusOr<std::vector<Value>>
{
    auto st = consume(TokenType::kLParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    std::vector<Value> values;
    do {
        auto val = parse_value();
        if (!val)
            return std::unexpected(val.error());
        values.push_back(std::move(*val));
    } while (peek() == TokenType::kComma && (consume(TokenType::kComma), true));

    st = consume(TokenType::kRParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    return values;
}

auto Parser::parse_value() -> StatusOr<Value>
{
    if (peek() == TokenType::kNumber) {
        Value v;
        std::string_view text = current_.text;
        if (text.find('.') != std::string_view::npos) {
            char *end = nullptr;
            double d = std::strtod(text.data(), &end);
            if (end != text.data() + text.size())
                return std::unexpected(Status::kInvalidArgument);
            v.data = d;
        }
        else {
            int64_t n;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (ec != std::errc())
                return std::unexpected(Status::kInvalidArgument);
            v.data = n;
        }
        next_token();
        return v;
    }

    if (peek() == TokenType::kString) {
        Value v;
        v.data = std::string(current_.text);
        next_token();
        return v;
    }

    return std::unexpected(Status::kInvalidArgument);
}

auto Parser::parse_select() -> StatusOr<SelectStmt>
{
    auto st = consume(TokenType::kSelect);
    if (st != Status::kOk)
        return std::unexpected(st);

    SelectStmt stmt;

    if (peek() == TokenType::kStar) {
        next_token();
    }
    else {
        do {
            auto col = parse_column_ref();
            if (!col)
                return std::unexpected(col.error());
            stmt.columns.push_back(std::move(*col));
        } while (peek() == TokenType::kComma && (consume(TokenType::kComma), true));
    }

    st = consume(TokenType::kFrom);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    stmt.table_name = current_.text;
    next_token();

    if (peek() == TokenType::kWhere) {
        next_token();
        auto pred = parse_predicate();
        if (!pred)
            return std::unexpected(pred.error());
        stmt.where = std::move(*pred);
        stmt.has_where = true;
    }

    if (peek() == TokenType::kOrder) {
        auto st2 = consume(TokenType::kOrder);
        if (st2 != Status::kOk)
            return std::unexpected(st2);
        st2 = consume(TokenType::kBy);
        if (st2 != Status::kOk)
            return std::unexpected(st2);
        auto col = parse_column_ref();
        if (!col)
            return std::unexpected(col.error());
        stmt.order_by.column = std::move(*col);
        stmt.order_by.asc = true;
        if (peek() == TokenType::kAsc) {
            next_token();
        }
        else if (peek() == TokenType::kDesc) {
            stmt.order_by.asc = false;
            next_token();
        }
        stmt.has_order_by = true;
    }

    if (peek() == TokenType::kLimit) {
        auto st2 = consume(TokenType::kLimit);
        if (st2 != Status::kOk)
            return std::unexpected(st2);
        if (peek() != TokenType::kNumber) {
            return std::unexpected(Status::kInvalidArgument);
        }
        auto [ptr, ec] = std::from_chars(current_.text.data(),
                                         current_.text.data() + current_.text.size(),
                                         stmt.limit_count);
        if (ec != std::errc())
            return std::unexpected(Status::kInvalidArgument);
        next_token();
        stmt.has_limit = true;
    }

    return stmt;
}

auto Parser::parse_delete() -> StatusOr<DeleteStmt>
{
    auto st = consume(TokenType::kDelete);
    if (st != Status::kOk)
        return std::unexpected(st);

    st = consume(TokenType::kFrom);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    DeleteStmt stmt;
    stmt.table_name = current_.text;
    next_token();

    if (peek() == TokenType::kWhere) {
        next_token();
        auto pred = parse_predicate();
        if (!pred)
            return std::unexpected(pred.error());
        stmt.where = std::move(*pred);
        stmt.has_where = true;
    }

    return stmt;
}

auto Parser::parse_update() -> StatusOr<UpdateStmt>
{
    auto st = consume(TokenType::kUpdate);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    UpdateStmt stmt;
    stmt.table_name = current_.text;
    next_token();

    st = consume(TokenType::kSet);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    stmt.column_name = current_.text;
    next_token();

    st = consume(TokenType::kEq);
    if (st != Status::kOk)
        return std::unexpected(st);

    auto val = parse_value();
    if (!val)
        return std::unexpected(val.error());
    stmt.new_value = std::move(*val);

    if (peek() == TokenType::kWhere) {
        next_token();
        auto pred = parse_predicate();
        if (!pred)
            return std::unexpected(pred.error());
        stmt.where = std::move(*pred);
        stmt.has_where = true;
    }

    return stmt;
}

auto Parser::parse_predicate() -> StatusOr<Predicate>
{
    auto col = parse_column_ref();
    if (!col)
        return std::unexpected(col.error());

    Predicate pred;
    pred.column = std::move(*col);

    switch (peek()) {
        case TokenType::kEq:
            pred.op = CmpOp::kEq;
            break;
        case TokenType::kNe:
            pred.op = CmpOp::kNe;
            break;
        case TokenType::kLt:
            pred.op = CmpOp::kLt;
            break;
        case TokenType::kGt:
            pred.op = CmpOp::kGt;
            break;
        case TokenType::kLe:
            pred.op = CmpOp::kLe;
            break;
        case TokenType::kGe:
            pred.op = CmpOp::kGe;
            break;
        default:
            return std::unexpected(Status::kInvalidArgument);
    }
    next_token();

    auto val = parse_value();
    if (!val)
        return std::unexpected(val.error());
    pred.value = std::move(*val);

    return pred;
}

auto Parser::parse_column_ref() -> StatusOr<ColumnRef>
{
    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    ColumnRef ref;
    ref.name = current_.text;
    next_token();
    return ref;
}

auto Parser::parse_create() -> StatusOr<Statement>
{
    auto st = consume(TokenType::kCreate);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() == TokenType::kIndex) {
        return parse_create_index();
    }
    if (peek() != TokenType::kTable) {
        return std::unexpected(Status::kInvalidArgument);
    }
    st = consume(TokenType::kTable);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    CreateStmt stmt;
    stmt.table_name = current_.text;
    next_token();

    st = consume(TokenType::kLParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    do {
        if (peek() != TokenType::kIdentifier) {
            return std::unexpected(Status::kInvalidArgument);
        }
        ColumnDef col;
        col.name = current_.text;
        next_token();

        if (peek() == TokenType::kIdentifier) {
            std::string type_text = current_.text;
            for (auto &ch : type_text)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (type_text == "int32") {
                col.type = ColumnType::kInt32;
            }
            else if (type_text == "int64") {
                col.type = ColumnType::kInt64;
            }
            else if (type_text == "float64" || type_text == "float" || type_text == "double") {
                col.type = ColumnType::kFloat64;
            }
            else if (type_text == "bool" || type_text == "boolean") {
                col.type = ColumnType::kBool;
            }
            else {
                col.type = ColumnType::kVarChar;
            }
            next_token();
        }
        else {
            col.type = ColumnType::kVarChar;
        }
        stmt.columns.push_back(std::move(col));
    } while (peek() == TokenType::kComma && (consume(TokenType::kComma), true));

    st = consume(TokenType::kRParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    return stmt;
}

auto Parser::parse_create_index() -> StatusOr<CreateIndexStmt>
{
    auto st = consume(TokenType::kIndex);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    CreateIndexStmt stmt;
    stmt.index_name = current_.text;
    next_token();

    st = consume(TokenType::kOn);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    stmt.table_name = current_.text;
    next_token();

    st = consume(TokenType::kLParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    stmt.column_name = current_.text;
    next_token();

    st = consume(TokenType::kRParen);
    if (st != Status::kOk)
        return std::unexpected(st);

    return stmt;
}

auto Parser::parse_vacuum() -> StatusOr<VacuumStmt>
{
    auto st = consume(TokenType::kVacuum);
    if (st != Status::kOk)
        return std::unexpected(st);

    if (peek() != TokenType::kIdentifier) {
        return std::unexpected(Status::kInvalidArgument);
    }
    VacuumStmt stmt;
    stmt.table_name = current_.text;
    next_token();

    return stmt;
}

} // namespace rawdb
