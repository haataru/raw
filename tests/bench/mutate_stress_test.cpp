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

int main()
{
    auto db_path = fs::temp_directory_path() / "rawdb_backup_stress";
    std::error_code ec;
    fs::remove_all(db_path, ec);
    fs::remove_all(db_path.string() + "_backup", ec);

    std::cout << "=== HARSH BACKUP STRESS TEST ===" << std::endl;
    std::cout << "Initializing database at " << db_path << std::endl;

    Database db;
    if (db.open(db_path) != Status::kOk) {
        std::cerr << "Failed to open DB\n";
        return 1;
    }

    {
        Connection conn(db);
        Executor exec(conn);
        exec.execute("CREATE TABLE stress (id INT, value INT, tag INT)");
        // Setup aggressive auto-backups
        exec.execute("SET BACKUP INTERVAL TO 1");
        exec.execute("SET BACKUP RETENTION TO 2");
    }

    constexpr int kInserters = 10;
    constexpr int kSelectors = 5;
    constexpr int kUpdaters = 5;
    constexpr int kDeleters = 2;
    constexpr int kDurationSec = 15;

    std::atomic<bool> stop{false};
    std::atomic<int64_t> total_inserts{0};
    std::atomic<int64_t> total_selects{0};
    std::atomic<int64_t> total_updates{0};
    std::atomic<int64_t> total_deletes{0};
    std::atomic<int64_t> errors{0};

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
                if (exec.execute(ss.str())) {
                    total_inserts.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            conn.commit();
        }
    };

    auto updater_worker = [&](int thread_id) {
        std::mt19937 rng(std::random_device{}() + thread_id);
        std::uniform_int_distribution<int> id_dist(0, kInserters - 1);
        Connection conn(db);
        Executor exec(conn);

        while (!stop.load(std::memory_order_relaxed)) {
            conn.begin();
            std::stringstream ss;
            ss << "UPDATE stress SET value = " << rng() % 1000 << " WHERE id = " << id_dist(rng);
            if (exec.execute(ss.str())) {
                total_updates.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            conn.commit();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    auto deleter_worker = [&](int thread_id) {
        std::mt19937 rng(std::random_device{}() + thread_id);
        std::uniform_int_distribution<int> id_dist(0, kInserters - 1);
        Connection conn(db);
        Executor exec(conn);

        while (!stop.load(std::memory_order_relaxed)) {
            conn.begin();
            std::stringstream ss;
            ss << "DELETE FROM stress WHERE id = " << id_dist(rng);
            if (exec.execute(ss.str())) {
                total_deletes.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            conn.commit();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    auto selector_worker = [&](int thread_id) {
        Connection conn(db);
        Executor exec(conn);

        while (!stop.load(std::memory_order_relaxed)) {
            conn.begin();
            auto res = exec.execute("SELECT id FROM stress");
            if (res) {
                total_selects.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            conn.commit();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200)); // Sleep slightly to prevent pure starvation
        }
    };

    auto backup_manual_worker = [&]() {
        // Occasionally trigger manual backups to conflict with auto backups and checkpointing!
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            std::cout << "[Backup Thread] Starting manual backup...\n";
            auto b_path =
                db_path.string() + "_backup_manual_" +
                std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            if (db.start_backup(b_path) == Status::kOk) {
                std::cout << "[Backup Thread] Manual backup SUCCESS.\n";
                fs::remove_all(b_path, ec); // clean up
            }
            else {
                std::cout << "[Backup Thread] Manual backup skipped or failed (likely already in "
                             "progress).\n";
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kInserters; ++i)
        threads.emplace_back(inserter_worker, i);
    for (int i = 0; i < kUpdaters; ++i)
        threads.emplace_back(updater_worker, i);
    for (int i = 0; i < kDeleters; ++i)
        threads.emplace_back(deleter_worker, i);
    for (int i = 0; i < kSelectors; ++i)
        threads.emplace_back(selector_worker, i);
    threads.emplace_back(backup_manual_worker);

    std::cout << "Started " << kInserters << " inserters, " << kUpdaters << " updaters, "
              << kDeleters << " deleters, " << kSelectors << " selectors." << std::endl;
    std::cout << "Auto backup interval is set to 1 second." << std::endl;
    std::cout << "Running for " << kDurationSec << " seconds..." << std::endl;

    for (int i = 0; i < kDurationSec; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Time elapsed: " << i + 1 << "s / " << kDurationSec
                  << "s (Inserts: " << total_inserts.load() << ", Updates: " << total_updates.load()
                  << ", Deletes: " << total_deletes.load() << ", Selects: " << total_selects.load()
                  << ")" << std::endl;
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto &t : threads)
        t.join();

    std::cout << "\n=== STRESS TEST RESULTS ===\n";
    std::cout << "Total Inserts (successful): " << total_inserts.load() << "\n";
    std::cout << "Total Updates (successful): " << total_updates.load() << "\n";
    std::cout << "Total Deletes (successful): " << total_deletes.load() << "\n";
    std::cout << "Total Selects (successful): " << total_selects.load() << "\n";
    std::cout << "Total Errors: " << errors.load() << "\n";

    // Check if backups were generated
    int auto_backups_found = 0;
    for (const auto &entry : fs::directory_iterator(db_path.parent_path())) {
        if (entry.is_directory() &&
            entry.path().string().find("rawdb_backup_stress_backup_") != std::string::npos &&
            entry.path().string().find("manual") == std::string::npos) {
            auto_backups_found++;
        }
    }
    std::cout << "Auto backups currently retained on disk: " << auto_backups_found << "\n";

    db.close();
    fs::remove_all(db_path, ec);

    return errors.load() == 0 ? 0 : 1;
}
