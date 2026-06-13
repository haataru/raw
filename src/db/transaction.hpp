#ifndef RAWDB_DB_TRANSACTION_HPP
#define RAWDB_DB_TRANSACTION_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "mvcc/version_index.hpp"

namespace rawdb
{

class Database;

enum class TransactionState {
    kActive,
    kCommitted,
    kAborted
};

struct Transaction
{
    TxId tx_id;
    Timestamp read_ts;
    TransactionState state{TransactionState::kActive};
    
    // Rows modified by this transaction. TableId -> vector<RowId>
    std::unordered_map<TableId, std::vector<RowId>> write_set;
};

class TransactionManager
{
public:
    explicit TransactionManager(TimestampAllocator& ts_alloc) : ts_alloc_(ts_alloc) {}

    auto begin() -> std::shared_ptr<Transaction>;
    
    auto commit(std::shared_ptr<Transaction> txn, Database& db) -> Status;
    
    auto rollback(std::shared_ptr<Transaction> txn, Database& db) -> Status;

private:
    std::atomic<TxId> next_tx_id_{1};
    std::mutex mtx_;
    std::unordered_map<TxId, std::shared_ptr<Transaction>> active_txns_;
    TimestampAllocator& ts_alloc_;
};

} // namespace rawdb

#endif // RAWDB_DB_TRANSACTION_HPP
