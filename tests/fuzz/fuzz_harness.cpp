#include "query/parser.hpp"
#include <cstdint>
#include <cstddef>
#include <string>

using namespace rawdb;

#include <iostream>

int main() {
    std::string sql((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    if (sql.empty()) return 0;
    
    try {
        Parser parser;
        auto ast = parser.parse(sql);
    } catch (...) {
    }
    
    return 0;
}
