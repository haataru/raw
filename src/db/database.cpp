#include "db/database.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "index/btree.hpp"
#include "query/executor.hpp"

namespace rawdb
{

// ── Schema serialization ──

static auto schema_to_bytes(const Schema &schema) -> std::vector<std::byte>
{
    std::vector<std::byte> buf;
    auto put = [&](const void *d, size_t n) {
        auto *p = static_cast<const std::byte *>(d);
        buf.insert(buf.end(), p, p + n);
    };
    uint32_t n = static_cast<uint32_t>(schema.columns.size());
    put(&n, sizeof(n));
    for (size_t i = 0; i < n; ++i) {
        uint32_t len = static_cast<uint32_t>(schema.names[i].size());
        put(&len, sizeof(len));
        put(schema.names[i].data(), len);
        uint8_t t = static_cast<uint8_t>(schema.columns[i]);
        put(&t, sizeof(t));
    }
    return buf;
}

static auto schema_from_bytes(const std::byte *data, size_t size) -> StatusOr<Schema>
{
    Schema schema;
    size_t pos = 0;
    auto get = [&](void *d, size_t n) -> bool {
        if (pos + n > size)
            return false;
        std::memcpy(d, data + pos, n);
        pos += n;
        return true;
    };
    uint32_t n;
    if (!get(&n, sizeof(n)))
        return std::unexpected(Status::kCorruptedData);
    schema.columns.reserve(n);
    schema.names.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t len;
        if (!get(&len, sizeof(len)))
            return std::unexpected(Status::kCorruptedData);
        if (pos + len > size)
            return std::unexpected(Status::kCorruptedData);
        schema.names.emplace_back(reinterpret_cast<const char *>(data + pos), len);
        pos += len;
        uint8_t t;
        if (!get(&t, sizeof(t)))
            return std::unexpected(Status::kCorruptedData);
        schema.columns.push_back(static_cast<ColumnType>(t));
    }
    return schema;
}

static auto write_file(const std::filesystem::path &path,
                       const std::vector<std::byte> &data) -> Status
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return Status::kIoError;
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return f.good() ? Status::kOk : Status::kIoError;
}

static auto read_file(const std::filesystem::path &path) -> StatusOr<std::vector<std::byte>>
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return std::unexpected(Status::kIoError);
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(sz);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

Database::Database() = default;

Database::~Database() { close(); }

auto Database::open(const std::filesystem::path &path,
                    std::optional<uint64_t> recovery_target_time_ms) -> Status
{
    if (is_open_) {
        return Status::kInvalidArgument;
    }

    path_ = path;
    std::filesystem::create_directories(path_);

    {
        std::unique_lock lock(tables_mtx_);
        if (std::filesystem::is_directory(path_)) {
            for (const auto &entry : std::filesystem::directory_iterator(path_)) {
                if (entry.path().extension() == ".schema") {
                    auto name = entry.path().stem().string();
                    auto raw = read_file(entry.path());
                    if (!raw)
                        continue;
                    auto schema = schema_from_bytes(raw->data(), raw->size());
                    if (!schema)
                        continue;
                    tables_.emplace_back(name, std::move(*schema));
                    auto &tbl = tables_.back();
                    TableId tid = static_cast<TableId>(tables_.size() - 1);
                    tbl.set_fpw_callback([this, tid](PageId p, const std::byte *d, size_t s) {
                        this->fpw_callback(tid, p, -1, d, s);
                    });
                    if (tbl.open_file(path_) != Status::kOk) {
                        tables_.pop_back();
                        continue;
                    }
                    auto _ = tbl.recover();
                    (void)_;
                    auto vl = tbl.load_vindex(path_);
                    if (vl != Status::kOk) {
                        tbl.rebuild_vindex_from_pages(timestamps_);
                    }
                }
            }
        }
    }

    {
        std::unique_lock lock(tables_mtx_);
        if (std::filesystem::is_directory(path_)) {
            for (const auto &entry : std::filesystem::directory_iterator(path_)) {
                if (entry.path().extension() != ".idx")
                    continue;
                auto stem = entry.path().stem().string();
                auto underscore = stem.rfind('_');
                if (underscore == std::string::npos)
                    continue;
                auto tbl_name = stem.substr(0, underscore);
                size_t col_idx;
                try {
                    col_idx = std::stoul(stem.substr(underscore + 1));
                }
                catch (...) {
                    continue;
                }
                for (size_t i = 0; i < tables_.size(); ++i) {
                    auto &tbl = tables_[i];
                    if (tbl.name() != tbl_name)
                        continue;
                    if (col_idx >= tbl.schema().column_count())
                        continue;
                    auto tree_r = BTree::open(entry.path());
                    if (!tree_r)
                        continue;
                    IndexInfo info;
                    info.name = tbl.schema().names[col_idx] + "_idx";
                    info.column_name = tbl.schema().names[col_idx];
                    info.column_idx = col_idx;
                    info.column_type = tbl.schema().columns[col_idx];
                    int32_t cid = static_cast<int32_t>(col_idx);
                    TableId tid = static_cast<TableId>(i);
                    tree_r->set_fpw_callback(
                        [this, tid, cid](PageId p, const std::byte *d, size_t s) {
                            this->fpw_callback(tid, p, cid, d, s);
                        });

                    info.tree = std::move(*tree_r);
                    tbl.add_index(std::move(info));
                    break;
                }
            }
        }
    }

    // Restore timestamp allocator past the max seen in any vindex
    {
        std::shared_lock lock(tables_mtx_);
        Timestamp max_ts = 0;
        for (auto &tbl : tables_) {
            auto mt = tbl.version_index_max_ts();
            if (mt > max_ts)
                max_ts = mt;
        }
        if (max_ts > 0)
            timestamps_.set_next(max_ts + 1);
    }

    flush_handler_ = std::make_unique<FlushHandler>(tables_, tables_mtx_);
    {
        std::unique_lock lock(tables_mtx_);
        for (auto &tbl : tables_) {
            tbl.set_flush_handler(flush_handler_.get());
        }
    }
    txn_manager_ = std::make_unique<TransactionManager>(timestamps_);
    gc_ = std::make_unique<GarbageCollector>(tables_, tables_mtx_, timestamps_, watermarks_);
    gc_->start();

    // 1. Read checkpoint.meta if it exists
    Lsn checkpoint_lsn = 0;
    auto meta_path = path_ / "checkpoint.meta";
    if (std::filesystem::exists(meta_path)) {
        auto meta_raw = read_file(meta_path);
        if (meta_raw && meta_raw->size() >= sizeof(Lsn)) {
            std::memcpy(&checkpoint_lsn, meta_raw->data(), sizeof(Lsn));
        }
    }

    // 2. Open WalWriter (which initializes its next_lsn_)
    auto wal_st = wal_writer_.open(path_);
    if (wal_st != Status::kOk)
        return wal_st;

    // 3. Replay WAL from checkpoint_lsn (Two passes for PITR and proper recovery)
    WalReader wal_reader;
    std::unordered_map<TxId, Timestamp> committed_txns;

    if (wal_reader.open(path_) == Status::kOk) {
        // --- Pass 1: Restore Full Page Images & Collect Committed TxIds ---
        while (true) {
            auto rec = wal_reader.next();
            if (!rec)
                break;
            if (rec->header.lsn <= checkpoint_lsn)
                continue;
            if (recovery_target_time_ms && rec->header.physical_time_ms > *recovery_target_time_ms)
                break;

            if (rec->header.type == WalRecordType::kCommit &&
                rec->payload.size() >= sizeof(Timestamp)) {
                Timestamp commit_ts;
                std::memcpy(&commit_ts, rec->payload.data(), sizeof(Timestamp));
                committed_txns[rec->header.tx_id] = commit_ts;
                std::cout << "Replayed kCommit tx_id: " << rec->header.tx_id << std::endl;
            }
            else if (rec->header.type == WalRecordType::kFullPageImage) {
                if (rec->payload.size() > sizeof(TableId) + sizeof(PageId) + sizeof(int32_t)) {
                    TableId tid;
                    PageId pid;
                    int32_t file_id;
                    size_t pos = 0;
                    std::memcpy(&tid, rec->payload.data() + pos, sizeof(tid));
                    pos += sizeof(tid);
                    std::memcpy(&pid, rec->payload.data() + pos, sizeof(pid));
                    pos += sizeof(pid);
                    std::memcpy(&file_id, rec->payload.data() + pos, sizeof(file_id));
                    pos += sizeof(file_id);
                    size_t data_sz = rec->payload.size() - pos;
                    const std::byte *page_data = rec->payload.data() + pos;

                    if (tid < tables_.size()) {
                        auto &table = tables_[tid];
                        if (file_id < 0) {
                            if (pid + data_sz > table.file().size()) {
                                auto _ = table.file().resize(pid + data_sz);
                                (void)_;
                            }
                            std::memcpy(table.file().data() + pid, page_data, data_sz);
                        }
                        else {
                            if (auto *tree = table.get_index_by_col(static_cast<size_t>(file_id))) {
                                tree->write_page(pid, page_data, data_sz);
                            }
                        }
                    }
                }
            }
        }
        wal_reader.close();

        // --- Pass 2: Replay Logical Operations (Insert) ---
        if (wal_reader.open(path_) == Status::kOk) {
            while (true) {
                auto rec = wal_reader.next();
                if (!rec)
                    break;
                if (rec->header.lsn <= checkpoint_lsn)
                    continue;
                if (recovery_target_time_ms &&
                    rec->header.physical_time_ms > *recovery_target_time_ms)
                    break;

                if (rec->header.type == WalRecordType::kInsert &&
                    rec->payload.size() >= sizeof(TableId) + sizeof(uint32_t)) {
                    if (auto it = committed_txns.find(rec->header.tx_id);
                        it != committed_txns.end()) {
                        TableId tid;
                        size_t pos = 0;
                        std::memcpy(&tid, rec->payload.data() + pos, sizeof(TableId));
                        pos += sizeof(TableId);
                        uint32_t col_count;
                        std::memcpy(&col_count, rec->payload.data() + pos, sizeof(uint32_t));
                        pos += sizeof(uint32_t);

                        std::cout << "Replaying kInsert for tx_id: " << rec->header.tx_id
                                  << " table_id: " << tid << std::endl;

                        if (tid < tables_.size()) {
                            auto &table = tables_[tid];
                            if (rec->header.lsn > table.lsn()) {
                                std::vector<ColumnData> columns;
                                for (uint32_t i = 0; i < col_count; ++i) {
                                    uint8_t type_val;
                                    std::memcpy(&type_val,
                                                rec->payload.data() + pos,
                                                sizeof(uint8_t));
                                    pos += sizeof(uint8_t);
                                    uint32_t size_val;
                                    std::memcpy(&size_val,
                                                rec->payload.data() + pos,
                                                sizeof(uint32_t));
                                    pos += sizeof(uint32_t);
                                    const std::byte *data_ptr = rec->payload.data() + pos;
                                    pos += size_val;
                                    columns.push_back({static_cast<ColumnType>(type_val),
                                                       data_ptr,
                                                       size_val,
                                                       nullptr});
                                }

                                auto row_id_or =
                                    table.insert_row(rec->header.tx_id | kTxIdFlag, columns);
                                if (row_id_or) {
                                    std::cout << "Successfully inserted row " << *row_id_or
                                              << std::endl;
                                    table.commit_rows({*row_id_or}, rec->header.tx_id, it->second);
                                }
                            }
                            else {
                                std::cout << "Skipped insert due to LSN" << std::endl;
                            }
                        }
                    }
                    else {
                        std::cout << "Skipped insert: tx_id " << rec->header.tx_id
                                  << " not in committed_txns" << std::endl;
                    }
                }
                else if (rec->header.type == WalRecordType::kInsertBatch &&
                         rec->payload.size() >= sizeof(TableId) + sizeof(uint32_t) * 2) {
                    if (auto it = committed_txns.find(rec->header.tx_id);
                        it != committed_txns.end()) {
                        TableId tid;
                        size_t pos = 0;
                        std::memcpy(&tid, rec->payload.data() + pos, sizeof(TableId));
                        pos += sizeof(TableId);
                        uint32_t row_count;
                        std::memcpy(&row_count, rec->payload.data() + pos, sizeof(uint32_t));
                        pos += sizeof(uint32_t);
                        uint32_t col_count;
                        std::memcpy(&col_count, rec->payload.data() + pos, sizeof(uint32_t));
                        pos += sizeof(uint32_t);

                        if (tid < tables_.size()) {
                            auto &table = tables_[tid];
                            if (rec->header.lsn > table.lsn()) {
                                std::vector<std::vector<ColumnData>> rows;
                                rows.reserve(row_count);
                                for (uint32_t r = 0; r < row_count; ++r) {
                                    std::vector<ColumnData> columns;
                                    columns.reserve(col_count);
                                    for (uint32_t i = 0; i < col_count; ++i) {
                                        uint8_t type_val;
                                        std::memcpy(&type_val,
                                                    rec->payload.data() + pos,
                                                    sizeof(uint8_t));
                                        pos += sizeof(uint8_t);
                                        uint32_t size_val;
                                        std::memcpy(&size_val,
                                                    rec->payload.data() + pos,
                                                    sizeof(uint32_t));
                                        pos += sizeof(uint32_t);
                                        const std::byte *data_ptr = rec->payload.data() + pos;
                                        pos += size_val;
                                        columns.push_back({static_cast<ColumnType>(type_val),
                                                           data_ptr,
                                                           size_val,
                                                           nullptr});
                                    }
                                    rows.push_back(std::move(columns));
                                }

                                auto row_ids_or =
                                    table.insert_rows(rec->header.tx_id | kTxIdFlag, rows);
                                if (row_ids_or) {
                                    table.commit_rows(*row_ids_or, rec->header.tx_id, it->second);
                                }
                            }
                            else {
                                std::cout << "Skipped insert batch due to LSN" << std::endl;
                            }
                        }
                    }
                    else {
                        // Skip uncommitted
                    }
                }
            }
            wal_reader.close();
        }
    }

    // 4. Start checkpointer thread
    stop_checkpointer_ = false;
    checkpointer_thread_ = std::make_unique<std::thread>(&Database::checkpointer_thread_func, this);

    stop_auto_backup_ = false;
    auto_backup_thread_ = std::make_unique<std::thread>(&Database::auto_backup_thread_func, this);

    is_open_ = true;
    return Status::kOk;
}

void Database::close()
{
    if (!is_open_)
        return;

    {
        std::unique_lock lock(tables_mtx_);
        for (auto &tbl : tables_) {
            tbl.flush_pending();
        }
    }

    if (flush_handler_) {
        flush_handler_->flush_all();
    }

    stop_checkpointer_ = true;
    checkpointer_cv_.notify_all();
    if (checkpointer_thread_ && checkpointer_thread_->joinable()) {
        checkpointer_thread_->join();
    }
    checkpointer_thread_.reset();

    stop_auto_backup_ = true;
    auto_backup_cv_.notify_all();
    if (auto_backup_thread_ && auto_backup_thread_->joinable()) {
        auto_backup_thread_->join();
    }
    auto_backup_thread_.reset();

    gc_->stop();
    flush_handler_.reset();
    gc_.reset();

    wal_writer_.close();

    {
        std::unique_lock lock(tables_mtx_);
        for (auto &tbl : tables_) {
            tbl.sync();
        }
        for (auto &tbl : tables_) {
            tbl.save_vindex(path_);
        }
        tables_.clear();
    }
    is_open_ = false;
}

auto Database::create_table(const std::string &name, Schema schema) -> StatusOr<TableId>
{
    if (!is_open_) {
        return std::unexpected(Status::kInvalidArgument);
    }

    std::unique_lock lock(tables_mtx_);
    TableId id = static_cast<TableId>(tables_.size());
    tables_.emplace_back(name, std::move(schema));
    auto &tbl = tables_.back();
    tbl.set_fpw_callback([this, id](PageId p, const std::byte *d, size_t s) {
        this->fpw_callback(id, p, -1, d, s);
    });
    if (flush_handler_) {
        tbl.set_flush_handler(flush_handler_.get());
    }
    auto st = tbl.open_file(path_);
    if (st != Status::kOk) {
        tables_.pop_back();
        return std::unexpected(st);
    }

    auto schema_bytes = schema_to_bytes(tbl.schema());
    auto schema_path = path_ / (name + ".schema");
    if (write_file(schema_path, schema_bytes) != Status::kOk) {
        tbl.close_file();
        tables_.pop_back();
        return std::unexpected(Status::kIoError);
    }

    return id;
}

auto Database::insert(TableId table_id,
                      const std::vector<ColumnData> &columns,
                      const std::shared_ptr<Transaction> &txn) -> StatusOr<RowId>
{
    if (table_id >= tables_.size()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    auto lsn_res = wal_writer_.append_insert(txn ? txn->tx_id : kInvalidTxId, table_id, columns);
    if (!lsn_res)
        return std::unexpected(lsn_res.error());
    tables_[table_id].set_lsn(*lsn_res);

    StatusOr<RowId> res;
    if (txn) {
        res = tables_[table_id].insert_row(txn->tx_id | kTxIdFlag, columns);
        if (res) {
            txn->write_set[table_id].push_back(*res);
        }
    }
    else {
        res = tables_[table_id].insert_row(timestamps_, columns);
    }
    return res;
}

auto Database::insert_batch(TableId table_id,
                            const std::vector<std::vector<ColumnData>> &rows,
                            const std::shared_ptr<Transaction> &txn) -> StatusOr<std::vector<RowId>>
{
    if (table_id >= tables_.size()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    auto lsn_res = wal_writer_.append_insert_batch(txn ? txn->tx_id : kInvalidTxId, table_id, rows);
    if (!lsn_res)
        return std::unexpected(lsn_res.error());
    tables_[table_id].set_lsn(*lsn_res);

    StatusOr<std::vector<RowId>> res;
    if (txn) {
        res = tables_[table_id].insert_rows(txn->tx_id | kTxIdFlag, rows);
        if (res) {
            txn->write_set[table_id].insert(txn->write_set[table_id].end(),
                                            res->begin(),
                                            res->end());
        }
    }
    else {
        res = tables_[table_id].insert_rows(timestamps_, rows);
    }
    return res;
}

auto Database::vacuum(TableId table_id) -> Status
{
    auto lock = tables_lock_unique();
    if (table_id >= tables_.size())
        return Status::kInvalidArgument;
    auto &tbl = tables_[table_id];

    struct SavedIdx
    {
        size_t col_idx;
        ColumnType col_type;
        std::string name;
    };
    std::vector<SavedIdx> saved;
    tbl.for_each_index([&](auto &idx) {
        saved.push_back({idx.column_idx, idx.column_type, idx.name});
        idx.tree.close();
        auto idx_path = path_ / (tbl.name() + "_" + std::to_string(idx.column_idx) + ".idx");
        std::error_code ec;
        std::filesystem::remove(idx_path, ec);
    });
    tbl.clear_indexes();

    auto new_count = tbl.vacuum(timestamps_);
    if (!new_count)
        return new_count.error();

    if (!saved.empty()) {
        auto full_scan = Executor::read_table_columns(tbl);
        if (full_scan) {
            size_t row_count = tbl.row_count();
            for (auto &si : saved) {
                auto idx_path = path_ / (tbl.name() + "_" + std::to_string(si.col_idx) + ".idx");
                auto tree_r = BTree::create(idx_path, si.col_type);
                if (!tree_r)
                    return tree_r.error();

                IndexInfo info;
                info.name = si.name;
                info.column_name = tbl.schema().names[si.col_idx];
                info.column_idx = si.col_idx;
                info.column_type = si.col_type;
                info.tree = std::move(*tree_r);

                const auto &cd = full_scan->columns[si.col_idx];
                if (si.col_type == ColumnType::kVarChar) {
                    auto *offsets =
                        reinterpret_cast<const uint32_t *>(static_cast<const void *>(cd.data));
                    for (size_t ri = 0; ri < row_count; ++ri) {
                        uint32_t end = offsets[ri];
                        uint32_t start = (ri == 0) ? 0 : offsets[ri - 1];
                        const std::byte *key = cd.data + row_count * sizeof(uint32_t) + start;
                        size_t key_len = static_cast<size_t>(end - start);
                        auto st = info.tree.insert(key, key_len, static_cast<RowId>(ri));
                        if (st != Status::kOk) {
                            info.tree.close();
                            return st;
                        }
                    }
                }
                else {
                    size_t elem_size = 0;
                    switch (si.col_type) {
                        case ColumnType::kInt32:
                            elem_size = 4;
                            break;
                        case ColumnType::kTimestamp:
                        case ColumnType::kInt64:
                            elem_size = 8;
                            break;
                        case ColumnType::kFloat64:
                            elem_size = 8;
                            break;
                        case ColumnType::kBool:
                            elem_size = 1;
                            break;
                        default:
                            break;
                    }
                    for (size_t ri = 0; ri < row_count; ++ri) {
                        const std::byte *key = cd.data + ri * elem_size;
                        auto st = info.tree.insert(key, elem_size, static_cast<RowId>(ri));
                        if (st != Status::kOk) {
                            info.tree.close();
                            return st;
                        }
                    }
                }

                tbl.add_index(std::move(info));
            }
        }
    }

    return Status::kOk;
}

auto Database::delete_rows(TableId table_id,
                           std::vector<RowId> row_ids,
                           const std::shared_ptr<Transaction> &txn) -> Status
{
    if (table_id >= tables_.size()) {
        return Status::kInvalidArgument;
    }

    auto lsn_res = wal_writer_.append_delete(txn ? txn->tx_id : kInvalidTxId, table_id, row_ids);
    if (!lsn_res)
        return lsn_res.error();
    tables_[table_id].set_lsn(*lsn_res);

    Timestamp ts = txn ? (txn->tx_id | kTxIdFlag) : timestamps_.allocate_ts();

    std::vector<IndexEntry> entries;
    entries.reserve(row_ids.size());
    for (auto rid : row_ids) {
        entries.push_back({rid, ts, Table::kNotFoundPage, 0});
    }

    tables_[table_id].insert_version_entries(entries.data(), entries.size());

    if (txn) {
        auto &ws = txn->write_set[table_id];
        ws.insert(ws.end(), row_ids.begin(), row_ids.end());
    }

    return Status::kOk;
}

auto Database::checkpoint() -> Status
{
    std::lock_guard exec_lock(checkpoint_exec_mtx_);
    Lsn checkpoint_lsn = wal_writer_.current_lsn();

    // Flush all tables to disk and save version index
    {
        std::shared_lock lock(tables_mtx_);
        for (auto &tbl : tables_) {
            if (auto s = tbl.flush_pending(); s.code != Status::kOk)
                return s;
            tbl.sync();
            tbl.save_vindex(path_);
        }
    }

    // Write checkpoint_lsn to checkpoint.meta
    std::vector<std::byte> meta_data(sizeof(Lsn));
    std::memcpy(meta_data.data(), &checkpoint_lsn, sizeof(Lsn));
    auto meta_path = path_ / "checkpoint.meta";
    if (write_file(meta_path, meta_data) != Status::kOk) {
        return Status::kIoError;
    }

    // Determine safe_lsn
    Lsn safe_lsn = checkpoint_lsn;
    Lsn oldest_active = txn_manager_->oldest_active_lsn();
    if (oldest_active != static_cast<Lsn>(-1) && oldest_active < safe_lsn) {
        safe_lsn = oldest_active;
    }

    // Cleanup old segments
    wal_writer_.remove_segments_before(safe_lsn);

    return Status::kOk;
}

auto Database::start_backup(const std::filesystem::path &dest_path) -> Status
{
    if (!std::filesystem::exists(dest_path)) {
        std::error_code ec;
        std::filesystem::create_directories(dest_path, ec);
        if (ec)
            return Status{Status::kIoError, "failed to create backup dir"};
    }

    // 1. Checkpoint to flush dirty pages
    if (auto s = checkpoint(); s.code != Status::kOk)
        return s;

    // 2. Start backup mode
    bool expected = false;
    if (!backup_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Status{Status::kAlreadyExists, "Backup already in progress"};
    }
    {
        std::lock_guard lock(fpw_mtx_);
        logged_pages_.clear();
    }

    // 3. Log backup start (using current LSN)
    Lsn start_lsn = wal_writer_.current_lsn();

    // 4. Fuzzy copy
    try {
        for (const auto &entry : std::filesystem::directory_iterator(path_)) {
            if (entry.is_regular_file()) {
                auto dest_file = dest_path / entry.path().filename();
                std::filesystem::copy_file(entry.path(),
                                           dest_file,
                                           std::filesystem::copy_options::overwrite_existing);
            }
        }
    }
    catch (const std::exception &e) {
        backup_in_progress_.store(false, std::memory_order_relaxed);
        return Status{Status::kIoError, e.what()};
    }

    // 5. End backup
    backup_in_progress_.store(false, std::memory_order_relaxed);

    // Write backup.meta
    std::vector<std::byte> meta_data(sizeof(Lsn));
    std::memcpy(meta_data.data(), &start_lsn, sizeof(Lsn));
    write_file(dest_path / "backup.meta", meta_data);

    return Status::kOk;
}
void Database::checkpointer_thread_func()
{
    while (!stop_checkpointer_) {
        std::unique_lock lock(checkpointer_mtx_);
        checkpointer_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return stop_checkpointer_.load();
        });
        if (stop_checkpointer_)
            break;

        auto st = checkpoint();
        if (st != Status::kOk) {
            // Log error
        }
    }
}

void Database::auto_backup_thread_func()
{
    while (!stop_auto_backup_) {
        std::unique_lock lock(auto_backup_mtx_);
        uint32_t interval = backup_interval_seconds_ > 0 ? backup_interval_seconds_
                                                         : 60; // default wake up every 60s
        auto_backup_cv_.wait_for(lock, std::chrono::seconds(interval), [this] {
            return stop_auto_backup_.load();
        });
        if (stop_auto_backup_)
            break;

        if (backup_interval_seconds_ > 0) {
            uint64_t now_ms =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
            auto dest_path = path_.parent_path() / ("backup_" + std::to_string(now_ms));
            auto st = start_backup(dest_path);
            if (st == Status::kOk) {
                cleanup_old_backups();
            }
        }
    }
}

void Database::cleanup_old_backups()
{
    if (backup_retention_seconds_ == 0)
        return;

    uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    uint64_t cutoff_ms = now_ms - (backup_retention_seconds_ * 1000ULL);

    auto parent_dir = path_.parent_path();
    for (const auto &entry : std::filesystem::directory_iterator(parent_dir)) {
        if (entry.is_directory()) {
            std::string name = entry.path().filename().string();
            if (name.starts_with("backup_")) {
                try {
                    uint64_t ts = std::stoull(name.substr(7));
                    if (ts < cutoff_ms) {
                        std::error_code ec;
                        std::filesystem::remove_all(entry.path(), ec);
                    }
                }
                catch (...) {
                    // Ignore non-timestamp backup directories
                }
            }
        }
    }
}

void Database::fpw_callback(TableId table_id,
                            PageId page_id,
                            int32_t file_id,
                            const std::byte *data,
                            size_t size)
{
    if (!backup_in_progress_.load(std::memory_order_relaxed))
        return;

    LoggedPageKey key{table_id, page_id, file_id};
    {
        std::lock_guard lock(fpw_mtx_);
        if (logged_pages_.count(key))
            return;
        logged_pages_.insert(key);
    }

    auto _ = wal_writer_.append_full_page(table_id, page_id, file_id, data, size);
    (void)_;
}

} // namespace rawdb
