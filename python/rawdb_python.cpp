#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>

#include "db/database.hpp"
#include "query/connection.hpp"
#include "query/executor.hpp"

namespace py = pybind11;

namespace rawdb {

// Helper to convert QueryResult to a list of dicts or list of lists
py::list convert_query_result(const QueryResult& result) {
    py::list rows;
    for (const auto& row : result.rows) {
        py::dict row_dict;
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            const auto& col_name = result.column_names[i];
            const auto& str_val = row[i];
            const auto type = result.column_types[i];
            
            // Basic type conversion
            if (type == ColumnType::kInt32 || type == ColumnType::kInt64) {
                row_dict[py::str(col_name)] = std::stoll(str_val);
            } else if (type == ColumnType::kFloat64) {
                row_dict[py::str(col_name)] = std::stod(str_val);
            } else if (type == ColumnType::kBool) {
                row_dict[py::str(col_name)] = (str_val == "true" || str_val == "1");
            } else {
                row_dict[py::str(col_name)] = str_val;
            }
        }
        rows.append(row_dict);
    }
    return rows;
}

PYBIND11_MODULE(rawdb, m) {
    m.doc() = "rawDB Python Bindings"; // module docstring

    py::class_<Database>(m, "Database")
        .def(py::init<>())
        .def("open", [](Database& db, const std::string& path) {
            auto st = db.open(path);
            if (st != Status::kOk) {
                throw std::runtime_error("Failed to open database");
            }
        })
        .def("close", &Database::close);

    py::class_<Connection>(m, "Connection")
        .def(py::init<Database&>())
        .def("begin", [](Connection& conn) {
            auto st = conn.begin();
            if (st != Status::kOk) {
                throw std::runtime_error("Failed to begin transaction");
            }
        })
        .def("commit", [](Connection& conn) {
            auto st = conn.commit();
            if (st != Status::kOk) {
                throw std::runtime_error("Failed to commit transaction");
            }
        })
        .def("rollback", [](Connection& conn) {
            auto st = conn.rollback();
            if (st != Status::kOk) {
                throw std::runtime_error("Failed to rollback transaction");
            }
        });

    py::class_<Executor>(m, "Executor")
        .def(py::init<Connection&>())
        .def("execute", [](Executor& exec, const std::string& sql) {
            auto res = exec.execute(sql);
            if (!res) {
                throw std::runtime_error("Query execution failed");
            }
            return convert_query_result(*res);
        });
}

} // namespace rawdb
