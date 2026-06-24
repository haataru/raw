#include "query/parser.hpp"

#include <gtest/gtest.h>

using rawdb::DeleteStmt;
using rawdb::InsertStmt;
using rawdb::Parser;
using rawdb::SelectStmt;
using rawdb::Status;

// ──────────────────────────────────────────────
// INSERT
// ──────────────────────────────────────────────

TEST(ParserTest, InsertSingleRow)
{
    Parser p;
    auto res = p.parse("INSERT INTO users VALUES (1, 'alice')");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<InsertStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->table_name, "users");
    ASSERT_EQ(stmt->rows.size(), 1);
    ASSERT_EQ(stmt->rows[0].size(), 2);
    EXPECT_EQ(std::get<int64_t>(stmt->rows[0][0].data), 1);
    EXPECT_EQ(std::get<std::string>(stmt->rows[0][1].data), "alice");
}

TEST(ParserTest, InsertMultipleRows)
{
    Parser p;
    auto res = p.parse("INSERT INTO t VALUES (1, 'a'), (2, 'b')");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<InsertStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(stmt->rows.size(), 2);
    ASSERT_EQ(stmt->rows[0].size(), 2);
    EXPECT_EQ(std::get<int64_t>(stmt->rows[0][0].data), 1);
    EXPECT_EQ(std::get<int64_t>(stmt->rows[1][0].data), 2);
}

// ──────────────────────────────────────────────
// SELECT
// ──────────────────────────────────────────────

TEST(ParserTest, SelectStar)
{
    Parser p;
    auto res = p.parse("SELECT * FROM users");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->columns.empty());
    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_FALSE(stmt->has_where);
}

TEST(ParserTest, ParseJoin)
{
    rawdb::Parser parser;
    auto ast = parser.parse(
        "SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id");
    if (!ast.has_value()) {
        std::cerr << "Parser error: " << ast.error() << std::endl;
    }
    ASSERT_TRUE(ast.has_value());

    auto *stmt = std::get_if<rawdb::SelectStmt>(&ast.value());
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_TRUE(stmt->has_join);
    EXPECT_EQ(stmt->join_clause.table_name, "orders");
    EXPECT_EQ(stmt->join_clause.left_col.table, "users");
    EXPECT_EQ(stmt->join_clause.left_col.name, "id");
    EXPECT_EQ(stmt->join_clause.right_col.table, "orders");
    EXPECT_EQ(stmt->join_clause.right_col.name, "user_id");

    ASSERT_EQ(stmt->columns.size(), 2);
    EXPECT_EQ(stmt->columns[0].table, "users");
    EXPECT_EQ(stmt->columns[0].name, "name");
    EXPECT_EQ(stmt->columns[1].table, "orders");
    EXPECT_EQ(stmt->columns[1].name, "amount");
}

TEST(ParserTest, SelectSpecificColumns)
{
    Parser p;
    auto res = p.parse("SELECT name, age FROM users");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(stmt->columns.size(), 2);
    EXPECT_EQ(stmt->columns[0].name, "name");
    EXPECT_EQ(stmt->columns[1].name, "age");
}

TEST(ParserTest, SelectWithWhere)
{
    Parser p;
    auto res = p.parse("SELECT * FROM users WHERE age >= 18");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_where);
    EXPECT_EQ(stmt->where->type, rawdb::ExprNode::Type::kPredicate);
    EXPECT_EQ(stmt->where->pred.column.name, "age");
    EXPECT_EQ(stmt->where->pred.op, rawdb::CmpOp::kGe);
    EXPECT_EQ(std::get<int64_t>(stmt->where->pred.value.data), 18);
}

// ──────────────────────────────────────────────
// DELETE
// ──────────────────────────────────────────────

TEST(ParserTest, DeleteAll)
{
    Parser p;
    auto res = p.parse("DELETE FROM users");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<DeleteStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->table_name, "users");
    EXPECT_FALSE(stmt->has_where);
}

TEST(ParserTest, DeleteWithWhere)
{
    Parser p;
    auto res = p.parse("DELETE FROM users WHERE name = 'bob'");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<DeleteStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_where);
    EXPECT_EQ(stmt->where->type, rawdb::ExprNode::Type::kPredicate);
    EXPECT_EQ(stmt->where->pred.column.name, "name");
    EXPECT_EQ(stmt->where->pred.op, rawdb::CmpOp::kEq);
    EXPECT_EQ(std::get<std::string>(stmt->where->pred.value.data), "bob");
}

// ──────────────────────────────────────────────
// Error handling
// ──────────────────────────────────────────────

TEST(ParserTest, InvalidSyntax)
{
    Parser p;
    auto res = p.parse("BLARGO");
    EXPECT_FALSE(res.has_value());
}

TEST(ParserTest, MissingValues)
{
    Parser p;
    auto res = p.parse("INSERT INTO t");
    EXPECT_FALSE(res.has_value());
}

// ──────────────────────────────────────────────
// Semicolon terminator
// ──────────────────────────────────────────────

TEST(ParserTest, SelectWithSemicolon)
{
    Parser p;
    auto res = p.parse("SELECT * FROM users;");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->columns.empty());
    EXPECT_EQ(stmt->table_name, "users");
}

TEST(ParserTest, InsertWithSemicolon)
{
    Parser p;
    auto res = p.parse("INSERT INTO t VALUES (1, 'a');");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<InsertStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->table_name, "t");
}

TEST(ParserTest, TrailingJunkAfterSemicolon)
{
    Parser p;
    auto res = p.parse("SELECT * FROM t; DELETE FROM u");
    EXPECT_FALSE(res.has_value());
}

TEST(ParserTest, TrailingJunk)
{
    Parser p;
    auto res = p.parse("SELECT * FROM t blah");
    EXPECT_FALSE(res.has_value());
}

// ──────────────────────────────────────────────
// ORDER BY
// ──────────────────────────────────────────────

TEST(ParserTest, SelectOrderBy)
{
    Parser p;
    auto res = p.parse("SELECT name FROM users ORDER BY id");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_order_by);
    EXPECT_EQ(stmt->order_by.column.name, "id");
    EXPECT_TRUE(stmt->order_by.asc);
}

TEST(ParserTest, SelectOrderByDesc)
{
    Parser p;
    auto res = p.parse("SELECT * FROM t ORDER BY x DESC");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_order_by);
    EXPECT_EQ(stmt->order_by.column.name, "x");
    EXPECT_FALSE(stmt->order_by.asc);
}

TEST(ParserTest, SelectOrderByAsc)
{
    Parser p;
    auto res = p.parse("SELECT * FROM t ORDER BY x ASC");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_order_by);
    EXPECT_TRUE(stmt->order_by.asc);
}

// ──────────────────────────────────────────────
// LIMIT
// ──────────────────────────────────────────────

TEST(ParserTest, SelectLimit)
{
    Parser p;
    auto res = p.parse("SELECT * FROM t LIMIT 10");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_limit);
    EXPECT_EQ(stmt->limit_count, 10);
}

TEST(ParserTest, SelectOrderByAndLimit)
{
    Parser p;
    auto res = p.parse("SELECT name FROM t ORDER BY id DESC LIMIT 5");
    ASSERT_TRUE(res.has_value());
    auto *stmt = std::get_if<SelectStmt>(&*res);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->has_order_by);
    EXPECT_EQ(stmt->order_by.column.name, "id");
    EXPECT_FALSE(stmt->order_by.asc);
    EXPECT_TRUE(stmt->has_limit);
    EXPECT_EQ(stmt->limit_count, 5);
}
