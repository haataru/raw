#ifndef RAWDB_INDEX_BTREE_HPP
#define RAWDB_INDEX_BTREE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"
#include "memory/mmap_file.hpp"

namespace rawdb
{

static constexpr size_t kBTreeOrder = 128;

struct KeyComparator
{
    ColumnType type;
    auto operator()(const std::byte *a,
                    size_t a_len,
                    const std::byte *b,
                    size_t b_len) const -> int;
};

class BTree
{
public:
    static constexpr size_t kHeaderSize = 16;
    static constexpr size_t kNodeHdr = 8;
    static constexpr size_t kMaxKeys = 31;
    static constexpr size_t kMaxSlotKeySize = 256;

    BTree() = default;
    BTree(BTree &&) = default;
    auto operator=(BTree &&) -> BTree & = default;
    BTree(const BTree &) = delete;
    auto operator=(const BTree &) -> BTree & = delete;

    static auto create(const std::filesystem::path &path, ColumnType key_type) -> StatusOr<BTree>;
    static auto open(const std::filesystem::path &path) -> StatusOr<BTree>;

    void close();
    [[nodiscard]] auto is_open() const -> bool { return file_.is_open(); }
    [[nodiscard]] auto key_type() const -> ColumnType { return key_type_; }
    [[nodiscard]] auto search(const std::byte *key,
                              size_t key_len) const -> StatusOr<std::vector<RowId>>;
    auto insert(const std::byte *key, size_t key_len, RowId row_id) -> Status;

    static auto compare_keys(const std::byte *a,
                             size_t a_len,
                             const std::byte *b,
                             size_t b_len,
                             ColumnType type) -> int;

private:
    MmapFile file_;
    ColumnType key_type_{ColumnType::kInt32};
    PageId root_off_{0};
    size_t slot_key_size_{0};
    size_t node_size_{0};
    mutable std::unique_ptr<std::shared_mutex> rw_mutex_{std::make_unique<std::shared_mutex>()};

    void compute_layout();

    struct NodeReader
    {
        const std::byte *base;
        size_t file_size;
        PageId node_off;
        size_t slot_key_size;
        bool is_leaf;
        uint32_t num_entries;
        uint64_t first_child;

        auto key_ptr(uint32_t idx) const -> const std::byte *
        {
            return base + node_off + kNodeHdr + sizeof(PageId) + idx * slot_key_size;
        }
        auto value_ptr(uint32_t idx) const -> const uint64_t *
        {
            return reinterpret_cast<const uint64_t *>(base + node_off + kNodeHdr + sizeof(PageId) +
                                                      kMaxKeys * slot_key_size +
                                                      idx * sizeof(uint64_t));
        }
    };

    auto read_node(PageId off) const -> StatusOr<NodeReader>;

    static auto search_key_binary(const NodeReader &nr,
                                  const std::byte *key,
                                  size_t key_len,
                                  const KeyComparator &cmp) -> int;
    static auto find_child_binary(const NodeReader &nr,
                                  const std::byte *key,
                                  size_t key_len,
                                  const KeyComparator &cmp) -> uint32_t;

    auto insert_into_subtree(PageId node_off,
                             const std::byte *key,
                             size_t key_len,
                             RowId row_id,
                             const KeyComparator &cmp,
                             std::vector<std::vector<std::byte>> &split_keys,
                             std::vector<PageId> &split_pages) -> Status;

    auto insert_into_leaf_node(const NodeReader &nr,
                               const std::byte *key,
                               size_t key_len,
                               RowId row_id,
                               const KeyComparator &cmp,
                               std::vector<std::vector<std::byte>> &split_keys,
                               std::vector<PageId> &split_pages) -> Status;
};

struct IndexInfo
{
    std::string name;
    std::string column_name;
    size_t column_idx;
    ColumnType column_type;
    BTree tree;
};

} // namespace rawdb

#endif // RAWDB_INDEX_BTREE_HPP
