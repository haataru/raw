#include <cmath>
#include "query/simd_ops.hpp"

#include "query/executor.hpp"

namespace rawdb
{

auto Filter::get_column_index(const Schema &schema, std::string_view name) -> StatusOr<size_t>
{
    for (size_t i = 0; i < schema.names.size(); ++i) {
        if (schema.names[i] == name) {
            return i;
        }
    }
    return std::unexpected(Status::kNotFound);
}

static auto apply_int(int64_t v, int64_t val, CmpOp op) -> bool
{
    switch (op) {
        case CmpOp::kEq:
            return v == val;
        case CmpOp::kNe:
            return v != val;
        case CmpOp::kLt:
            return v < val;
        case CmpOp::kGt:
            return v > val;
        case CmpOp::kLe:
            return v <= val;
        case CmpOp::kGe:
            return v >= val;
    }
    return false;
}

static auto apply_double(double v, double val, CmpOp op) -> bool
{
    switch (op) {
        case CmpOp::kEq:
            return std::fabs(v - val) < 1e-12;
        case CmpOp::kNe:
            return std::fabs(v - val) >= 1e-12;
        case CmpOp::kLt:
            return v < val;
        case CmpOp::kGt:
            return v > val;
        case CmpOp::kLe:
            return v <= val;
        case CmpOp::kGe:
            return v >= val;
    }
    return false;
}

static auto apply_str(std::string_view v, std::string_view val, CmpOp op) -> bool
{
    switch (op) {
        case CmpOp::kEq:
            return v == val;
        case CmpOp::kNe:
            return v != val;
        case CmpOp::kLt:
            return v < val;
        case CmpOp::kGt:
            return v > val;
        case CmpOp::kLe:
            return v <= val;
        case CmpOp::kGe:
            return v >= val;
    }
    return false;
}

auto Filter::evaluate(const std::vector<ColumnData> &columns,
                      const Schema &schema,
                      size_t row_count,
                      const Predicate &pred) -> StatusOr<std::vector<size_t>>
{
    auto col_idx = get_column_index(schema, pred.column.name);
    if (!col_idx)
        return std::unexpected(col_idx.error());

    const auto &col = columns[*col_idx];
    ColumnType col_type = schema.columns[*col_idx];

    auto is_null = [&](size_t i) -> bool {
        if (col.nulls == nullptr)
            return false;
        return (col.nulls[i / 8] >> (i % 8)) & 1;
    };

    std::vector<size_t> result;

    auto get_simd_op = [](CmpOp op) -> simd::Op {
        switch (op) {
            case CmpOp::kEq: return simd::Op::kEq;
            case CmpOp::kGt: return simd::Op::kGt;
            case CmpOp::kLt: return simd::Op::kLt;
            case CmpOp::kGe: return simd::Op::kGe;
            case CmpOp::kLe: return simd::Op::kLe;
            default: return simd::Op::kEq; // Ne is not supported by fast path yet
        }
    };

    if (std::holds_alternative<int64_t>(pred.value.data)) {
        int64_t val = std::get<int64_t>(pred.value.data);
        if (col_type == ColumnType::kInt64) {
            auto *arr = static_cast<const int64_t *>(static_cast<const void *>(col.data));
            if (pred.op != CmpOp::kNe) {
                std::vector<size_t> raw;
                simd::filter(arr, row_count, val, get_simd_op(pred.op), raw);
                for (size_t i : raw) {
                    if (!is_null(i)) result.push_back(i);
                }
            } else {
                for (size_t i = 0; i < row_count; ++i) {
                    if (is_null(i)) continue;
                    if (apply_int(arr[i], val, pred.op)) result.push_back(i);
                }
            }
        }
        else if (col_type == ColumnType::kInt32) {
            auto *arr = static_cast<const int32_t *>(static_cast<const void *>(col.data));
            int32_t val32 = static_cast<int32_t>(val);
            if (pred.op != CmpOp::kNe) {
                std::vector<size_t> raw;
                simd::filter(arr, row_count, val32, get_simd_op(pred.op), raw);
                for (size_t i : raw) {
                    if (!is_null(i)) result.push_back(i);
                }
            } else {
                for (size_t i = 0; i < row_count; ++i) {
                    if (is_null(i)) continue;
                    if (apply_int(arr[i], val, pred.op)) result.push_back(i);
                }
            }
        }
    }
    else if (std::holds_alternative<double>(pred.value.data)) {
        double val = std::get<double>(pred.value.data);
        if (col_type == ColumnType::kFloat64) {
            auto *arr = static_cast<const double *>(static_cast<const void *>(col.data));
            if (pred.op != CmpOp::kNe) {
                std::vector<size_t> raw;
                simd::filter(arr, row_count, val, get_simd_op(pred.op), raw);
                for (size_t i : raw) {
                    if (!is_null(i)) result.push_back(i);
                }
            } else {
                for (size_t i = 0; i < row_count; ++i) {
                    if (is_null(i)) continue;
                    if (apply_double(arr[i], val, pred.op)) result.push_back(i);
                }
            }
        }
    }
    else if (std::holds_alternative<std::string>(pred.value.data)) {
        const auto &val_str = std::get<std::string>(pred.value.data);
        if (col_type == ColumnType::kVarChar) {
            auto *offsets = static_cast<const uint32_t *>(static_cast<const void *>(col.data));
            auto *blob = static_cast<const std::byte *>(col.data) + row_count * sizeof(uint32_t);

            for (size_t i = 0; i < row_count; ++i) {
                if (is_null(i))
                    continue;
                uint32_t start = (i == 0) ? 0 : offsets[i - 1];
                uint32_t end = offsets[i];
                size_t len = end - start;
                std::string_view v(reinterpret_cast<const char *>(blob + start), len);
                if (apply_str(v, val_str, pred.op))
                    result.push_back(i);
            }
        }
    }

    return result;
}

auto Filter::match_count(const std::vector<ColumnData> &columns,
                         const Schema &schema,
                         size_t row_count,
                         const Predicate &pred) -> StatusOr<size_t>
{
    auto r = evaluate(columns, schema, row_count, pred);
    if (!r)
        return std::unexpected(r.error());
    return r->size();
}

} // namespace rawdb
