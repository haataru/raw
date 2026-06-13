#include <algorithm>
#include <cstring>

#include "query/executor.hpp"

namespace rawdb
{

static auto compare_column_values(const ColumnData &col,
                                  ColumnType type,
                                  size_t a,
                                  size_t b,
                                  size_t total_rows) -> int
{
    switch (type) {
        case ColumnType::kInt32: {
            auto *arr = static_cast<const int32_t *>(static_cast<const void *>(col.data));
            return (arr[a] < arr[b]) ? -1 : (arr[a] > arr[b] ? 1 : 0);
        }
        case ColumnType::kInt64: {
            auto *arr = static_cast<const int64_t *>(static_cast<const void *>(col.data));
            return (arr[a] < arr[b]) ? -1 : (arr[a] > arr[b] ? 1 : 0);
        }
        case ColumnType::kFloat64: {
            auto *arr = static_cast<const double *>(static_cast<const void *>(col.data));
            if (arr[a] < arr[b])
                return -1;
            if (arr[a] > arr[b])
                return 1;
            return 0;
        }
        case ColumnType::kBool: {
            auto *arr = static_cast<const bool *>(static_cast<const void *>(col.data));
            return static_cast<int>(arr[a]) - static_cast<int>(arr[b]);
        }
        case ColumnType::kVarChar: {
            auto *raw = static_cast<const std::byte *>(col.data);
            size_t off_bytes = total_rows * sizeof(uint32_t);
            if (col.size < off_bytes)
                return 0;
            auto *offsets = reinterpret_cast<const uint32_t *>(static_cast<const void *>(raw));
            uint32_t a_start = (a == 0) ? 0 : offsets[a - 1];
            uint32_t a_end = offsets[a];
            uint32_t b_start = (b == 0) ? 0 : offsets[b - 1];
            uint32_t b_end = offsets[b];
            size_t a_len = a_end - a_start;
            size_t b_len = b_end - b_start;
            const char *a_str = reinterpret_cast<const char *>(raw + off_bytes + a_start);
            const char *b_str = reinterpret_cast<const char *>(raw + off_bytes + b_start);
            auto min_len = std::min(a_len, b_len);
            int cmp = std::memcmp(a_str, b_str, min_len);
            if (cmp != 0)
                return cmp;
            return (a_len < b_len) ? -1 : (a_len > b_len ? 1 : 0);
        }
    }
    return 0;
}

auto Executor::execute_select(const SelectStmt &stmt) -> StatusOr<QueryResult>
{
    TableId tid = 0;
    bool found = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.table_name) {
            tid = static_cast<TableId>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected(Status::kNotFound);
    }

    auto &tbl = conn_.db().table(tid);
    const auto &schema = tbl.schema();

    tbl.flush_pending();
    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    size_t row_count = tbl.row_count();

    QueryResult result;
    if (stmt.columns.empty()) {
        for (const auto &name : schema.names) {
            result.column_names.push_back(name);
        }
        result.column_types = schema.columns;
    }
    else {
        for (const auto &col : stmt.columns) {
            std::string col_name_out;
            if (col.func == AggFunc::kCount) col_name_out = "COUNT(" + col.name + ")";
            else if (col.func == AggFunc::kSum) col_name_out = "SUM(" + col.name + ")";
            else if (col.func == AggFunc::kAvg) col_name_out = "AVG(" + col.name + ")";
            else if (col.func == AggFunc::kMin) col_name_out = "MIN(" + col.name + ")";
            else if (col.func == AggFunc::kMax) col_name_out = "MAX(" + col.name + ")";
            else col_name_out = col.name;
            
            result.column_names.push_back(col_name_out);
            
            bool found_type = false;
            for (size_t ci = 0; ci < schema.names.size(); ++ci) {
                if (schema.names[ci] == col.name) {
                    // For aggregates, type might change (e.g. COUNT is INT64, SUM is FLOAT64 ideally, but keep it simple)
                    if (col.func == AggFunc::kCount) result.column_types.push_back(ColumnType::kInt64);
                    else if (col.func == AggFunc::kAvg || col.func == AggFunc::kSum) result.column_types.push_back(ColumnType::kFloat64);
                    else result.column_types.push_back(schema.columns[ci]);
                    found_type = true;
                    break;
                }
            }
            if (!found_type && col.is_star && col.func == AggFunc::kCount) {
                result.column_types.push_back(ColumnType::kInt64);
            }
        }
    }

    std::vector<size_t> col_indices;
    bool is_agg = !stmt.group_by.empty();
    if (stmt.columns.empty()) {
        for (size_t ci = 0; ci < schema.names.size(); ++ci) {
            col_indices.push_back(ci);
        }
    } else {
        for (const auto &col : stmt.columns) {
            if (col.func != AggFunc::kNone) {
                is_agg = true;
            }
            if (col.is_star) continue;
            for (size_t ci = 0; ci < schema.names.size(); ++ci) {
                if (schema.names[ci] == col.name) {
                    col_indices.push_back(ci);
                    break;
                }
            }
        }
    }
    
    std::vector<size_t> group_by_indices;
    for (const auto &gb : stmt.group_by) {
        for (size_t ci = 0; ci < schema.names.size(); ++ci) {
            if (schema.names[ci] == gb.name) {
                group_by_indices.push_back(ci);
                // Also add to col_indices if not already there, to load the column
                if (std::find(col_indices.begin(), col_indices.end(), ci) == col_indices.end()) {
                    col_indices.push_back(ci);
                }
                break;
            }
        }
    }

    size_t order_col = static_cast<size_t>(-1);
    bool have_order_col = false;
    if (stmt.has_order_by) {
        for (size_t ci = 0; ci < schema.names.size(); ++ci) {
            if (schema.names[ci] == stmt.order_by.column.name) {
                order_col = ci;
                have_order_col = true;
                break;
            }
        }
    }

    std::vector<RowId> visible;
    bool index_used = false;
    TableScanResult full_scan;

    if (!stmt.has_order_by && stmt.has_where && stmt.where.op == CmpOp::kEq) {
        auto index_rids = index_lookup(tbl, stmt.where);
        if (index_rids.has_value()) {
            index_used = true;
            for (auto rid : *index_rids) {
                if (static_cast<size_t>(rid) >= row_count)
                    continue;
                auto r = tbl.search_version_index(rid, read_ts);
                if (!r || *r != Table::kNotFoundPage) {
                    visible.push_back(rid);
                }
            }
        }
    }

    if (!index_used) {
        auto scan_r = read_table_columns(tbl);
        if (!scan_r)
            return std::unexpected(scan_r.error());
        full_scan = std::move(*scan_r);

        std::vector<size_t> matching;
        if (stmt.has_where) {
            auto filtered = Filter::evaluate(full_scan.columns, schema, row_count, stmt.where);
            if (!filtered)
                return std::unexpected(filtered.error());
            matching = std::move(*filtered);
        }
        else {
            matching.resize(row_count);
            for (size_t i = 0; i < row_count; ++i)
                matching[i] = i;
        }

        for (auto row_idx : matching) {
            auto r = tbl.search_version_index(static_cast<RowId>(row_idx), read_ts);
            if (!r || *r != Table::kNotFoundPage) {
                visible.push_back(static_cast<RowId>(row_idx));
            }
        }

        if (stmt.has_order_by && !visible.empty() && have_order_col &&
            order_col < full_scan.columns.size()) {
            std::sort(visible.begin(), visible.end(), [&](RowId a, RowId b) -> bool {
                int cmp = compare_column_values(full_scan.columns[order_col],
                                                schema.columns[order_col],
                                                a,
                                                b,
                                                row_count);
                return stmt.order_by.asc ? (cmp < 0) : (cmp > 0);
            });
        }
    }

    if (stmt.has_limit && visible.size() > stmt.limit_count && !is_agg) {
        // For aggregation, LIMIT applies AFTER aggregation
        visible.resize(stmt.limit_count);
    }

    if (visible.empty()) {
        if (is_agg && stmt.group_by.empty()) {
            // Return 1 row with 0/null for aggregates
            std::vector<std::string> row;
            for (const auto &col : stmt.columns) {
                if (col.func == AggFunc::kCount) row.push_back("0");
                else row.push_back("0"); // Simplification for empty
            }
            result.rows.push_back(std::move(row));
        }
        return result;
    }

    auto final_scan = tbl.read_rows(visible, col_indices);
    if (!final_scan)
        return std::unexpected(final_scan.error());

    if (!is_agg) {
        result.rows.reserve(visible.size());
        for (size_t ri = 0; ri < visible.size(); ++ri) {
            std::vector<std::string> row;
            if (stmt.columns.empty()) {
                // SELECT *
                for (size_t ci = 0; ci < col_indices.size(); ++ci) {
                    size_t col_idx = col_indices[ci];
                    auto val = value_to_string(final_scan->columns[col_idx],
                                               schema.columns[col_idx],
                                               ri,
                                               visible.size());
                    row.push_back(std::move(val));
                }
            } else {
                for (const auto &col : stmt.columns) {
                    size_t col_idx = static_cast<size_t>(-1);
                    for (size_t i = 0; i < schema.names.size(); ++i) {
                        if (schema.names[i] == col.name) col_idx = i;
                    }
                    
                    auto val = value_to_string(final_scan->columns[col_idx],
                                               schema.columns[col_idx],
                                               ri,
                                               visible.size());
                    row.push_back(std::move(val));
                }
            }
            result.rows.push_back(std::move(row));
        }
    } else {
        // Aggregation Logic
        struct AggState {
            int64_t count{0};
            double sum{0.0};
            double min{std::numeric_limits<double>::max()};
            double max{std::numeric_limits<double>::lowest()};
        };
        
        std::unordered_map<std::string, std::vector<AggState>> groups;
        std::vector<std::string> group_keys_order;
        
        for (size_t ri = 0; ri < visible.size(); ++ri) {
            std::string key = "";
            for (size_t gbi : group_by_indices) {
                key += value_to_string(final_scan->columns[gbi], schema.columns[gbi], ri, visible.size()) + "|";
            }
            
            if (groups.find(key) == groups.end()) {
                groups[key].resize(stmt.columns.size());
                group_keys_order.push_back(key);
            }
            
            auto& states = groups[key];
            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                states[ci].count++;
                
                if (col.func != AggFunc::kNone && col.func != AggFunc::kCount) {
                    size_t col_idx = static_cast<size_t>(-1);
                    for (size_t i = 0; i < schema.names.size(); ++i) {
                        if (schema.names[i] == col.name) col_idx = i;
                    }
                    std::string sval = value_to_string(final_scan->columns[col_idx], schema.columns[col_idx], ri, visible.size());
                    double dval = 0;
                    if (!sval.empty()) {
                        try { dval = std::stod(sval); } catch (...) {}
                    }
                    states[ci].sum += dval;
                    if (dval < states[ci].min) states[ci].min = dval;
                    if (dval > states[ci].max) states[ci].max = dval;
                }
            }
        }
        
        for (const auto& key : group_keys_order) {
            const auto& states = groups[key];
            std::vector<std::string> row;
            // The first few columns might be group_by columns. We need to parse the key or just re-read?
            // Actually we can just look up the key string, but it's easier to just use the original value if it's a group by column!
            // Wait, we need to extract the original values.
            // Let's just do a naive split of the key.
            size_t key_pos = 0;
            
            for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                const auto& col = stmt.columns[ci];
                if (col.func == AggFunc::kCount) {
                    row.push_back(std::to_string(states[ci].count));
                } else if (col.func == AggFunc::kSum) {
                    row.push_back(std::to_string(states[ci].sum));
                } else if (col.func == AggFunc::kAvg) {
                    row.push_back(std::to_string(states[ci].sum / static_cast<double>(states[ci].count)));
                } else if (col.func == AggFunc::kMin) {
                    row.push_back(std::to_string(states[ci].min));
                } else if (col.func == AggFunc::kMax) {
                    row.push_back(std::to_string(states[ci].max));
                } else {
                    // It's a GROUP BY column.
                    size_t next_pipe = key.find('|', key_pos);
                    row.push_back(key.substr(key_pos, next_pipe - key_pos));
                    key_pos = next_pipe + 1;
                }
            }
            result.rows.push_back(std::move(row));
        }
        
        if (stmt.has_limit && result.rows.size() > stmt.limit_count) {
            result.rows.resize(stmt.limit_count);
        }
    }

    return result;
}

} // namespace rawdb
