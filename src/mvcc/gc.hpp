#ifndef RAWDB_MVCC_GC_HPP
#define RAWDB_MVCC_GC_HPP

#include <atomic>
#include <chrono>
#include <deque>
#include <shared_mutex>
#include <thread>

#include "core/config.hpp"
#include "mvcc/version_index.hpp"

namespace rawdb
{

class Table;

class GarbageCollector
{
public:
    explicit GarbageCollector(std::deque<Table> &tables,
                              std::shared_mutex &tables_mtx,
                              TimestampAllocator &timestamps,
                              GlobalWatermarks &watermarks)
        : tables_(tables), tables_mtx_(tables_mtx), timestamps_(timestamps), watermarks_(watermarks)
    {}

    ~GarbageCollector();

    GarbageCollector(const GarbageCollector &) = delete;
    auto operator=(const GarbageCollector &) -> GarbageCollector & = delete;

    void start();
    void stop();

    [[nodiscard]] auto is_running() const -> bool { return running_.load(); }

private:
    std::deque<Table> &tables_;
    std::shared_mutex &tables_mtx_;
    TimestampAllocator &timestamps_;
    GlobalWatermarks &watermarks_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    static constexpr size_t kBudget = config::kGcMaxBytesPerSec;

    void gc_loop();
    void prune_tables(Timestamp cutoff);
    auto compute_cutoff() -> Timestamp;
};

} // namespace rawdb

#endif // RAWDB_MVCC_GC_HPP
