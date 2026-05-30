#ifndef RAWDB_CORE_TYPES_HPP
#define RAWDB_CORE_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rawdb
{

using RowId = uint64_t;
using Timestamp = uint64_t;
using TxId = uint64_t;
using PageId = uint64_t;
using ColumnId = uint16_t;
using TableId = uint32_t;

enum class ColumnType : uint8_t
{
    kInt32 = 0,
    kInt64 = 1,
    kFloat64 = 2,
    kBool = 3,
    kVarChar = 4,
};

struct ColumnData
{
    ColumnType type;
    const std::byte *data;
    size_t size;
    const uint8_t *nulls;
};

struct Status
{
    enum Code : uint8_t
    {
        kOk = 0,
        kNotFound = 1,
        kOutOfMemory = 2,
        kInvalidArgument = 3,
        kCorruptedData = 4,
        kIoError = 5,
        kNotSupported = 6,
        kAlreadyExists = 7,
        kNotVisible = 8,
        kFatal = 9,
    };

    Code code;
    std::string msg;

    Status() noexcept : code(kOk) {}
    // NOLINTNEXTLINE(google-explicit-constructor) — implicit for enum-compatible API
    Status(Code c, std::string m = "") : code(c), msg(std::move(m)) {}

    friend auto operator==(Status s, Code c) noexcept -> bool { return s.code == c; }
    friend auto operator!=(Status s, Code c) noexcept -> bool { return s.code != c; }
    friend auto operator==(Code c, Status s) noexcept -> bool { return s.code == c; }
    friend auto operator!=(Code c, Status s) noexcept -> bool { return s.code != c; }
    friend auto operator==(Status a, Status b) noexcept -> bool { return a.code == b.code; }
    friend auto operator!=(Status a, Status b) noexcept -> bool { return a.code != b.code; }
};

} // namespace rawdb

#endif // RAWDB_CORE_TYPES_HPP
