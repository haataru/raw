#include "rawdb.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "core/types.hpp"
#include "db/database.hpp"
#include "query/connection.hpp"
#include "query/executor.hpp"

struct rawdb_t
{
    rawdb::Database db;
    std::string errmsg;
};

struct rawdb_result_t
{
    std::vector<std::string> column_names;
    std::vector<std::vector<std::string>> rows;
};

rawdb_t *rawdb_open(const char *path)
{
    auto *db = new rawdb_t();
    auto st = db->db.open(path);
    if (st != rawdb::Status::kOk) {
        db->errmsg = "failed to open database";
        return db;
    }
    return db;
}

void rawdb_close(rawdb_t *db)
{
    if (db) {
        db->db.close();
        delete db;
    }
}

int rawdb_execute(rawdb_t *db, const char *sql, rawdb_result_t **out_result)
{
    if (!db || !sql)
        return -1;

    rawdb::Connection conn(db->db);
    rawdb::Executor exec(conn);
    auto r = exec.execute(sql);
    if (!r) {
        db->errmsg = std::string(rawdb::status_message(r.error().code));
        return static_cast<int>(r.error().code);
    }

    auto *res = new rawdb_result_t();
    res->column_names = std::move(r->column_names);
    res->rows = std::move(r->rows);

    if (out_result) {
        *out_result = res;
    }
    else {
        delete res;
    }
    return 0;
}

int rawdb_result_row_count(const rawdb_result_t *result)
{
    if (!result)
        return 0;
    auto n = result->rows.size();
    return static_cast<int>(n);
}

int rawdb_result_col_count(const rawdb_result_t *result)
{
    if (!result)
        return 0;
    auto n = result->column_names.size();
    return static_cast<int>(n);
}

const char *rawdb_result_column_name(const rawdb_result_t *result, int col)
{
    if (!result || col < 0)
        return nullptr;
    auto c = static_cast<size_t>(col);
    if (c >= result->column_names.size())
        return nullptr;
    return result->column_names[c].c_str();
}

const char *rawdb_result_value(const rawdb_result_t *result, int row, int col)
{
    if (!result || row < 0 || col < 0)
        return nullptr;
    auto r = static_cast<size_t>(row);
    auto c = static_cast<size_t>(col);
    if (r >= result->rows.size())
        return nullptr;
    if (c >= result->rows[r].size())
        return nullptr;
    return result->rows[r][c].c_str();
}

void rawdb_result_free(rawdb_result_t *result) { delete result; }

const char *rawdb_error_message(rawdb_t *db) { return db ? db->errmsg.c_str() : "null db"; }
