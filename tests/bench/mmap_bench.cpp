#include <benchmark/benchmark.h>

#include <cstring>

#include "memory/mmap_file.hpp"

using namespace rawdb;

static void BM_MmapFile_OpenClose(benchmark::State &state)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_bench_mmap.db";
    for (auto _ : state) {
        MmapFile file;
        file.open(path, 4096 * 1024);
        benchmark::DoNotOptimize(file.data());
        file.close();
    }
    std::filesystem::remove(path);
}
BENCHMARK(BM_MmapFile_OpenClose);

static void BM_MmapFile_Read(benchmark::State &state)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_bench_read.db";
    {
        MmapFile file;
        file.open(path, 4096 * 1024);
        std::memset(file.data(), 'A', file.size());
        file.msync_sync();
    }

    MmapFile file;
    file.open(path, 4096 * 1024);
    volatile char sink = 0;
    for (auto _ : state) {
        sink = static_cast<char>(file.data()[0]);
        benchmark::DoNotOptimize(sink);
    }
    file.close();
    std::filesystem::remove(path);
}
BENCHMARK(BM_MmapFile_Read);

static void BM_MmapFile_Write(benchmark::State &state)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_bench_write.db";
    MmapFile file;
    file.open(path, 4096 * 1024);
    for (auto _ : state) {
        std::memset(file.data(), 'B', 64);
        benchmark::ClobberMemory();
    }
    file.close();
    std::filesystem::remove(path);
}
BENCHMARK(BM_MmapFile_Write);

static void BM_MmapFile_Resize(benchmark::State &state)
{
    auto path = std::filesystem::temp_directory_path() / "rawdb_bench_resize.db";
    MmapFile file;
    file.open(path, 4096);
    for (auto _ : state) {
        file.resize(4096);
        benchmark::ClobberMemory();
    }
    file.close();
    std::filesystem::remove(path);
}
BENCHMARK(BM_MmapFile_Resize);

BENCHMARK_MAIN();
