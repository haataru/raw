#include "query/executor.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <algorithm>

using namespace rawdb;

TEST(ExecutorTest, Join)
{
    std::filesystem::path path = std::filesystem::temp_directory_path() / "rawdb_exec_join";
    std::filesystem::remove_all(path);
    Database db;
    ASSERT_EQ(db.open(path), Status::kOk);
    Connection conn(db);
    Executor exec(conn);

    // Create users table
    auto st_cu = exec.execute("CREATE TABLE users (id INT32, name VARCHAR)");
    ASSERT_TRUE(st_cu.has_value());
    
    // Create orders table
    auto st_co = exec.execute("CREATE TABLE orders (id INT32, user_id INT32, amount FLOAT64)");
    ASSERT_TRUE(st_co.has_value());

    // Insert users
    ASSERT_TRUE(exec.execute("INSERT INTO users VALUES (1, 'Alice')").has_value());
    ASSERT_TRUE(exec.execute("INSERT INTO users VALUES (2, 'Bob')").has_value());
    ASSERT_TRUE(exec.execute("INSERT INTO users VALUES (3, 'Charlie')").has_value());

    // Insert orders
    ASSERT_TRUE(exec.execute("INSERT INTO orders VALUES (101, 1, 50.5)").has_value());
    ASSERT_TRUE(exec.execute("INSERT INTO orders VALUES (102, 1, 120.0)").has_value());
    ASSERT_TRUE(exec.execute("INSERT INTO orders VALUES (103, 2, 75.25)").has_value());
    // No order for Charlie

    // Query with JOIN
    auto q_res = exec.execute("SELECT users.name, orders.amount FROM users JOIN orders ON users.id = orders.user_id");
    if (!q_res.has_value()) {
        std::cerr << "Query error: " << q_res.error() << std::endl;
    }
    ASSERT_TRUE(q_res.has_value());
    
    const auto &res = q_res.value();
    ASSERT_EQ(res.column_names.size(), 2);
    EXPECT_EQ(res.column_names[0], "users.name");
    EXPECT_EQ(res.column_names[1], "orders.amount");
    
    ASSERT_EQ(res.rows.size(), 3);
    
    // Check results
    // Wait, order is not guaranteed because of hash table.
    // Let's sort the results to verify easily
    auto sorted_rows = res.rows;
    std::sort(sorted_rows.begin(), sorted_rows.end(), [](const auto &a, const auto &b) {
        if (a[0] != b[0]) return a[0] < b[0];
        return std::stod(a[1]) < std::stod(b[1]);
    });
    
    EXPECT_EQ(sorted_rows[0][0], "Alice");
    EXPECT_EQ(sorted_rows[0][1], "50.500000"); // std::to_string output formatting
    EXPECT_EQ(sorted_rows[1][0], "Alice");
    EXPECT_EQ(sorted_rows[1][1], "120.000000");
    EXPECT_EQ(sorted_rows[2][0], "Bob");
    EXPECT_EQ(sorted_rows[2][1], "75.250000");
}

TEST(ExecutorTest, InsertAndSelect)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_test";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    auto res = exec.execute("INSERT INTO users VALUES (1, 'alice')");
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->rows.size(), 1);

    auto sel = exec.execute("SELECT * FROM users");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1);
    ASSERT_EQ(sel->rows[0].size(), 2);
    EXPECT_EQ(sel->rows[0][0], "1");
    EXPECT_EQ(sel->rows[0][1], "alice");
    EXPECT_EQ(sel->column_names.size(), 2);
    EXPECT_EQ(sel->column_names[0], "id");
    EXPECT_EQ(sel->column_names[1], "name");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, InsertMultipleRows)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_multi";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt64, ColumnType::kFloat64};
    schema.names = {"x", "y"};
    *db.create_table("points", schema);

    Connection conn(db);
    Executor exec(conn);
    auto res = exec.execute("INSERT INTO points VALUES (10, 3.14), (20, 6.28)");
    ASSERT_TRUE(res.has_value());

    auto sel = exec.execute("SELECT * FROM points");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);
    EXPECT_EQ(sel->rows[0][0], "10");
    EXPECT_EQ(sel->rows[0][1], "3.140000");
    EXPECT_EQ(sel->rows[1][0], "20");
    EXPECT_EQ(sel->rows[1][1], "6.280000");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectWithFilter)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_filter";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    auto sel = exec.execute("SELECT * FROM users WHERE id >= 2");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);
    EXPECT_EQ(sel->rows[0][1], "bob");
    EXPECT_EQ(sel->rows[1][1], "charlie");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectSpecificColumns)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_cols";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar, ColumnType::kFloat64};
    schema.names = {"id", "name", "score"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice', 95.5)");

    auto sel = exec.execute("SELECT name, id FROM users WHERE name = 'alice'");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1);
    ASSERT_EQ(sel->rows[0].size(), 2);
    EXPECT_EQ(sel->rows[0][0], "alice");
    EXPECT_EQ(sel->rows[0][1], "1");
    EXPECT_EQ(sel->column_names[0], "name");
    EXPECT_EQ(sel->column_names[1], "id");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, UnknownTable)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_unk";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Connection conn(db);
    Executor exec(conn);
    auto res = exec.execute("SELECT * FROM nonexistent");
    EXPECT_FALSE(res.has_value());

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, DeleteWithFilter)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_del_filter";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    auto del = exec.execute("DELETE FROM users WHERE id >= 2");
    ASSERT_TRUE(del.has_value());
    ASSERT_EQ(del->rows.size(), 1);
    EXPECT_EQ(del->rows[0][0], "2"); // 2 rows deleted

    auto sel = exec.execute("SELECT * FROM users");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1);
    EXPECT_EQ(sel->rows[0][0], "1");
    EXPECT_EQ(sel->rows[0][1], "alice");

    auto sel2 = exec.execute("SELECT * FROM users WHERE name = 'bob'");
    ASSERT_TRUE(sel2.has_value());
    EXPECT_EQ(sel2->rows.size(), 0);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, DeleteAllRows)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_del_all";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt64};
    schema.names = {"val"};
    *db.create_table("nums", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO nums VALUES (10)");
    exec.execute("INSERT INTO nums VALUES (20)");
    exec.execute("INSERT INTO nums VALUES (30)");

    auto del = exec.execute("DELETE FROM nums");
    ASSERT_TRUE(del.has_value());
    EXPECT_EQ(del->rows[0][0], "3");

    auto sel = exec.execute("SELECT * FROM nums");
    ASSERT_TRUE(sel.has_value());
    EXPECT_EQ(sel->rows.size(), 0);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectWithOrderBy)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_ord";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    auto sel = exec.execute("SELECT name FROM users ORDER BY id");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 3);
    EXPECT_EQ(sel->rows[0][0], "alice");
    EXPECT_EQ(sel->rows[1][0], "bob");
    EXPECT_EQ(sel->rows[2][0], "charlie");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectWithOrderByDesc)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_ord_desc";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    auto sel = exec.execute("SELECT name FROM users ORDER BY id DESC");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 3);
    EXPECT_EQ(sel->rows[0][0], "charlie");
    EXPECT_EQ(sel->rows[1][0], "bob");
    EXPECT_EQ(sel->rows[2][0], "alice");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectWithLimit)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_lim";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    *db.create_table("nums", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO nums VALUES (10)");
    exec.execute("INSERT INTO nums VALUES (20)");
    exec.execute("INSERT INTO nums VALUES (30)");

    auto sel = exec.execute("SELECT * FROM nums LIMIT 2");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, SelectWithOrderByAndLimit)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_ord_lim";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");

    auto sel = exec.execute("SELECT name FROM users ORDER BY id ASC LIMIT 2");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);
    EXPECT_EQ(sel->rows[0][0], "alice");
    EXPECT_EQ(sel->rows[1][0], "bob");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, OrderByVarCharUtf8)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_utf8";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    // Cyrillic: а < б < в in UTF-8 code-point order (U+0430 < U+0431 < U+0432)
    // Latin: a < b < c
    exec.execute("INSERT INTO users VALUES (3, '\xd0\xb2')"); // в
    exec.execute("INSERT INTO users VALUES (1, '\xd0\xb0')"); // а
    exec.execute("INSERT INTO users VALUES (2, '\xd0\xb1')"); // б
    exec.execute("INSERT INTO users VALUES (6, 'c')");
    exec.execute("INSERT INTO users VALUES (4, 'a')");
    exec.execute("INSERT INTO users VALUES (5, 'b')");

    // ASCII < Cyrillic in byte order (0x6x < 0xD0)
    auto sel = exec.execute("SELECT id, name FROM users ORDER BY name");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 6);
    EXPECT_EQ(sel->rows[0][1], "a");
    EXPECT_EQ(sel->rows[1][1], "b");
    EXPECT_EQ(sel->rows[2][1], "c");
    EXPECT_EQ(sel->rows[3][1], "\xd0\xb0"); // а
    EXPECT_EQ(sel->rows[4][1], "\xd0\xb1"); // б
    EXPECT_EQ(sel->rows[5][1], "\xd0\xb2"); // в

    auto sel2 = exec.execute("SELECT id FROM users ORDER BY name DESC");
    ASSERT_TRUE(sel2.has_value());
    ASSERT_EQ(sel2->rows.size(), 6);
    EXPECT_EQ(sel2->rows[0][0], "3"); // первая в DESC = в

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, VacuumRemovesDeletedRows)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_vac";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    exec.execute("DELETE FROM users WHERE id = 2");

    auto vac = exec.execute("VACUUM users");
    ASSERT_TRUE(vac.has_value());

    auto sel = exec.execute("SELECT * FROM users ORDER BY id");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);
    EXPECT_EQ(sel->rows[0][0], "1");
    EXPECT_EQ(sel->rows[0][1], "alice");
    EXPECT_EQ(sel->rows[1][0], "3");
    EXPECT_EQ(sel->rows[1][1], "charlie");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, VacuumAllRowsDeleted)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_vac_all";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32};
    schema.names = {"x"};
    *db.create_table("nums", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO nums VALUES (10)");
    exec.execute("INSERT INTO nums VALUES (20)");

    exec.execute("DELETE FROM nums");

    auto vac = exec.execute("VACUUM nums");
    ASSERT_TRUE(vac.has_value());

    auto sel = exec.execute("SELECT * FROM nums");
    ASSERT_TRUE(sel.has_value());
    EXPECT_EQ(sel->rows.size(), 0);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, VacuumNoDeletesIsNoOp)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_vac_nop";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt64, ColumnType::kFloat64};
    schema.names = {"x", "y"};
    *db.create_table("points", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO points VALUES (1, 1.5)");
    exec.execute("INSERT INTO points VALUES (2, 2.5)");

    auto vac = exec.execute("VACUUM points");
    ASSERT_TRUE(vac.has_value());

    auto sel = exec.execute("SELECT * FROM points ORDER BY x");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 2);
    EXPECT_EQ(sel->rows[0][0], "1");
    EXPECT_EQ(sel->rows[1][0], "2");

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, VacuumWithIndex)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_vac_idx";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    exec.execute("CREATE INDEX id_idx ON users (id)");

    exec.execute("DELETE FROM users WHERE id = 2");

    auto vac = exec.execute("VACUUM users");
    ASSERT_TRUE(vac.has_value());

    auto sel = exec.execute("SELECT name FROM users WHERE id = 1");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1);
    EXPECT_EQ(sel->rows[0][0], "alice");

    auto sel2 = exec.execute("SELECT name FROM users WHERE id = 2");
    ASSERT_TRUE(sel2.has_value());
    EXPECT_EQ(sel2->rows.size(), 0);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, VacuumPersistence)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_vac_persist";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar};
    schema.names = {"id", "name"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice')");
    exec.execute("INSERT INTO users VALUES (2, 'bob')");
    exec.execute("INSERT INTO users VALUES (3, 'charlie')");

    exec.execute("CREATE INDEX id_idx ON users (id)");

    exec.execute("DELETE FROM users WHERE id >= 2");
    exec.execute("VACUUM users");

    db.close();

    ASSERT_EQ(db.open(path), Status::kOk);
    Connection conn2(db);
    Executor exec2(conn2);

    auto sel = exec2.execute("SELECT * FROM users");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1);
    EXPECT_EQ(sel->rows[0][0], "1");

    auto sel2 = exec2.execute("SELECT name FROM users WHERE id = 1");
    ASSERT_TRUE(sel2.has_value());
    ASSERT_EQ(sel2->rows.size(), 1);

    db.close();
    std::filesystem::remove_all(path);
}

TEST(ExecutorTest, UpdateStatement)
{
    Database db;
    auto path = std::filesystem::temp_directory_path() / "rawdb_exec_update";
    std::filesystem::remove_all(path);
    ASSERT_EQ(db.open(path), Status::kOk);

    Schema schema;
    schema.columns = {ColumnType::kInt32, ColumnType::kVarChar, ColumnType::kFloat64};
    schema.names = {"id", "name", "score"};
    *db.create_table("users", schema);

    Connection conn(db);
    Executor exec(conn);
    exec.execute("INSERT INTO users VALUES (1, 'alice', 95.5)");
    exec.execute("INSERT INTO users VALUES (2, 'bob', 80.0)");
    exec.execute("INSERT INTO users VALUES (3, 'charlie', 70.0)");

    // Update with filter
    auto upd = exec.execute("UPDATE users SET score = 100.0 WHERE id = 2");
    ASSERT_TRUE(upd.has_value());
    ASSERT_EQ(upd->rows.size(), 1);
    EXPECT_EQ(upd->rows[0][0], "1"); // 1 row updated

    // Update without filter (all rows)
    auto upd_all = exec.execute("UPDATE users SET name = 'anonymous'");
    ASSERT_TRUE(upd_all.has_value());
    EXPECT_EQ(upd_all->rows[0][0], "3"); // 3 rows updated

    // Verify
    auto sel = exec.execute("SELECT id, name, score FROM users ORDER BY id");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 3);
    EXPECT_EQ(sel->rows[0][0], "1");
    EXPECT_EQ(sel->rows[0][1], "anonymous");
    EXPECT_EQ(sel->rows[0][2], "95.500000");

    EXPECT_EQ(sel->rows[1][0], "2");
    EXPECT_EQ(sel->rows[1][1], "anonymous");
    EXPECT_EQ(sel->rows[1][2], "100.000000");

    EXPECT_EQ(sel->rows[2][0], "3");
    EXPECT_EQ(sel->rows[2][1], "anonymous");
    EXPECT_EQ(sel->rows[2][2], "70.000000");

    db.close();
    std::filesystem::remove_all(path);
}
