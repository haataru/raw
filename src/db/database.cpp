#include "db/database.hpp"

#include <filesystem>
#include <fstream>

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

auto Database::open(const std::filesystem::path &path) -> Status
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
                for (auto &tbl : tables_) {
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
    txn_manager_ = std::make_unique<TransactionManager>(timestamps_);
    gc_ = std::make_unique<GarbageCollector>(tables_, tables_mtx_, timestamps_, watermarks_);
    gc_->start();

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
    gc_->stop();
    flush_handler_.reset();
    gc_.reset();

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

auto Database::insert(TableId table_id, const std::vector<ColumnData> &columns, const std::shared_ptr<Transaction>& txn) -> StatusOr<RowId>
{
    if (table_id >= tables_.size()) {
        return std::unexpected(Status::kInvalidArgument);
    }

    Lsn lsn = wal_writer_.append_insert(txn ? txn->tx_id : kInvalidTxId, table_id, columns);
    tables_[table_id].set_lsn(lsn);

    StatusOr<RowId> res;
    if (txn) {
        res = tables_[table_id].insert_row(txn->tx_id | kTxIdFlag, columns);
        if (res) {
            txn->write_set[table_id].push_back(*res);
        }
    } else {
        res = tables_[table_id].insert_row(timestamps_, columns);
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

auto Database::delete_rows(TableId table_id, std::vector<RowId> row_ids, const std::shared_ptr<Transaction>& txn) -> Status
{
    if (table_id >= tables_.size()) {
        return Status::kInvalidArgument;
    }

    Lsn lsn = wal_writer_.append_delete(txn ? txn->tx_id : kInvalidTxId, table_id, row_ids);
    tables_[table_id].set_lsn(lsn);

    Timestamp ts = txn ? (txn->tx_id | kTxIdFlag) : timestamps_.allocate_ts();

    std::vector<IndexEntry> entries;
    entries.reserve(row_ids.size());
    for (auto rid : row_ids) {
        entries.push_back({rid, ts, Table::kNotFoundPage, 0});
    }

    tables_[table_id].insert_version_entries(entries.data(), entries.size());
    
    if (txn) {
        auto& ws = txn->write_set[table_id];
        ws.insert(ws.end(), row_ids.begin(), row_ids.end());
    }

    return Status::kOk;
}

} // namespace rawdb
