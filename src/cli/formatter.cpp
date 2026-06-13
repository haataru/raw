#include "cli/formatter.hpp"
#include <iostream>
#include <vector>
#include <algorithm>

namespace rawdb::cli
{
void Formatter::print_table(const QueryResult& result)
{
    if (result.column_names.empty()) return;

    std::vector<size_t> widths;
    for (const auto& name : result.column_names) {
        widths.push_back(name.size());
    }

    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (row[i].size() > widths[i]) {
                widths[i] = row[i].size();
            }
        }
    }

    auto print_separator = [&]() {
        std::cout << "+";
        for (size_t w : widths) {
            std::cout << std::string(w + 2, '-') << "+";
        }
        std::cout << "\n";
    };

    print_separator();

    std::cout << "|";
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        std::cout << " " << result.column_names[i] 
                  << std::string(widths[i] - result.column_names[i].size(), ' ') << " |";
    }
    std::cout << "\n";

    print_separator();

    for (const auto& row : result.rows) {
        std::cout << "|";
        for (size_t i = 0; i < row.size(); ++i) {
            std::cout << " " << row[i] 
                      << std::string(widths[i] - row[i].size(), ' ') << " |";
        }
        std::cout << "\n";
    }

    print_separator();
    std::cout << "(" << result.rows.size() << " row" << (result.rows.size() == 1 ? "" : "s") << ")\n";
}
}
