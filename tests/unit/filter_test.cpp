#include <gtest/gtest.h>

#include <cstring>

#include "query/executor.hpp"

using rawdb::CmpOp;
using rawdb::ColumnData;
using rawdb::ColumnRef;
using rawdb::ColumnType;
using rawdb::Filter;
using rawdb::Predicate;
using rawdb::Schema;
using rawdb::Value;

// ──────────────────────────────────────────────
// Int32 filter
// ──────────────────────────────────────────────

TEST(FilterTest, Int32Eq)
{
    int32_t data[5] = {10, 20, 30, 40, 50};
    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kInt32, reinterpret_cast<const std::byte *>(data), sizeof(data), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "x"};
    pred.op = CmpOp::kEq;
    pred.value = Value{int64_t(30)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 5, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 1);
    EXPECT_EQ((*res)[0], 2);
}

TEST(FilterTest, Int32Gt)
{
    int32_t data[5] = {10, 20, 30, 40, 50};
    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kInt32, reinterpret_cast<const std::byte *>(data), sizeof(data), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "x"};
    pred.op = CmpOp::kGt;
    pred.value = Value{int64_t(25)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 5, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 3);
    EXPECT_EQ((*res)[0], 2);
    EXPECT_EQ((*res)[1], 3);
    EXPECT_EQ((*res)[2], 4);
}

// ──────────────────────────────────────────────
// Int64 filter
// ──────────────────────────────────────────────

TEST(FilterTest, Int64Range)
{
    int64_t data[4] = {100, 200, 300, 400};
    Schema schema;
    schema.columns = {ColumnType::kInt64};
    schema.names = {"val"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kInt64, reinterpret_cast<const std::byte *>(data), sizeof(data), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "val"};
    pred.op = CmpOp::kLe;
    pred.value = Value{int64_t(200)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 4, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 2);
    EXPECT_EQ((*res)[0], 0);
    EXPECT_EQ((*res)[1], 1);
}

// ──────────────────────────────────────────────
// Float64 filter
// ──────────────────────────────────────────────

TEST(FilterTest, Float64Ne)
{
    double data[3] = {1.5, 2.5, 3.5};
    Schema schema;
    schema.columns = {ColumnType::kFloat64};
    schema.names = {"y"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kFloat64, reinterpret_cast<const std::byte *>(data), sizeof(data), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "y"};
    pred.op = CmpOp::kNe;
    pred.value = Value{double(2.5)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 3, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 2);
    EXPECT_EQ((*res)[0], 0);
    EXPECT_EQ((*res)[1], 2);
}

// ──────────────────────────────────────────────
// Null handling
// ──────────────────────────────────────────────

TEST(FilterTest, SkipNulls)
{
    int32_t data[4] = {1, 2, 3, 4};
    uint8_t nulls[1] = {0b0101}; // rows 0, 2 are null

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kInt32, reinterpret_cast<const std::byte *>(data), sizeof(data), nulls});

    Predicate pred;
    pred.column = ColumnRef{"", "x"};
    pred.op = CmpOp::kEq;
    pred.value = Value{int64_t(2)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 4, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 1);
    EXPECT_EQ((*res)[0], 1);
}

// ──────────────────────────────────────────────
// VarChar filter
// ──────────────────────────────────────────────

TEST(FilterTest, VarCharEq)
{
    // Simulate ColumnData for VarChar: offset array + blob
    // Offsets: cumulative byte length per row
    uint32_t offsets[3] = {5, 8, 15}; // "alice"(5), "bob"(3), "charlie"(7)
    const char blob[] = "alicebobcharlie";
    size_t row_count = 3;
    std::vector<std::byte> vc_data(sizeof(uint32_t) * row_count);
    std::memcpy(vc_data.data(), offsets, sizeof(offsets));
    vc_data.insert(vc_data.end(),
                   reinterpret_cast<const std::byte *>(blob),
                   reinterpret_cast<const std::byte *>(blob) + sizeof(blob) - 1);

    Schema schema;
    schema.columns = {ColumnType::kVarChar};
    schema.names = {"name"};

    std::vector<ColumnData> cols;
    cols.push_back({ColumnType::kVarChar, vc_data.data(), vc_data.size(), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "name"};
    pred.op = CmpOp::kEq;
    pred.value = Value{std::string("bob")};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 3, &expr);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->size(), 1);
    EXPECT_EQ((*res)[0], 1);
}

// ──────────────────────────────────────────────
// Column not found
// ──────────────────────────────────────────────

TEST(FilterTest, ColumnNotFound)
{
    int32_t data[2] = {1, 2};
    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};

    std::vector<ColumnData> cols;
    cols.push_back(
        {ColumnType::kInt32, reinterpret_cast<const std::byte *>(data), sizeof(data), nullptr});

    Predicate pred;
    pred.column = ColumnRef{"", "nonexistent"};
    pred.op = CmpOp::kEq;
    pred.value = Value{int64_t(1)};

    rawdb::ExprNode expr;
    expr.type = rawdb::ExprNode::Type::kPredicate;
    expr.pred = pred;

    auto res = Filter::evaluate(cols, schema, 2, &expr);
    EXPECT_FALSE(res.has_value());
}
