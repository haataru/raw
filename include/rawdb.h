#ifndef RAWDB_C_API_H
#define RAWDB_C_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct rawdb_t rawdb_t;
    typedef struct rawdb_result_t rawdb_result_t;

    /// Open or create a database at the given directory.
    rawdb_t *rawdb_open(const char *path);

    /// Close a database and free resources.
    void rawdb_close(rawdb_t *db);

    /// Execute a SQL statement. Returns 0 on success.
    int rawdb_execute(rawdb_t *db, const char *sql, rawdb_result_t **out_result);

    /// Number of rows in the result set.
    int rawdb_result_row_count(const rawdb_result_t *result);

    /// Number of columns in the result set.
    int rawdb_result_col_count(const rawdb_result_t *result);

    /// Column name at the given index (0-based).
    const char *rawdb_result_column_name(const rawdb_result_t *result, int col);

    /// Value at the given row/column (0-based).
    const char *rawdb_result_value(const rawdb_result_t *result, int row, int col);

    /// Free a result set.
    void rawdb_result_free(rawdb_result_t *result);

    /// Return the last error message for this database.
    const char *rawdb_error_message(rawdb_t *db);

#ifdef __cplusplus
}
#endif

#endif // RAWDB_C_API_H
