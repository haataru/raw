#include "buffer/flush_handler.hpp"

#include <chrono>

#include "core/config.hpp"

namespace rawdb
{

FlushHandler::FlushHandler(std::deque<Table> &tables, std::shared_mutex &tables_mtx)
    : tables_(tables), tables_mtx_(tables_mtx)
{
    running_.store(true);
    thread_ = std::thread(&FlushHandler::flush_loop, this);
}

FlushHandler::~FlushHandler()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FlushHandler::signal()
{
    flush_requested_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void FlushHandler::flush_all()
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint64_t my_gen = flush_gen_.load();
    flush_requested_.store(true, std::memory_order_release);
    lock.unlock();
    cv_.notify_one();

    lock.lock();
    cv_.wait(lock, [this, my_gen] { return flush_gen_.load() > my_gen || shutdown_; });
}

void FlushHandler::flush_loop()
{
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config::kSyncIntervalMs), [this] {
            return flush_requested_ || shutdown_;
        });

        if (shutdown_) {
            break;
        }

        bool need_flush = flush_requested_.exchange(false, std::memory_order_acq_rel);
        lock.unlock();

        if (need_flush) {
            do_flush();

            lock.lock();
            flush_gen_.fetch_add(1, std::memory_order_release);
            lock.unlock();
            cv_.notify_all();
        }
    }

    running_.store(false);
}

void FlushHandler::do_flush()
{
    std::shared_lock table_lock(tables_mtx_);
    for (auto &tbl : tables_) {
        tbl.flush_pending();
        tbl.sync();
    }
}

} // namespace rawdb
