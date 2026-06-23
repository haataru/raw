#include "db/transaction.hpp"

#include "db/database.hpp"

namespace rawdb
{

auto TransactionManager::begin(Database& db) -> std::shared_ptr<Transaction>
{
    auto txn = std::make_shared<Transaction>();
    txn->tx_id = next_tx_id_.fetch_add(1, std::memory_order_relaxed);
    txn->read_ts = ts_alloc_.current();
    auto lsn_res = db.wal().append_begin(txn->tx_id);
    if (!lsn_res) return nullptr;
    txn->start_lsn = *lsn_res;
    
    std::lock_guard lock(mtx_);
    active_txns_.insert({txn->tx_id, txn});
    return txn;
}

auto TransactionManager::commit(std::shared_ptr<Transaction> txn, Database& db) -> Status
{
    if (!txn || txn->state != TransactionState::kActive) {
        return Status::kInvalidArgument;
    }

    Timestamp commit_ts = ts_alloc_.allocate_ts();
    
    if (auto s = db.wal().append_commit(txn->tx_id); !s) {
        return s.error();
    }
    db.wal().flush(); // Synchronous commit
    
    for (const auto& [table_id, rows] : txn->write_set) {
        auto& table = db.table(table_id);
        table.commit_rows(rows, txn->tx_id, commit_ts);
    }
    
    txn->state = TransactionState::kCommitted;
    
    std::lock_guard lock(mtx_);
    active_txns_.erase(txn->tx_id);
    return Status::kOk;
}

auto TransactionManager::rollback(std::shared_ptr<Transaction> txn, Database& db) -> Status
{
    if (!txn || txn->state != TransactionState::kActive) {
        return Status::kInvalidArgument;
    }

    // Rollback means the rows should be invisible forever.
    // We set their ts to 0 (which is less than any read_ts, wait, 0 is visible to everyone! No, 0 is a tombstone timestamp? 
    // Wait, if it's 0, it's older than any snapshot, so it's visible if it's an insert!
    // We should set it to `kTxIdFlag` without any tx_id, meaning it's aborted, or a special tombstone value.
    // Or we can just set it to `std::numeric_limits<Timestamp>::max()` so it's from the infinite future and never visible!
    // Yes! Infinite future.
    Timestamp aborted_ts = static_cast<Timestamp>(-1);
    
    if (auto s = db.wal().append_rollback(txn->tx_id); !s) {
        return s.error();
    }
    // Rollback doesn't strictly need synchronous flush because if we crash, it aborts anyway.
    
    for (const auto& [table_id, rows] : txn->write_set) {
        auto& table = db.table(table_id);
        table.commit_rows(rows, txn->tx_id, aborted_ts);
    }

    txn->state = TransactionState::kAborted;
    
    std::lock_guard lock(mtx_);
    active_txns_.erase(txn->tx_id);
    return Status::kOk;
}

auto TransactionManager::oldest_active_lsn() -> Lsn
{
    std::lock_guard lock(mtx_);
    if (active_txns_.empty()) {
        return static_cast<Lsn>(-1); // Max LSN
    }
    Lsn oldest = static_cast<Lsn>(-1);
    for (const auto& [tx_id, txn] : active_txns_) {
        if (txn->start_lsn > 0 && txn->start_lsn < oldest) {
            oldest = txn->start_lsn;
        }
    }
    return oldest;
}

} // namespace rawdb
