#pragma once
#include "db/database.hpp"
#include "query/executor.hpp"
#include <string>

namespace rawdb::cli
{
class Repl
{
public:
    Repl(Database& db);
    void run();

private:
    void handle_meta_command(const std::string& cmd);
    void execute_query(const std::string& query);
    
    Database& db_;
    Connection conn_;
    Executor executor_;
};
}
