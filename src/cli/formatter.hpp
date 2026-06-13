#pragma once
#include "query/executor.hpp"

namespace rawdb::cli
{
class Formatter
{
public:
    static void print_table(const QueryResult& result);
};
}
