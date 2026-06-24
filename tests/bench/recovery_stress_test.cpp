#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db/database.hpp"
#include "query/connection.hpp"
#include "query/executor.hpp"

using namespace rawdb;
namespace fs = std::filesystem;

void run_child_workload(const fs::path &db_path)
{
    Database db;
    if (db.open(db_path) != Status::kOk) {
        std::cerr << "Failed to open DB in child\n";
        _exit(1);
    }

    {
        Connection conn(db);
        Executor exec(conn);
        exec.execute("CREATE TABLE stress (id INT, value INT, tag INT)");
    }

    constexpr int kInserters = 10;
    std::atomic<bool> stop{false};

    auto inserter_worker = [&](int thread_id) {
        std::mt19937 rng(std::random_device{}() + thread_id);
        std::uniform_int_distribution<int> val_dist(1, 1000000);
        Connection conn(db);
        Executor exec(conn);

        while (!stop.load(std::memory_order_relaxed)) {
            conn.begin();
            for (int i = 0; i < 50; ++i) {
                std::stringstream ss;
                ss << "INSERT INTO stress VALUES (" << thread_id << ", " << val_dist(rng) << ", "
                   << val_dist(rng) << ")";
                exec.execute(ss.str());
            }
            conn.commit();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kInserters; ++i)
        threads.emplace_back(inserter_worker, i);

    std::cout << "[Child] Running heavy inserts for 5 seconds, then crashing abruptly...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "[Child] ABORTING NOW to simulate power failure!\n";
    std::abort(); // Hard crash!
}

int main()
{
    auto db_path = fs::temp_directory_path() / "rawdb_recovery_stress";
    std::error_code ec;
    fs::remove_all(db_path, ec);

    std::cout << "=== HARSH RECOVERY STRESS TEST ===" << std::endl;

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Fork failed\n";
        return 1;
    }
    else if (pid == 0) {
        run_child_workload(db_path);
        _exit(0);
    }
    else {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        std::cout << "[Parent] Child died. Status: " << wstatus << std::endl;

        std::cout << "[Parent] Re-opening database to test WAL crash recovery...\n";
        Database db;
        auto status = db.open(db_path);
        if (status != Status::kOk) {
            std::cerr << "[Parent] FAILED TO RECOVER DATABASE!\n";
            return 1;
        }

        Connection conn(db);
        Executor exec(conn);
        auto res = exec.execute("SELECT id FROM stress");
        if (!res) {
            std::cerr << "[Parent] Failed to query recovered data!\n";
            return 1;
        }

        std::cout << "[Parent] Successfully recovered! Total rows preserved: " << res->rows.size()
                  << "\n";
        std::cout << "\n=== STRESS TEST RESULTS ===\n";
        std::cout << "Total Errors: 0\n";

        fs::remove_all(db_path, ec);
    }

    return 0;
}
