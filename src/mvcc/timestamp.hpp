#ifndef RAWDB_MVCC_TIMESTAMP_HPP
#define RAWDB_MVCC_TIMESTAMP_HPP

#include <atomic>
#include <cstdint>

#include "core/types.hpp"

namespace rawdb
{

class TimestampAllocator
{
public:
    [[nodiscard]] auto allocate_ts() -> Timestamp
    {
        return next_ts_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    [[nodiscard]] auto current() const -> Timestamp
    {
        return next_ts_.load(std::memory_order_acquire);
    }

    // For restoring on restart.
    void set_next(Timestamp ts) { next_ts_.store(ts - 1, std::memory_order_release); }

private:
    std::atomic<Timestamp> next_ts_{0};
};

} // namespace rawdb

#endif // RAWDB_MVCC_TIMESTAMP_HPP
