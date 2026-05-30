#ifndef RAWDB_BUFFER_FLUSH_HANDLER_HPP
#define RAWDB_BUFFER_FLUSH_HANDLER_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "storage/table.hpp"

namespace rawdb
{

class FlushHandler
{
public:
    FlushHandler(std::deque<Table> &tables, std::shared_mutex &tables_mtx);

    ~FlushHandler();

    FlushHandler(const FlushHandler &) = delete;
    auto operator=(const FlushHandler &) -> FlushHandler & = delete;

    void signal();

    void flush_all();

    [[nodiscard]] auto is_running() const -> bool { return running_.load(); }

private:
    std::deque<Table> &tables_;
    std::shared_mutex &tables_mtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
    std::atomic<bool> flush_requested_{false};
    std::atomic<uint64_t> flush_gen_{0};

    void flush_loop();
    void do_flush();
};

} // namespace rawdb

#endif // RAWDB_BUFFER_FLUSH_HANDLER_HPP
