#include "mvcc/version_index.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace rawdb
{

void VersionIndex::insert_bulk(const IndexEntry *entries, size_t count)
{
    size_t old_size = entries_.size();
    entries_.resize(old_size + count);
    for (size_t i = 0; i < count; ++i) {
        entries_[old_size + i] = entries[i];
    }
    dirty_.store(true, std::memory_order_release);
}

void VersionIndex::ensure_sorted() const
{
    if (!dirty_.load(std::memory_order_acquire))
        return;

    if (!sorting_.exchange(true, std::memory_order_acq_rel)) {
        std::sort(entries_.begin(), entries_.end(), [](const IndexEntry &a, const IndexEntry &b) {
            if (a.row_id != b.row_id)
                return a.row_id < b.row_id;
            return a.ts > b.ts;
        });
        sorting_.store(false, std::memory_order_release);
        dirty_.store(false, std::memory_order_release);
        return;
    }

    while (sorting_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

auto VersionIndex::search(RowId row_id, Timestamp max_ts, TxId current_tx_id) const -> StatusOr<uint64_t>
{
    ensure_sorted();

    if (entries_.empty()) {
        return std::unexpected(Status::kNotFound);
    }

    auto it = std::lower_bound(entries_.begin(),
                               entries_.end(),
                               row_id,
                               [](const IndexEntry &e, RowId rid) { return e.row_id < rid; });

    uint64_t best_offset = kNotFound;
    uint64_t best_ts = 0;
    bool found = false;

    for (; it != entries_.end() && it->row_id == row_id; ++it) {
        uint64_t raw_ts = reinterpret_cast<const std::atomic<uint64_t>&>(it->ts).load(std::memory_order_acquire);
        
        if ((raw_ts & kTxIdFlag) == 0) {
            // Committed row
            if (raw_ts <= max_ts && raw_ts >= best_ts) {
                best_ts = raw_ts;
                best_offset = it->offset;
                found = true;
            }
        } else {
            // Uncommitted row (active transaction)
            TxId row_txid = raw_ts & ~kTxIdFlag;
            if (row_txid == current_tx_id && current_tx_id != kInvalidTxId) {
                return it->offset;
            }
        }
    }

    if (found) return best_offset;
    return std::unexpected(Status::kNotFound);
}

void VersionIndex::commit_rows(const std::vector<RowId>& row_ids, TxId tx_id, Timestamp commit_ts)
{
    ensure_sorted();
    uint64_t target_ts = tx_id | kTxIdFlag;

    for (RowId rid : row_ids) {
        auto it = std::lower_bound(entries_.begin(),
                                   entries_.end(),
                                   rid,
                                   [](const IndexEntry &e, RowId id) { return e.row_id < id; });
                                   
        for (; it != entries_.end() && it->row_id == rid; ++it) {
            auto& atomic_ts = reinterpret_cast<std::atomic<uint64_t>&>(it->ts);
            uint64_t raw_ts = atomic_ts.load(std::memory_order_acquire);
            if (raw_ts == target_ts) {
                atomic_ts.store(commit_ts, std::memory_order_release);
                break;
            }
        }
    }
}

void VersionIndex::serialize(std::vector<std::byte> &out) const
{
    ensure_sorted();
    auto put = [&](const void *d, size_t n) {
        auto *p = static_cast<const std::byte *>(d);
        out.insert(out.end(), p, p + n);
    };
    uint32_t magic = 0x56495844; // "VIDX"
    uint32_t ver = 1;
    uint64_t n = entries_.size();
    put(&magic, sizeof(magic));
    put(&ver, sizeof(ver));
    put(&n, sizeof(n));
    for (auto &e : entries_) {
        put(&e.row_id, sizeof(e.row_id));
        put(&e.ts, sizeof(e.ts));
        put(&e.offset, sizeof(e.offset));
        put(&e.row_size, sizeof(e.row_size));
    }
}

auto VersionIndex::deserialize(const std::byte *data, size_t size) -> StatusOr<size_t>
{
    size_t pos = 0;
    auto get = [&](void *d, size_t n) -> bool {
        if (pos + n > size)
            return false;
        std::memcpy(d, data + pos, n);
        pos += n;
        return true;
    };
    uint32_t magic, ver;
    uint64_t n;
    if (!get(&magic, sizeof(magic)) || magic != 0x56495844) {
        return std::unexpected(Status::kCorruptedData);
    }
    if (!get(&ver, sizeof(ver)) || ver != 1) {
        return std::unexpected(Status::kCorruptedData);
    }
    if (!get(&n, sizeof(n))) {
        return std::unexpected(Status::kCorruptedData);
    }
    entries_.resize(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        if (!get(&entries_[static_cast<size_t>(i)].row_id, sizeof(RowId)) ||
            !get(&entries_[static_cast<size_t>(i)].ts, sizeof(Timestamp)) ||
            !get(&entries_[static_cast<size_t>(i)].offset, sizeof(uint64_t)) ||
            !get(&entries_[static_cast<size_t>(i)].row_size, sizeof(uint16_t))) {
            return std::unexpected(Status::kCorruptedData);
        }
    }
    dirty_.store(false, std::memory_order_release);
    return pos;
}

auto VersionIndex::max_timestamp() const -> Timestamp
{
    ensure_sorted();
    Timestamp mt = 0;
    for (auto &e : entries_) {
        if (e.ts > mt)
            mt = e.ts;
    }
    return mt;
}

auto VersionIndex::prune(Timestamp cutoff_ts) -> size_t
{
    ensure_sorted();
    if (entries_.empty())
        return 0;

    size_t write = 0;
    size_t i = 0;
    while (i < entries_.size()) {
        RowId current_row = entries_[i].row_id;

        size_t group_end = i;
        while (group_end < entries_.size() && entries_[group_end].row_id == current_row) {
            ++group_end;
        }

        size_t keeper_pos = group_end;
        for (size_t j = i; j < group_end; ++j) {
            if (entries_[j].ts <= cutoff_ts) {
                keeper_pos = j;
                break;
            }
        }

        size_t keep_count;
        if (keeper_pos == group_end) {
            keep_count = group_end - i;
        }
        else if (entries_[keeper_pos].offset == kNotFound) {
            keep_count = 0; // tombstone → row is fully deleted
        }
        else {
            keep_count = keeper_pos - i + 1;
        }

        if (write != i && keep_count > 0) {
            std::memmove(&entries_[write], &entries_[i], keep_count * sizeof(IndexEntry));
        }
        write += keep_count;
        i = group_end;
    }

    size_t removed = entries_.size() - write;
    entries_.resize(write);
    return removed;
}

} // namespace rawdb
