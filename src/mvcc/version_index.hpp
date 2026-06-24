#ifndef RAWDB_MVCC_VERSION_INDEX_HPP
#define RAWDB_MVCC_VERSION_INDEX_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/error.hpp"
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

    void set_next(Timestamp ts) { next_ts_.store(ts - 1, std::memory_order_release); }

private:
    std::atomic<Timestamp> next_ts_{0};
};

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

struct IndexEntry
{
    RowId row_id;
    Timestamp ts;
    uint64_t offset;
    uint16_t row_size;
};

class VersionIndex
{
public:
    void insert_bulk(const IndexEntry *entries, size_t count);

    void ensure_sorted() const;

    [[nodiscard]] auto search(RowId row_id,
                              Timestamp max_ts,
                              TxId current_tx_id = kInvalidTxId) const -> StatusOr<uint64_t>;

    void commit_rows(const std::vector<RowId> &row_ids, TxId tx_id, Timestamp commit_ts);

    [[nodiscard]] auto size() const -> size_t { return entries_.size(); }

    /// For restoring TimestampAllocator.
    [[nodiscard]] auto max_timestamp() const -> Timestamp;

    auto prune(Timestamp cutoff_ts) -> size_t;

    void serialize(std::vector<std::byte> &out) const;

    auto deserialize(const std::byte *data, size_t size) -> StatusOr<size_t>;

    VersionIndex() = default;
    VersionIndex(VersionIndex &&other) noexcept : entries_(std::move(other.entries_))
    {
        dirty_.store(other.dirty_.exchange(false, std::memory_order_relaxed));
        sorting_.store(other.sorting_.exchange(false, std::memory_order_relaxed));
    }
    auto operator=(VersionIndex &&other) noexcept -> VersionIndex &
    {
        if (this != &other) {
            entries_ = std::move(other.entries_);
            dirty_.store(other.dirty_.exchange(false, std::memory_order_relaxed));
            sorting_.store(other.sorting_.exchange(false, std::memory_order_relaxed));
        }
        return *this;
    }

    static constexpr uint64_t kNotFound = static_cast<uint64_t>(-1);

private:
    mutable std::vector<IndexEntry> entries_;
    mutable std::atomic<bool> dirty_{false};
    mutable std::atomic<bool> sorting_{false};
};

} // namespace rawdb

#endif // RAWDB_MVCC_VERSION_INDEX_HPP
