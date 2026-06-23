#include "index/btree.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "core/config.hpp"

namespace rawdb
{

// ── KeyComparator ──

auto KeyComparator::operator()(const std::byte *a,
                               size_t a_len,
                               const std::byte *b,
                               size_t b_len) const -> int
{
    return BTree::compare_keys(a, a_len, b, b_len, type);
}

// ── Static helpers ──

auto BTree::compare_keys(const std::byte *a,
                         size_t a_len,
                         const std::byte *b,
                         size_t b_len,
                         ColumnType type) -> int
{
    switch (type) {
        case ColumnType::kInt32: {
            if (a_len < sizeof(int32_t) || b_len < sizeof(int32_t))
                return 0;
            int32_t va, vb;
            std::memcpy(&va, a, sizeof(va));
            std::memcpy(&vb, b, sizeof(vb));
            return (va > vb) - (va < vb);
        }
        case ColumnType::kTimestamp:
        case ColumnType::kInt64: {
            if (a_len < sizeof(int64_t) || b_len < sizeof(int64_t))
                return 0;
            int64_t va, vb;
            std::memcpy(&va, a, sizeof(va));
            std::memcpy(&vb, b, sizeof(vb));
            return (va > vb) - (va < vb);
        }
        case ColumnType::kFloat64: {
            if (a_len < sizeof(double) || b_len < sizeof(double))
                return 0;
            double va, vb;
            std::memcpy(&va, a, sizeof(va));
            std::memcpy(&vb, b, sizeof(vb));
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case ColumnType::kBool: {
            bool ba = a_len > 0 && a[0] != std::byte{0};
            bool bb = b_len > 0 && b[0] != std::byte{0};
            return static_cast<int>(ba) - static_cast<int>(bb);
        }
        case ColumnType::kVarChar: {
            auto real_len = [](const std::byte *p, size_t max) -> size_t {
                const auto *start = reinterpret_cast<const char *>(p);
                const auto *zero = static_cast<const char *>(std::memchr(start, 0, max));
                return zero ? static_cast<size_t>(zero - start) : max;
            };
            size_t a_sz = real_len(a, a_len);
            size_t b_sz = real_len(b, b_len);
            int r = std::memcmp(a, b, std::min(a_sz, b_sz));
            if (r != 0)
                return r;
            if (a_sz < b_sz)
                return -1;
            if (a_sz > b_sz)
                return 1;
            return 0;
        }
    }
    return 0;
}

// ── Layout ──

void BTree::compute_layout()
{
    switch (key_type_) {
        case ColumnType::kInt32:
            slot_key_size_ = 4;
            break;
        case ColumnType::kTimestamp:
        case ColumnType::kInt64:
            slot_key_size_ = 8;
            break;
        case ColumnType::kFloat64:
            slot_key_size_ = 8;
            break;
        case ColumnType::kBool:
            slot_key_size_ = 1;
            break;
        case ColumnType::kVarChar:
            slot_key_size_ = kMaxSlotKeySize;
            break;
    }
    node_size_ = kNodeHdr + sizeof(PageId) + kMaxKeys * (slot_key_size_ + sizeof(uint64_t));
}

// ── Node reader ──

auto BTree::read_node(PageId off) const -> StatusOr<NodeReader>
{
    if (off + node_size_ > file_.size()) {
        return std::unexpected(Status::kCorruptedData);
    }
    NodeReader r;
    r.base = file_.data();
    r.file_size = file_.size();
    r.node_off = off;
    r.slot_key_size = slot_key_size_;
    const auto *hdr = r.base + off;
    std::memcpy(&r.num_entries, hdr, sizeof(r.num_entries));
    r.is_leaf = (hdr[4] != std::byte{0});
    std::memcpy(&r.first_child, hdr + kNodeHdr, sizeof(r.first_child));
    return r;
}

// ── Binary search helpers ──

auto BTree::search_key_binary(const NodeReader &nr,
                              const std::byte *key,
                              size_t key_len,
                              const KeyComparator &cmp) -> int
{
    if (nr.num_entries == 0)
        return -1;
    uint32_t lo = 0, hi = nr.num_entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, key_len, nr.key_ptr(mid), nr.slot_key_size);
        if (c == 0) {
            while (mid > 0 && cmp(key, key_len, nr.key_ptr(mid - 1), nr.slot_key_size) == 0)
                --mid;
            return static_cast<int>(mid);
        }
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return -1;
}

auto BTree::find_child_binary(const NodeReader &nr,
                              const std::byte *key,
                              size_t key_len,
                              const KeyComparator &cmp) -> uint32_t
{
    uint32_t lo = 0, hi = nr.num_entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cmp(key, key_len, nr.key_ptr(mid), nr.slot_key_size) < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

// ── Create / Open ──

auto BTree::create(const std::filesystem::path &path, ColumnType key_type) -> StatusOr<BTree>
{
    BTree tree;
    tree.key_type_ = key_type;
    tree.compute_layout();
    tree.root_off_ = kHeaderSize;
    try {
        tree.file_.open(path, tree.root_off_ + tree.node_size_);
        std::memcpy(tree.file_.data(), &tree.root_off_, sizeof(tree.root_off_));
        uint64_t kt = static_cast<uint64_t>(key_type);
        std::memcpy(tree.file_.data() + 8, &kt, sizeof(kt));

        auto *node_start = tree.file_.data() + tree.root_off_;
        std::memset(node_start, 0, tree.node_size_);
        uint32_t zero = 0;
        std::memcpy(node_start, &zero, sizeof(zero));
        node_start[4] = std::byte{1};
    }
    catch (...) {
        return std::unexpected(Status::kIoError);
    }
    return tree;
}

auto BTree::open(const std::filesystem::path &path) -> StatusOr<BTree>
{
    BTree tree;
    try {
        tree.file_.open(path);
        if (tree.file_.size() < kHeaderSize) {
            return std::unexpected(Status::kCorruptedData);
        }
        std::memcpy(&tree.root_off_, tree.file_.data(), sizeof(tree.root_off_));
        uint64_t kt;
        std::memcpy(&kt, tree.file_.data() + 8, sizeof(kt));
        tree.key_type_ = static_cast<ColumnType>(kt);
        tree.compute_layout();
    }
    catch (...) {
        return std::unexpected(Status::kIoError);
    }
    return tree;
}

void BTree::close()
{
    if (file_.is_open()) {
        if (file_.size() >= kHeaderSize) {
            std::memcpy(file_.data(), &root_off_, sizeof(root_off_));
        }
        file_.close();
    }
}

// ── Search ──

auto BTree::search(const std::byte *key, size_t key_len) const -> StatusOr<std::vector<RowId>>
{
    std::shared_lock lock(*rw_mutex_);
    if (!file_.is_open() || root_off_ == 0) {
        return std::unexpected(Status::kNotFound);
    }
    KeyComparator cmp{key_type_};
    PageId off = root_off_;
    while (true) {
        auto nr_r = read_node(off);
        if (!nr_r)
            return std::unexpected(nr_r.error());
        auto &nr = *nr_r;
        if (nr.is_leaf) {
            int idx = search_key_binary(nr, key, key_len, cmp);
            if (idx < 0)
                return std::unexpected(Status::kNotFound);
            std::vector<RowId> results;
            int n = static_cast<int>(nr.num_entries);
            while (idx < n &&
                   cmp(key, key_len, nr.key_ptr(static_cast<uint32_t>(idx)), nr.slot_key_size) ==
                       0) {
                results.push_back(*nr.value_ptr(static_cast<uint32_t>(idx)));
                ++idx;
            }
            if (results.empty())
                return std::unexpected(Status::kNotFound);
            return results;
        }
        uint32_t ci = find_child_binary(nr, key, key_len, cmp);
        if (ci == 0) {
            off = static_cast<PageId>(nr.first_child);
        }
        else {
            off = static_cast<PageId>(*nr.value_ptr(ci - 1));
        }
    }
}

// ── Insert ──

auto BTree::insert(const std::byte *key, size_t key_len, RowId row_id) -> Status
{
    std::unique_lock lock(*rw_mutex_);
    KeyComparator cmp{key_type_};
    std::vector<std::vector<std::byte>> split_keys;
    std::vector<PageId> split_pages;

    auto st = insert_into_subtree(root_off_, key, key_len, row_id, cmp, split_keys, split_pages);
    if (st != Status::kOk)
        return st;
    if (split_keys.empty())
        return Status::kOk;

    auto sep = std::move(split_keys.back());
    PageId new_node_page = split_pages.back();
    split_keys.pop_back();
    split_pages.pop_back();

    PageId new_root_off = static_cast<PageId>(file_.size());
    size_t new_root_sz = node_size_;
    if (auto s = file_.resize(new_root_off + new_root_sz); s.code != Status::kOk) {
        return s;
    }

    auto *node = file_.data() + new_root_off;
    std::memset(node, 0, new_root_sz);
    uint32_t one = 1;
    std::memcpy(node, &one, sizeof(one));
    PageId old_root = root_off_;
    std::memcpy(node + kNodeHdr, &old_root, sizeof(old_root));

    size_t copy_len = std::min(sep.size(), slot_key_size_);
    std::memcpy(node + kNodeHdr + sizeof(PageId), sep.data(), copy_len);
    std::memcpy(node + kNodeHdr + sizeof(PageId) + kMaxKeys * slot_key_size_,
                &new_node_page,
                sizeof(new_node_page));

    root_off_ = new_root_off;
    std::memcpy(file_.data(), &root_off_, sizeof(root_off_));
    return Status::kOk;
}

auto BTree::insert_into_subtree(PageId node_off,
                                const std::byte *key,
                                size_t key_len,
                                RowId row_id,
                                const KeyComparator &cmp,
                                std::vector<std::vector<std::byte>> &split_keys,
                                std::vector<PageId> &split_pages) -> Status
{
    auto nr_r = read_node(node_off);
    if (!nr_r)
        return nr_r.error();
    auto &nr = *nr_r;

    if (nr.is_leaf) {
        return insert_into_leaf_node(nr, key, key_len, row_id, cmp, split_keys, split_pages);
    }

    uint32_t ci = find_child_binary(nr, key, key_len, cmp);
    PageId child_off;
    if (ci == 0) {
        child_off = static_cast<PageId>(nr.first_child);
    }
    else {
        child_off = static_cast<PageId>(*nr.value_ptr(ci - 1));
    }

    auto st = insert_into_subtree(child_off, key, key_len, row_id, cmp, split_keys, split_pages);
    if (st != Status::kOk)
        return st;
    if (split_keys.empty())
        return Status::kOk;

    auto sep_key = std::move(split_keys.back());
    split_keys.pop_back();
    PageId new_page = split_pages.back();
    split_pages.pop_back();

    std::array<std::byte,
               kNodeHdr + sizeof(PageId) + kMaxKeys *(kMaxSlotKeySize + sizeof(uint64_t))>
        buf;
    std::memset(buf.data(), 0, buf.size());
    std::memcpy(buf.data(), file_.data() + node_off, node_size_);

    uint32_t num = 0;
    std::memcpy(&num, buf.data(), sizeof(num));

    if (num < kMaxKeys) {
        size_t ks = slot_key_size_;
        std::byte *keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);

        if (ci < num) {
            std::memmove(keys + (ci + 1) * ks, keys + ci * ks, (num - ci) * ks);
            std::memmove(vals + (ci + 1), vals + ci, (num - ci) * sizeof(uint64_t));
        }
        size_t copy_len = std::min(sep_key.size(), ks);
        std::memcpy(keys + ci * ks, sep_key.data(), copy_len);
        vals[ci] = new_page;
        num++;
        std::memcpy(buf.data(), &num, sizeof(num));
        std::memcpy(file_.data() + node_off, buf.data(), node_size_);
        return Status::kOk;
    }

    size_t ks = slot_key_size_;
    uint32_t total = num + 1;
    auto combined_keys = std::make_unique<std::byte[]>(total * ks);
    auto combined_vals = std::make_unique<uint64_t[]>(total);
    {
        const std::byte *src_keys = buf.data() + kNodeHdr + sizeof(PageId);
        const auto *src_vals = reinterpret_cast<const uint64_t *>(buf.data() + kNodeHdr +
                                                                  sizeof(PageId) + kMaxKeys * ks);
        if (ci > 0) {
            std::memcpy(combined_keys.get(), src_keys, ci * ks);
            std::memcpy(combined_vals.get(), src_vals, ci * sizeof(uint64_t));
        }
        size_t copy_len = std::min(sep_key.size(), ks);
        std::memcpy(combined_keys.get() + ci * ks, sep_key.data(), copy_len);
        combined_vals[ci] = new_page;
        if (ci < num) {
            std::memcpy(combined_keys.get() + (ci + 1) * ks, src_keys + ci * ks, (num - ci) * ks);
            std::memcpy(combined_vals.get() + ci + 1, src_vals + ci, (num - ci) * sizeof(uint64_t));
        }
    }

    size_t half = total / 2;

    {
        std::memset(buf.data(), 0, node_size_);
        uint32_t n_left = static_cast<uint32_t>(half);
        std::memcpy(buf.data(), &n_left, sizeof(n_left));
        std::memcpy(buf.data() + kNodeHdr, &nr.first_child, sizeof(nr.first_child));
        std::byte *dst_keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *dst_vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);
        std::memcpy(dst_keys, combined_keys.get(), half * ks);
        std::memcpy(dst_vals, combined_vals.get(), half * sizeof(uint64_t));
        std::memcpy(file_.data() + node_off, buf.data(), node_size_);
    }

    {
        PageId new_off = static_cast<PageId>(file_.size());
        try {
            file_.resize(new_off + node_size_);
        }
        catch (...) {
            return Status::kIoError;
        }
        std::memset(buf.data(), 0, node_size_);
        uint32_t n_right = static_cast<uint32_t>(total - half - 1);
        std::memcpy(buf.data(), &n_right, sizeof(n_right));
        uint64_t new_first = combined_vals[half];
        std::memcpy(buf.data() + kNodeHdr, &new_first, sizeof(new_first));
        std::byte *dst_keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *dst_vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);
        if (n_right > 0) {
            std::memcpy(dst_keys, combined_keys.get() + (half + 1) * ks, n_right * ks);
            std::memcpy(dst_vals, combined_vals.get() + half + 1, n_right * sizeof(uint64_t));
        }
        std::memcpy(file_.data() + new_off, buf.data(), node_size_);
        split_keys.push_back(std::vector<std::byte>(combined_keys.get() + half * ks,
                                                    combined_keys.get() + half * ks + ks));
        split_pages.push_back(new_off);
    }
    return Status::kOk;
}

auto BTree::insert_into_leaf_node(const NodeReader &nr,
                                  const std::byte *key,
                                  size_t key_len,
                                  RowId row_id,
                                  const KeyComparator &cmp,
                                  std::vector<std::vector<std::byte>> &split_keys,
                                  std::vector<PageId> &split_pages) -> Status
{
    size_t ks = slot_key_size_;
    constexpr size_t kBufSize =
        kNodeHdr + sizeof(PageId) + kMaxKeys * (kMaxSlotKeySize + sizeof(uint64_t));
    std::array<std::byte, kBufSize> buf;
    std::memset(buf.data(), 0, buf.size());
    std::memcpy(buf.data(), file_.data() + nr.node_off, node_size_);

    uint32_t num = 0;
    std::memcpy(&num, buf.data(), sizeof(num));

    uint32_t ipos = 0;
    {
        uint32_t lo = 0, hi = num;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const std::byte *ek = buf.data() + kNodeHdr + sizeof(PageId) + mid * ks;
            int c = cmp(key, key_len, ek, ks);
            if (c <= 0)
                hi = mid;
            else
                lo = mid + 1;
        }
        ipos = lo;
    }

    if (num < kMaxKeys) {
        std::byte *keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);

        if (ipos < num) {
            std::memmove(keys + (ipos + 1) * ks, keys + ipos * ks, (num - ipos) * ks);
            std::memmove(vals + (ipos + 1), vals + ipos, (num - ipos) * sizeof(uint64_t));
        }
        size_t copy_len = std::min(key_len, ks);
        std::memcpy(keys + ipos * ks, key, copy_len);
        vals[ipos] = row_id;
        num++;
        std::memcpy(buf.data(), &num, sizeof(num));
        std::memcpy(file_.data() + nr.node_off, buf.data(), node_size_);
        return Status::kOk;
    }

    uint32_t total = num + 1;
    auto combined_keys = std::make_unique<std::byte[]>(total * ks);
    auto combined_vals = std::make_unique<uint64_t[]>(total);

    const std::byte *src_keys = buf.data() + kNodeHdr + sizeof(PageId);
    const auto *src_vals =
        reinterpret_cast<const uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);

    if (ipos > 0) {
        std::memcpy(combined_keys.get(), src_keys, ipos * ks);
        std::memcpy(combined_vals.get(), src_vals, ipos * sizeof(uint64_t));
    }
    size_t copy_len = std::min(key_len, ks);
    std::memcpy(combined_keys.get() + ipos * ks, key, copy_len);
    combined_vals[ipos] = row_id;
    if (ipos < num) {
        std::memcpy(combined_keys.get() + (ipos + 1) * ks, src_keys + ipos * ks, (num - ipos) * ks);
        std::memcpy(combined_vals.get() + ipos + 1,
                    src_vals + ipos,
                    (num - ipos) * sizeof(uint64_t));
    }

    size_t half = total / 2;

    {
        std::memset(buf.data(), 0, node_size_);
        buf.data()[4] = std::byte{1};
        uint32_t n_left = static_cast<uint32_t>(half);
        std::memcpy(buf.data(), &n_left, sizeof(n_left));
        std::byte *dst_keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *dst_vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);
        std::memcpy(dst_keys, combined_keys.get(), half * ks);
        std::memcpy(dst_vals, combined_vals.get(), half * sizeof(uint64_t));
        std::memcpy(file_.data() + nr.node_off, buf.data(), node_size_);
    }

    {
        PageId new_off = static_cast<PageId>(file_.size());
        try {
            file_.resize(new_off + node_size_);
        }
        catch (...) {
            return Status::kIoError;
        }
        std::memset(buf.data(), 0, node_size_);
        buf.data()[4] = std::byte{1};
        uint32_t n_right = static_cast<uint32_t>(total - half);
        std::memcpy(buf.data(), &n_right, sizeof(n_right));
        std::byte *dst_keys = buf.data() + kNodeHdr + sizeof(PageId);
        auto *dst_vals =
            reinterpret_cast<uint64_t *>(buf.data() + kNodeHdr + sizeof(PageId) + kMaxKeys * ks);
        std::memcpy(dst_keys, combined_keys.get() + half * ks, n_right * ks);
        std::memcpy(dst_vals, combined_vals.get() + half, n_right * sizeof(uint64_t));
        std::memcpy(file_.data() + new_off, buf.data(), node_size_);
        split_keys.push_back(std::vector<std::byte>(combined_keys.get() + half * ks,
                                                    combined_keys.get() + half * ks + ks));
        split_pages.push_back(new_off);
    }
    return Status::kOk;
}

} // namespace rawdb
