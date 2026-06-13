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
        case ColumnType::kTimestamp: {
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
    if (stmt.has_join) {
        return execute_join(stmt);
    }

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

    if (!stmt.has_order_by && stmt.has_where && stmt.where->type == ExprNode::Type::kPredicate && stmt.where->pred.op == CmpOp::kEq) {
        auto index_rids = index_lookup(tbl, stmt.where->pred);
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
            auto filtered = Filter::evaluate(full_scan.columns, schema, row_count, stmt.where.get());
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

    if (stmt.has_offset && stmt.offset_count > 0 && !is_agg) {
        if (stmt.offset_count >= visible.size()) {
            visible.clear();
        } else {
            visible.erase(visible.begin(), visible.begin() + static_cast<ptrdiff_t>(stmt.offset_count));
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
        
        if (stmt.has_offset && stmt.offset_count > 0) {
            if (stmt.offset_count >= result.rows.size()) {
                result.rows.clear();
            } else {
                result.rows.erase(result.rows.begin(), result.rows.begin() + static_cast<ptrdiff_t>(stmt.offset_count));
            }
        }
        if (stmt.has_limit && result.rows.size() > stmt.limit_count) {
            result.rows.resize(stmt.limit_count);
        }
    }

    return result;
}

auto Executor::execute_join(const SelectStmt &stmt) -> StatusOr<QueryResult>
{
    // Find Left Table
    TableId tid_left = 0;
    bool found_left = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.table_name) {
            tid_left = static_cast<TableId>(i);
            found_left = true;
            break;
        }
    }
    if (!found_left) return std::unexpected(Status::kNotFound);

    // Find Right Table
    TableId tid_right = 0;
    bool found_right = false;
    for (TableId i = 0; i < static_cast<TableId>(conn_.db().table_count()); ++i) {
        if (conn_.db().table(i).name() == stmt.join_clause.table_name) {
            tid_right = static_cast<TableId>(i);
            found_right = true;
            break;
        }
    }
    if (!found_right) return std::unexpected(Status::kNotFound);

    auto &tbl_left = conn_.db().table(tid_left);
    auto &tbl_right = conn_.db().table(tid_right);
    const auto &schema_left = tbl_left.schema();
    const auto &schema_right = tbl_right.schema();

    tbl_left.flush_pending();
    tbl_right.flush_pending();

    Timestamp read_ts = conn_.txn() ? conn_.txn()->read_ts : conn_.db().next_ts();
    size_t row_count_left = tbl_left.row_count();
    size_t row_count_right = tbl_right.row_count();

    QueryResult result;
    
    struct ColMapping {
        bool is_left;
        size_t idx;
    };
    std::vector<ColMapping> output_mapping;

    if (stmt.columns.empty()) {
        for (size_t i = 0; i < schema_left.names.size(); ++i) {
            result.column_names.push_back(stmt.table_name + "." + schema_left.names[i]);
            result.column_types.push_back(schema_left.columns[i]);
            output_mapping.push_back({true, i});
        }
        for (size_t i = 0; i < schema_right.names.size(); ++i) {
            result.column_names.push_back(stmt.join_clause.table_name + "." + schema_right.names[i]);
            result.column_types.push_back(schema_right.columns[i]);
            output_mapping.push_back({false, i});
        }
    } else {
        for (const auto &col : stmt.columns) {
            std::string col_name = col.name;
            size_t left_idx = static_cast<size_t>(-1);
            size_t right_idx = static_cast<size_t>(-1);

            if (col.table.empty() || col.table == stmt.table_name) {
                auto it = std::find(schema_left.names.begin(), schema_left.names.end(), col_name);
                if (it != schema_left.names.end()) {
                    left_idx = static_cast<size_t>(std::distance(schema_left.names.begin(), it));
                }
            }
            if (col.table.empty() || col.table == stmt.join_clause.table_name) {
                auto it = std::find(schema_right.names.begin(), schema_right.names.end(), col_name);
                if (it != schema_right.names.end()) {
                    right_idx = static_cast<size_t>(std::distance(schema_right.names.begin(), it));
                }
            }

            if (left_idx != static_cast<size_t>(-1) && right_idx != static_cast<size_t>(-1)) {
                return std::unexpected(Status::kInvalidArgument); // Ambiguous column reference
            } else if (left_idx != static_cast<size_t>(-1)) {
                result.column_names.push_back(col.table.empty() ? col_name : col.table + "." + col_name);
                result.column_types.push_back(schema_left.columns[left_idx]);
                output_mapping.push_back({true, left_idx});
            } else if (right_idx != static_cast<size_t>(-1)) {
                result.column_names.push_back(col.table.empty() ? col_name : col.table + "." + col_name);
                result.column_types.push_back(schema_right.columns[right_idx]);
                output_mapping.push_back({false, right_idx});
            } else {
                return std::unexpected(Status::kNotFound);
            }
        }
    }

    auto resolve_col = [&](const ColumnRef &col) -> std::pair<bool, size_t> {
        size_t left_idx = static_cast<size_t>(-1);
        size_t right_idx = static_cast<size_t>(-1);

        if (col.table.empty() || col.table == stmt.table_name) {
            auto it = std::find(schema_left.names.begin(), schema_left.names.end(), col.name);
            if (it != schema_left.names.end()) left_idx = static_cast<size_t>(std::distance(schema_left.names.begin(), it));
        }
        if (col.table.empty() || col.table == stmt.join_clause.table_name) {
            auto it = std::find(schema_right.names.begin(), schema_right.names.end(), col.name);
            if (it != schema_right.names.end()) right_idx = static_cast<size_t>(std::distance(schema_right.names.begin(), it));
        }

        if (left_idx != static_cast<size_t>(-1) && right_idx != static_cast<size_t>(-1)) {
            return {false, static_cast<size_t>(-1)}; // Ambiguous
        } else if (left_idx != static_cast<size_t>(-1)) {
            return {true, left_idx};
        } else if (right_idx != static_cast<size_t>(-1)) {
            return {false, right_idx};
        }
        return {false, static_cast<size_t>(-1)};
    };

    auto left_res = resolve_col(stmt.join_clause.left_col);
    auto right_res = resolve_col(stmt.join_clause.right_col);

    if (left_res.second == static_cast<size_t>(-1) || right_res.second == static_cast<size_t>(-1)) {
        return std::unexpected(Status::kNotFound);
    }
    if (left_res.first == right_res.first) {
        return std::unexpected(Status::kInvalidArgument);
    }

    size_t join_col_left_idx = left_res.first ? left_res.second : right_res.second;
    size_t join_col_right_idx = !left_res.first ? left_res.second : right_res.second;

    std::vector<size_t> all_left;
    for (size_t i = 0; i < schema_left.columns.size(); ++i) all_left.push_back(i);
    std::vector<size_t> all_right;
    for (size_t i = 0; i < schema_right.columns.size(); ++i) all_right.push_back(i);

    std::vector<RowId> left_visible;
    for (size_t ri = 0; ri < row_count_left; ++ri) {
        auto r = tbl_left.search_version_index(static_cast<RowId>(ri), read_ts);
        if (!r || *r != Table::kNotFoundPage) left_visible.push_back(static_cast<RowId>(ri));
    }
    
    std::vector<RowId> right_visible;
    for (size_t ri = 0; ri < row_count_right; ++ri) {
        auto r = tbl_right.search_version_index(static_cast<RowId>(ri), read_ts);
        if (!r || *r != Table::kNotFoundPage) right_visible.push_back(static_cast<RowId>(ri));
    }

    auto scan_left = tbl_left.read_rows(left_visible, all_left);
    if (!scan_left) return std::unexpected(scan_left.error());
    auto scan_right = tbl_right.read_rows(right_visible, all_right);
    if (!scan_right) return std::unexpected(scan_right.error());

    std::unordered_multimap<std::string, size_t> hash_table;
    for (size_t k = 0; k < right_visible.size(); ++k) {
        std::string val = value_to_string(scan_right->columns[join_col_right_idx], schema_right.columns[join_col_right_idx], k, right_visible.size());
        hash_table.insert({val, k});
    }

    for (size_t k_left = 0; k_left < left_visible.size(); ++k_left) {
        std::string val_left = value_to_string(scan_left->columns[join_col_left_idx], schema_left.columns[join_col_left_idx], k_left, left_visible.size());
        
        auto range = hash_table.equal_range(val_left);
        for (auto it = range.first; it != range.second; ++it) {
            size_t k_right = it->second;
            
            std::vector<std::string> row_out;
            for (auto map : output_mapping) {
                if (map.is_left) {
                    row_out.push_back(value_to_string(scan_left->columns[map.idx], schema_left.columns[map.idx], k_left, left_visible.size()));
                } else {
                    row_out.push_back(value_to_string(scan_right->columns[map.idx], schema_right.columns[map.idx], k_right, right_visible.size()));
                }
            }
            result.rows.push_back(std::move(row_out));
        }
    }

    if (stmt.has_offset && stmt.offset_count > 0) {
        if (stmt.offset_count >= result.rows.size()) {
            result.rows.clear();
        } else {
            result.rows.erase(result.rows.begin(), result.rows.begin() + static_cast<ptrdiff_t>(stmt.offset_count));
        }
    }
    if (stmt.has_limit && result.rows.size() > stmt.limit_count) {
        result.rows.resize(stmt.limit_count);
    }

    return result;
}

} // namespace rawdb
