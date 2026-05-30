#ifndef RAWDB_MVCC_SNAPSHOT_HPP
#define RAWDB_MVCC_SNAPSHOT_HPP

#include <atomic>
#include <cstdint>

#include "core/types.hpp"

namespace rawdb
{

class GlobalWatermarks
{
public:
    [[nodiscard]] auto min_active_read_ts() const -> Timestamp
    {
        return min_active_read_ts_.load(std::memory_order_acquire);
    }

private:
    std::atomic<Timestamp> min_active_read_ts_{0};
};

} // namespace rawdb

#endif // RAWDB_MVCC_SNAPSHOT_HPP
