#ifndef RAWDB_MEMORY_MMAP_FILE_HPP
#define RAWDB_MEMORY_MMAP_FILE_HPP

#include <cstddef>
#include <filesystem>
#include <string>

#include "core/error.hpp"

namespace rawdb
{

class MmapFile
{
public:
    MmapFile() = default;

    ~MmapFile();

    MmapFile(const MmapFile &) = delete;
    auto operator=(const MmapFile &) -> MmapFile & = delete;

    MmapFile(MmapFile &&other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          fd_(std::exchange(other.fd_, -1))
    {}

    auto operator=(MmapFile &&other) noexcept -> MmapFile &;

    void open(const std::filesystem::path &path, size_t size = 0);

    void close();

    [[nodiscard]] auto data() const -> std::byte * { return data_; }

    [[nodiscard]] auto size() const -> size_t { return size_; }

    [[nodiscard]] auto path() const -> const std::filesystem::path & { return path_; }

    [[nodiscard]] auto is_open() const -> bool { return fd_ != -1; }

    auto resize(size_t new_size) -> Status;

    auto msync_async() -> Status;

    auto msync_sync() -> Status;

    auto msync_sync_safe() -> Status;

private:
    std::byte *data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
    std::filesystem::path path_;

    void do_munmap();
};

} // namespace rawdb

#endif // RAWDB_MEMORY_MMAP_FILE_HPP
