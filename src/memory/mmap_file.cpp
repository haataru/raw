#include "memory/mmap_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rawdb
{

MmapFile::~MmapFile()
{
    if (data_ != nullptr) {
        do_munmap();
    }
    if (fd_ != -1) {
        ::close(fd_);
    }
}

auto MmapFile::operator=(MmapFile &&other) noexcept -> MmapFile &
{
    if (this != &other) {
        if (data_ != nullptr) {
            do_munmap();
        }
        if (fd_ != -1) {
            ::close(fd_);
        }
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
    }
    return *this;
}

void MmapFile::open(const std::filesystem::path &path, size_t size)
{
    if (data_ != nullptr) {
        throw std::runtime_error("MmapFile already open: " + path_.string());
    }

    path_ = path;

    // Always create file if it doesn't exist
    int flags = O_RDWR | O_CREAT;

    fd_ = ::open(path_.c_str(), flags, 0644);
    if (fd_ == -1) {
        throw std::runtime_error("Failed to open file: " + path_.string() + ": " +
                                 std::strerror(errno));
    }

    if (size > 0) {
        if (::ftruncate(fd_, static_cast<off_t>(size)) == -1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Failed to truncate file: " + path_.string() + ": " +
                                     std::strerror(errno));
        }
        size_ = size;
    }
    else {
        struct stat st;
        if (::fstat(fd_, &st) == -1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Failed to stat file: " + path_.string() + ": " +
                                     std::strerror(errno));
        }
        size_ = static_cast<size_t>(st.st_size);
    }

    // mmap — handle zero-size file
    if (size_ == 0) {
        data_ = nullptr;
        return;
    }

    data_ = static_cast<std::byte *>(
        ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));

    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Failed to mmap file: " + path_.string() + ": " +
                                 std::strerror(errno));
    }
}

void MmapFile::close()
{
    if (data_ != nullptr) {
        do_munmap();
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
    data_ = nullptr;
    size_ = 0;
}

auto MmapFile::resize(size_t new_size) -> Status
{
    if (new_size == size_) {
        return Status::kOk;
    }

    do_munmap();

    if (::ftruncate(fd_, static_cast<off_t>(new_size)) == -1) {
        int err = errno;
        // Restore previous mmap
        if (size_ > 0) {
            data_ = static_cast<std::byte *>(
                ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
            if (data_ == MAP_FAILED) {
                data_ = nullptr;
                return {Status::kFatal,
                        "failed to restore mmap after ftruncate error: " +
                            std::string(std::strerror(errno))};
            }
        }
        if (err == ENOSPC) {
            return {Status::kNoSpace, "failed to resize file: no space left on device"};
        }
        return {Status::kIoError, "failed to resize file: " + std::string(std::strerror(err))};
    }

    // Handle zero-size after resize
    if (new_size == 0) {
        data_ = nullptr;
        size_ = 0;
        return Status::kOk;
    }

    data_ = static_cast<std::byte *>(
        ::mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));

    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        return {Status::kIoError, "failed to remap file: " + std::string(std::strerror(errno))};
    }

    size_ = new_size;
    return Status::kOk;
}

auto MmapFile::msync_async() -> Status
{
    if (data_ == nullptr || size_ == 0) {
        return Status::kInvalidArgument;
    }

    int ret = ::msync(data_, size_, MS_ASYNC);
    if (ret == -1) {
        return Status::kIoError;
    }

    return Status::kOk;
}

auto MmapFile::msync_sync() -> Status
{
    if (data_ == nullptr || size_ == 0) {
        return Status::kInvalidArgument;
    }

    int ret = ::msync(data_, size_, MS_SYNC);
    if (ret == -1) {
        return Status::kIoError;
    }

    return Status::kOk;
}

auto MmapFile::msync_sync_safe() -> Status
{
    if (data_ == nullptr || size_ == 0) {
        return Status::kInvalidArgument;
    }

    int ret;
    do {
        ret = ::msync(data_, size_, MS_SYNC);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        return Status::kIoError;
    }

    return Status::kOk;
}

void MmapFile::do_munmap()
{
    if (data_ != nullptr) {
        ::munmap(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }
}

} // namespace rawdb
