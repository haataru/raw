#include "storage/page.hpp"

#include <gtest/gtest.h>

#include <cstring>

using namespace rawdb;

TEST(PageTest, HeaderSize) { EXPECT_EQ(sizeof(PageHeader), 20); }

TEST(PageTest, CreateHeader)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;
    hdr.type = PageType::Data;
    hdr.row_count = 42;
    hdr.data_size = 100;

    EXPECT_EQ(hdr.magic, kPageMagic);
    EXPECT_EQ(hdr.version, 1);
    EXPECT_EQ(hdr.type, PageType::Data);
    EXPECT_EQ(hdr.row_count, 42);
    EXPECT_EQ(hdr.data_size, 100);
}

TEST(PageTest, ComputeChecksum)
{
    std::byte data[64]{};
    data[0] = std::byte{0xAB};
    data[63] = std::byte{0xCD};

    uint32_t c1 = compute_page_checksum(data, 64);
    uint32_t c2 = compute_page_checksum(data, 64);
    EXPECT_EQ(c1, c2);
}

TEST(PageTest, DifferentDataDifferentChecksum)
{
    std::byte a[64]{};
    std::byte b[64]{};
    b[0] = std::byte{0x01};

    EXPECT_NE(compute_page_checksum(a, 64), compute_page_checksum(b, 64));
}

TEST(PageTest, SetAndValidate)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;
    hdr.type = PageType::Data;
    hdr.row_count = 10;

    std::byte payload[64]{};
    std::memset(payload, 0x42, 64);
    hdr.data_size = 64;

    set_page_checksum(hdr, payload, 64);
    EXPECT_EQ(validate_page(hdr, payload), Status::kOk);
}

TEST(PageTest, CorruptMagic)
{
    PageHeader hdr{};
    hdr.magic = 0xDEADBEEF;
    hdr.version = 1;
    hdr.type = PageType::Data;

    std::byte payload[8]{};
    EXPECT_EQ(validate_page(hdr, payload), Status::kCorruptedData);
}

TEST(PageTest, CorruptPayload)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;
    hdr.type = PageType::Data;

    std::byte payload[64]{};
    std::memset(payload, 0x42, 64);
    hdr.data_size = 64;

    set_page_checksum(hdr, payload, 64);
    EXPECT_EQ(validate_page(hdr, payload), Status::kOk);

    payload[0] = std::byte{0xFF};
    EXPECT_EQ(validate_page(hdr, payload), Status::kCorruptedData);
}

TEST(PageTest, CorruptHeader)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;
    hdr.type = PageType::Data;
    hdr.row_count = 100;

    std::byte payload[64]{};
    hdr.data_size = 64;

    set_page_checksum(hdr, payload, 64);
    EXPECT_EQ(validate_page(hdr, payload), Status::kOk);

    // Corrupt header after checksum was set
    hdr.row_count = 200;
    EXPECT_EQ(validate_page(hdr, payload), Status::kCorruptedData);
}

TEST(PageTest, AllPageTypes)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;

    for (auto type : {PageType::Data, PageType::Index, PageType::Metadata}) {
        hdr.type = type;
        std::byte payload[8]{};
        hdr.data_size = 8;
        set_page_checksum(hdr, payload, 8);
        EXPECT_EQ(validate_page(hdr, payload), Status::kOk);
    }
}

TEST(PageTest, EmptyPayload)
{
    PageHeader hdr{};
    hdr.magic = kPageMagic;
    hdr.version = 1;
    hdr.type = PageType::Data;
    hdr.data_size = 0;

    set_page_checksum(hdr, nullptr, 0);
    EXPECT_EQ(validate_page(hdr, nullptr), Status::kOk);
}
