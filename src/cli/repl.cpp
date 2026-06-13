#include "cli/repl.hpp"
#include "cli/formatter.hpp"
#include <iostream>
#include <sstream>

namespace rawdb::cli
{

Repl::Repl(Database& db) : db_(db), conn_(db), executor_(conn_)
{
}

void Repl::run()
{
    std::string query_buffer;
    std::string line;

    std::cout << "rawDB shell\n";
    std::cout << "Type \".help\" for hints.\n";

    while (true) {
        if (query_buffer.empty()) {
            std::cout << "rawdb> ";
        } else {
            std::cout << "  ...> ";
        }

        if (!std::getline(std::cin, line)) {
            break; // EOF
        }

        // Trim
        auto start = line.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            line = line.substr(start);
        } else {
            line = "";
        }

        auto end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            line = line.substr(0, end + 1);
        }

        if (line.empty() && query_buffer.empty()) {
            continue;
        }

        if (query_buffer.empty()) {
            if (line[0] == '.') {
                if (line == ".exit" || line == ".quit") break;
                handle_meta_command(line);
                continue;
            }
            // Friendly aliases for common commands without dot
            if (line == "exit" || line == "quit") break;
            if (line == "help") {
                handle_meta_command(".help");
                continue;
            }
        }

        query_buffer += line + " ";
        
        if (query_buffer.find(';') != std::string::npos) {
            execute_query(query_buffer);
            query_buffer.clear();
        }
    }
}

void Repl::handle_meta_command(const std::string& cmd)
{
    if (cmd == ".tables") {
        std::cout << "Tables in database:\n";
        for (size_t i = 0; i < db_.table_count(); ++i) {
            std::cout << "  " << db_.table(static_cast<TableId>(i)).name() << "\n";
        }
    } else if (cmd.starts_with(".schema")) {
        std::istringstream iss(cmd);
        std::string cmd_name, table_name;
        iss >> cmd_name >> table_name;
        if (table_name.empty()) {
            std::cout << "Usage: .schema <table_name>\n";
            return;
        }
        
        bool found = false;
        for (size_t i = 0; i < db_.table_count(); ++i) {
            auto& tbl = db_.table(static_cast<TableId>(i));
            if (tbl.name() == table_name) {
                found = true;
                const auto& schema = tbl.schema();
                std::cout << "CREATE TABLE " << table_name << " (\n";
                for (size_t c = 0; c < schema.columns.size(); ++c) {
                    std::cout << "  " << schema.names[c] << " ";
                    switch(schema.columns[c]) {
                        case ColumnType::kInt32: std::cout << "INT32"; break;
                        case ColumnType::kInt64: std::cout << "INT64"; break;
                        case ColumnType::kFloat64: std::cout << "FLOAT64"; break;
                        case ColumnType::kBool: std::cout << "BOOL"; break;
                        case ColumnType::kVarChar: std::cout << "VARCHAR"; break;
                    }
                    if (c + 1 < schema.columns.size()) std::cout << ",";
                    std::cout << "\n";
                }
                std::cout << ");\n";
                break;
            }
        }
        if (!found) {
            std::cout << "Error: Table '" << table_name << "' not found.\n";
        }
    } else if (cmd == ".help") {
        std::cout << "Available meta-commands:\n";
        std::cout << "  .exit, .quit     Exit the shell\n";
        std::cout << "  .tables          List all tables\n";
        std::cout << "  .schema <table>  Show table schema\n";
        std::cout << "  .help            Show this help message\n";
    } else {
        std::cout << "Unknown command: " << cmd << "\n";
    }
}

void Repl::execute_query(const std::string& query)
{
    auto result = executor_.execute(query);
    if (!result.has_value()) {
        std::cout << "Error: " << result.error() << "\n";
    } else {
        if (!result.value().column_names.empty()) {
            Formatter::print_table(result.value());
        } else {
            std::cout << "Query executed successfully.\n";
        }
    }
}

}
