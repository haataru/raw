#ifndef RAWDB_QUERY_CONNECTION_HPP
#define RAWDB_QUERY_CONNECTION_HPP

#include <memory>

#include "core/error.hpp"
#include "db/database.hpp"
#include "db/transaction.hpp"

namespace rawdb
{

class Connection
{
public:
    explicit Connection(Database& db) : db_(db) {}

    auto db() -> Database& { return db_; }
    auto txn() -> std::shared_ptr<Transaction>& { return txn_; }

    auto begin() -> Status {
        if (txn_) {
            return Status::kInvalidArgument; // Already in a transaction
        }
        txn_ = db_.txn_manager().begin(db_);
        return Status::kOk;
    }

    auto commit() -> Status {
        if (!txn_) {
            return Status::kInvalidArgument; // Not in a transaction
        }
        auto st = db_.txn_manager().commit(txn_, db_);
        txn_.reset();
        return st;
    }

    auto rollback() -> Status {
        if (!txn_) {
            return Status::kInvalidArgument;
        }
        auto st = db_.txn_manager().rollback(txn_, db_);
        txn_.reset();
        return st;
    }

private:
    Database& db_;
    std::shared_ptr<Transaction> txn_;
};

} // namespace rawdb

#endif // RAWDB_QUERY_CONNECTION_HPP
