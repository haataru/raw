#ifndef RAWDB_CORE_CONFIG_HPP
#define RAWDB_CORE_CONFIG_HPP

#include <cstddef>
#include <cstdint>

namespace rawdb
{
namespace config
{

inline constexpr size_t kPageSize = 65536;
inline constexpr size_t kMaxPendingRows = 1'000'000;
inline constexpr uint32_t kSyncIntervalMs = 100;
inline constexpr uint32_t kGcIntervalMs = 1000;
inline constexpr size_t kGcMaxBytesPerSec = 500 * 1024 * 1024;

} // namespace config
} // namespace rawdb

#endif // RAWDB_CORE_CONFIG_HPP
