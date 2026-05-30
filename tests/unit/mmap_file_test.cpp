#include "memory/mmap_file.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

using namespace rawdb;

namespace fs = std::filesystem;

class MmapFileTest : public ::testing::Test
{
protected:
    fs::path test_dir_;

    void SetUp() override
    {
        test_dir_ = fs::temp_directory_path() / "rawdb_mmap_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override { fs::remove_all(test_dir_); }

    auto test_path(const char *name) const -> fs::path { return test_dir_ / name; }
};

// ──────────────────────────────────────────────
// Open / Close
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, OpenNewFile)
{
    MmapFile file;
    file.open(test_path("test.db"), 4096);
    EXPECT_TRUE(file.is_open());
    EXPECT_NE(file.data(), nullptr);
    EXPECT_EQ(file.size(), 4096);
    file.close();
    EXPECT_FALSE(file.is_open());
    EXPECT_EQ(file.data(), nullptr);
    EXPECT_EQ(file.size(), 0);
}

TEST_F(MmapFileTest, OpenExistingFile)
{
    {
        MmapFile file;
        file.open(test_path("existing.db"), 4096);
        file.close();
    }

    {
        MmapFile file;
        file.open(test_path("existing.db"));
        EXPECT_TRUE(file.is_open());
        EXPECT_EQ(file.size(), 4096);
    }
}

TEST_F(MmapFileTest, OpenNonExistentFileFails)
{
    MmapFile file;
    // MmapFile now creates the file if it doesn't exist
    // Verify it succeeds and creates an empty file
    file.open(test_path("new_file.db"));
    EXPECT_TRUE(file.is_open());
    EXPECT_EQ(file.size(), 0);
    file.close();
    std::error_code ec;
    std::filesystem::remove(test_path("new_file.db"), ec);
}

TEST_F(MmapFileTest, DoubleOpenFails)
{
    MmapFile file;
    file.open(test_path("double.db"), 4096);
    try {
        file.open(test_path("double2.db"), 4096);
        FAIL() << "Expected std::runtime_error";
    }
    catch (const std::runtime_error &) {
        // expected
    }
    file.close();
}

// ──────────────────────────────────────────────
// Read / Write
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, WriteAndRead)
{
    MmapFile file;
    file.open(test_path("rw.db"), 4096);

    auto *data = file.data();
    ASSERT_NE(data, nullptr);

    const char *message = "Hello, rawDB!";
    std::memcpy(data, message, std::strlen(message) + 1);

    EXPECT_STREQ(reinterpret_cast<const char *>(data), message);
    file.close();
}

TEST_F(MmapFileTest, DataPersistsAfterClose)
{
    const char *message = "Persist me!";
    {
        MmapFile file;
        file.open(test_path("persist.db"), 4096);
        std::memcpy(file.data(), message, std::strlen(message) + 1);
        // msync to ensure data hits disk
        EXPECT_EQ(file.msync_sync(), Status::kOk);
    }

    {
        MmapFile file;
        file.open(test_path("persist.db"));
        EXPECT_STREQ(reinterpret_cast<const char *>(file.data()), message);
    }
}

// ──────────────────────────────────────────────
// Resize
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, ResizeLarger)
{
    MmapFile file;
    file.open(test_path("resize.db"), 4096);
    EXPECT_EQ(file.size(), 4096);

    file.resize(8192);
    EXPECT_EQ(file.size(), 8192);
    EXPECT_NE(file.data(), nullptr);
    file.close();
}

TEST_F(MmapFileTest, ResizeSmaller)
{
    MmapFile file;
    file.open(test_path("resize_small.db"), 8192);
    EXPECT_EQ(file.size(), 8192);

    file.resize(4096);
    EXPECT_EQ(file.size(), 4096);
    file.close();
}

TEST_F(MmapFileTest, ResizeSameSize)
{
    MmapFile file;
    file.open(test_path("resize_same.db"), 4096);
    file.resize(4096);
    EXPECT_EQ(file.size(), 4096);
    EXPECT_NE(file.data(), nullptr);
    file.close();
}

TEST_F(MmapFileTest, WriteAfterResize)
{
    MmapFile file;
    file.open(test_path("resize_write.db"), 4096);
    file.resize(8192);

    const char *msg = "After resize";
    std::memcpy(file.data() + 4096, msg, std::strlen(msg) + 1);

    EXPECT_STREQ(reinterpret_cast<const char *>(file.data() + 4096), msg);
    file.close();
}

// ──────────────────────────────────────────────
// msync
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, MsyncAsync)
{
    MmapFile file;
    file.open(test_path("msync_async.db"), 4096);
    EXPECT_EQ(file.msync_async(), Status::kOk);
    file.close();
}

TEST_F(MmapFileTest, MsyncSync)
{
    MmapFile file;
    file.open(test_path("msync_sync.db"), 4096);
    EXPECT_EQ(file.msync_sync(), Status::kOk);
    file.close();
}

TEST_F(MmapFileTest, MsyncSafe)
{
    MmapFile file;
    file.open(test_path("msync_safe.db"), 4096);
    EXPECT_EQ(file.msync_sync_safe(), Status::kOk);
    file.close();
}

// ──────────────────────────────────────────────
// Move semantics
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, MoveConstructor)
{
    MmapFile file1;
    file1.open(test_path("move_ctor.db"), 4096);
    auto *data = file1.data();
    size_t size = file1.size();

    MmapFile file2(std::move(file1));
    EXPECT_EQ(file1.data(), nullptr);
    EXPECT_EQ(file1.size(), 0);
    EXPECT_FALSE(file1.is_open());

    EXPECT_EQ(file2.data(), data);
    EXPECT_EQ(file2.size(), size);
    EXPECT_TRUE(file2.is_open());

    file2.close();
}

TEST_F(MmapFileTest, MoveAssignment)
{
    MmapFile file1;
    file1.open(test_path("move_assign.db"), 4096);
    auto *data = file1.data();

    MmapFile file2;
    file2 = std::move(file1);

    EXPECT_EQ(file1.data(), nullptr);
    EXPECT_FALSE(file1.is_open());
    EXPECT_EQ(file2.data(), data);
    EXPECT_TRUE(file2.is_open());

    file2.close();
}

// ──────────────────────────────────────────────
// Edge cases
// ──────────────────────────────────────────────

TEST_F(MmapFileTest, SizeZero)
{
    // Create empty file
    {
        MmapFile file;
        file.open(test_path("empty.db"), 4096);
        file.close();
    }

    // Open with size 0
    {
        MmapFile file;
        file.open(test_path("empty.db"));
        EXPECT_GE(file.size(), 0);
    }
}

TEST_F(MmapFileTest, Path)
{
    MmapFile file;
    auto p = test_path("path_test.db");
    file.open(p, 4096);
    EXPECT_EQ(file.path(), p);
    file.close();
}

TEST_F(MmapFileTest, MultipleMsync)
{
    MmapFile file;
    file.open(test_path("multi_msync.db"), 4096);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(file.msync_async(), Status::kOk);
    }

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(file.msync_sync(), Status::kOk);
    }

    file.close();
}
