#ifndef RAWDB_STORAGE_PAGE_HPP
#define RAWDB_STORAGE_PAGE_HPP

#include <cstddef>
#include <cstdint>

#include "core/error.hpp"
#include "core/types.hpp"

namespace rawdb
{

struct ColMeta
{
    ColumnType type;
    size_t data_off;
    size_t data_size;
    size_t nulls_off;
};

struct BatchHeader
{
    TableId table_id;
    size_t row_count;
    size_t col_count;
};

static constexpr uint32_t kPageMagic = 0x52415744; // "RAWD"

enum class PageType : uint8_t
{
    Data = 1,
    Index = 2,
    Metadata = 3
};

struct PageHeader
{
    uint32_t magic;
    uint16_t version;
    PageType type;
    uint8_t reserved;
    uint32_t checksum;
    uint32_t row_count;
    uint32_t data_size;

    static constexpr size_t kSize = 20;
};

static_assert(sizeof(PageHeader) == PageHeader::kSize, "PageHeader must be packed (20 bytes)");

[[nodiscard]] auto compute_page_checksum(const std::byte *data, size_t len) -> uint32_t;

void set_page_checksum(PageHeader &hdr, const std::byte *payload, size_t payload_len);

[[nodiscard]] auto validate_page(const PageHeader &hdr, const std::byte *payload) -> Status;

} // namespace rawdb

#endif // RAWDB_STORAGE_PAGE_HPP
