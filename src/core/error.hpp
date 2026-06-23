#ifndef RAWDB_CORE_ERROR_HPP
#define RAWDB_CORE_ERROR_HPP

#include <expected>
#include <ostream>
#include <string_view>

#include "core/types.hpp"

namespace rawdb
{

template <typename T>
using StatusOr = std::expected<T, Status>;

[[nodiscard]] inline std::string_view status_message(Status::Code c) noexcept
{
    switch (c) {
        case Status::kOk:
            return "ok";
        case Status::kNotFound:
            return "not found";
        case Status::kOutOfMemory:
            return "out of memory";
        case Status::kInvalidArgument:
            return "invalid argument";
        case Status::kCorruptedData:
            return "corrupted data";
        case Status::kIoError:
            return "I/O error";
        case Status::kNotSupported:
            return "not supported";
        case Status::kAlreadyExists:
            return "already exists";
        case Status::kNotVisible:
            return "version not visible";
        case Status::kFatal:
            return "fatal error";
        case Status::kNoSpace:
            return "no space left on device";
    }
    return "unknown";
}

inline std::ostream &operator<<(std::ostream &os, Status s)
{
    if (s.msg.empty()) {
        os << status_message(s.code);
    }
    else {
        os << s.msg;
    }
    return os;
}

} // namespace rawdb

#endif // RAWDB_CORE_ERROR_HPP
