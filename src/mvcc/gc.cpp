#include "mvcc/gc.hpp"

#include <chrono>
#include <thread>

#include "core/config.hpp"
#include "storage/table.hpp"

namespace rawdb
{

GarbageCollector::~GarbageCollector() { stop(); }

void GarbageCollector::start()
{
    if (running_.exchange(true))
        return;
    thread_ = std::thread(&GarbageCollector::gc_loop, this);
}

void GarbageCollector::stop()
{
    if (!running_.exchange(false))
        return;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GarbageCollector::gc_loop()
{
    auto last_run = std::chrono::steady_clock::now();

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config::kGcIntervalMs));

        if (!running_.load())
            break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_run).count();
        if (elapsed < static_cast<decltype(elapsed)>(config::kGcIntervalMs)) {
            continue;
        }
        last_run = now;

        Timestamp cutoff = compute_cutoff();
        if (cutoff == 0)
            continue;
        prune_tables(cutoff);
    }
}

void GarbageCollector::prune_tables(Timestamp cutoff)
{
    if (cutoff == 0)
        return;
    std::shared_lock lock(tables_mtx_);
    size_t budget_remaining = kBudget;
    for (auto &tbl : tables_) {
        if (tbl.version_index_size() == 0)
            continue;
        tbl.flush_pending();
        size_t removed = tbl.prune_version_index(cutoff);
        size_t bytes = removed * sizeof(IndexEntry);
        if (bytes >= budget_remaining)
            break;
        budget_remaining -= bytes;
    }
}

auto GarbageCollector::compute_cutoff() -> Timestamp
{
    Timestamp alloc_ts = timestamps_.allocate_ts();
    if (alloc_ts <= 1)
        return 0;
    Timestamp min_read = watermarks_.min_active_read_ts();
    if (min_read > 0 && min_read < alloc_ts - 1) {
        return min_read;
    }
    return alloc_ts - 1;
}

} // namespace rawdb
