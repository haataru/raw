#include "index/btree.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace rawdb;

struct BTreeTest : ::testing::Test
{
    std::filesystem::path path = std::filesystem::temp_directory_path() / "btree_test";
    void SetUp() override
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    void TearDown() override { std::filesystem::remove_all(path); }
};

TEST_F(BTreeTest, CreateAndOpen)
{
    auto idx_path = path / "test.idx";
    auto tree_r = BTree::create(idx_path, ColumnType::kInt32);
    ASSERT_TRUE(tree_r.has_value());
    EXPECT_TRUE(tree_r->is_open());
    EXPECT_EQ(tree_r->key_type(), ColumnType::kInt32);

    tree_r->close();
    EXPECT_FALSE(tree_r->is_open());

    auto open_r = BTree::open(idx_path);
    ASSERT_TRUE(open_r.has_value());
    EXPECT_TRUE(open_r->is_open());
    EXPECT_EQ(open_r->key_type(), ColumnType::kInt32);
    open_r->close();
}

TEST_F(BTreeTest, InsertAndSearchInt32)
{
    auto idx_path = path / "int32.idx";
    auto tree_r = BTree::create(idx_path, ColumnType::kInt32);
    ASSERT_TRUE(tree_r.has_value());
    auto &tree = *tree_r;

    int32_t keys[] = {10, 20, 30, 20, 40};
    for (size_t i = 0; i < 5; ++i) {
        auto key_bytes = reinterpret_cast<const std::byte *>(&keys[i]);
        ASSERT_EQ(tree.insert(key_bytes, sizeof(int32_t), static_cast<RowId>(i)), Status::kOk);
    }

    int32_t lookup = 20;
    auto key_bytes = reinterpret_cast<const std::byte *>(&lookup);
    auto r = tree.search(key_bytes, sizeof(int32_t));
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2);

    lookup = 10;
    key_bytes = reinterpret_cast<const std::byte *>(&lookup);
    r = tree.search(key_bytes, sizeof(int32_t));
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1);
    EXPECT_EQ((*r)[0], 0);

    lookup = 99;
    key_bytes = reinterpret_cast<const std::byte *>(&lookup);
    r = tree.search(key_bytes, sizeof(int32_t));
    EXPECT_FALSE(r.has_value());

    tree.close();
}

TEST_F(BTreeTest, InsertAndSearchInt64)
{
    auto idx_path = path / "int64.idx";
    auto tree_r = BTree::create(idx_path, ColumnType::kInt64);
    ASSERT_TRUE(tree_r.has_value());
    auto &tree = *tree_r;

    int64_t keys[] = {100, 200, 300};
    for (size_t i = 0; i < 3; ++i) {
        auto key_bytes = reinterpret_cast<const std::byte *>(&keys[i]);
        ASSERT_EQ(tree.insert(key_bytes, sizeof(int64_t), static_cast<RowId>(i)), Status::kOk);
    }

    int64_t lookup = 200;
    auto key_bytes = reinterpret_cast<const std::byte *>(&lookup);
    auto r = tree.search(key_bytes, sizeof(int64_t));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)[0], 1);

    tree.close();
}

TEST_F(BTreeTest, InsertAndSearchVarChar)
{
    auto idx_path = path / "varchar.idx";
    auto tree_r = BTree::create(idx_path, ColumnType::kVarChar);
    ASSERT_TRUE(tree_r.has_value());
    auto &tree = *tree_r;

    auto insert_key = [&](const std::string &s, RowId rid) {
        auto *kb = reinterpret_cast<const std::byte *>(s.data());
        return tree.insert(kb, s.size(), rid);
    };

    ASSERT_EQ(insert_key("alice", 0), Status::kOk);
    ASSERT_EQ(insert_key("bob", 1), Status::kOk);
    ASSERT_EQ(insert_key("alice", 2), Status::kOk);

    auto *kb = reinterpret_cast<const std::byte *>("alice");
    auto r = tree.search(kb, 5);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2);

    kb = reinterpret_cast<const std::byte *>("charlie");
    r = tree.search(kb, 7);
    EXPECT_FALSE(r.has_value());

    tree.close();
}

TEST_F(BTreeTest, Persistence)
{
    auto idx_path = path / "persist.idx";

    {
        auto tree_r = BTree::create(idx_path, ColumnType::kInt32);
        ASSERT_TRUE(tree_r.has_value());
        int32_t keys[] = {5, 3, 8, 1};
        for (size_t i = 0; i < 4; ++i) {
            auto *kp = reinterpret_cast<const std::byte *>(&keys[i]);
            ASSERT_EQ(tree_r->insert(kp, sizeof(int32_t), static_cast<RowId>(i)), Status::kOk);
        }
        tree_r->close();
    }

    {
        auto tree_r = BTree::open(idx_path);
        ASSERT_TRUE(tree_r.has_value());
        EXPECT_EQ(tree_r->key_type(), ColumnType::kInt32);

        int32_t lookup = 3;
        auto *kp = reinterpret_cast<const std::byte *>(&lookup);
        auto r = tree_r->search(kp, sizeof(int32_t));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)[0], 1);

        lookup = 8;
        kp = reinterpret_cast<const std::byte *>(&lookup);
        r = tree_r->search(kp, sizeof(int32_t));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)[0], 2);

        lookup = 99;
        kp = reinterpret_cast<const std::byte *>(&lookup);
        r = tree_r->search(kp, sizeof(int32_t));
        EXPECT_FALSE(r.has_value());

        tree_r->close();
    }
}

TEST_F(BTreeTest, ManyInserts)
{
    auto idx_path = path / "many.idx";
    auto tree_r = BTree::create(idx_path, ColumnType::kInt32);
    ASSERT_TRUE(tree_r.has_value());
    auto &tree = *tree_r;

    for (int i = 0; i < 1000; ++i) {
        int32_t key = i * 2; // even numbers
        auto *kp = reinterpret_cast<const std::byte *>(&key);
        ASSERT_EQ(tree.insert(kp, sizeof(int32_t), static_cast<RowId>(i)), Status::kOk);
    }

    for (int i = 0; i < 1000; ++i) {
        int32_t key = i * 2;
        auto *kp = reinterpret_cast<const std::byte *>(&key);
        auto r = tree.search(kp, sizeof(int32_t));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)[0], static_cast<RowId>(i));
    }

    int32_t lookup = 1;
    auto *kp = reinterpret_cast<const std::byte *>(&lookup);
    auto r = tree.search(kp, sizeof(int32_t));
    EXPECT_FALSE(r.has_value());

    tree.close();
}
