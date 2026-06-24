#include <filesystem>
#include <iostream>

#include "cli/repl.hpp"
#include "db/database.hpp"

int main(int argc, char **argv)
{
    std::filesystem::path db_path;
    if (argc > 1) {
        db_path = argv[1];
    }
    else {
        db_path = "rawdb_data";
        std::cout << "No database path provided. Using default: " << db_path << "\n";
    }

    rawdb::Database db;
    if (db_path.string() == ":memory:") {
        std::cout << "In-memory mode is not fully supported by CLI yet, storing on disk.\n";
        db_path = "rawdb_memory_fallback";
    }

    auto st = db.open(db_path);
    if (st != rawdb::Status::kOk) {
        std::cerr << "Failed to open database at " << db_path << ": " << st << "\n";
        return 1;
    }

    rawdb::cli::Repl repl(db);
    repl.run();

    db.close();
    return 0;
}
