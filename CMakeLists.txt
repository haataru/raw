cmake_minimum_required(VERSION 3.28)
project(rawdb
    VERSION 0.1.0
    DESCRIPTION "Relational database with MVCC and ClickHouse-style buffer"
    LANGUAGES CXX
)

# ──────────────────────────────────────────────
# Compiler options (global ABI settings +
# target-specific strict flags)
# ──────────────────────────────────────────────
include(cmake/CompilerOptions.cmake)

# ──────────────────────────────────────────────
# Library: rawdb (static)
# ──────────────────────────────────────────────
set(RAWDB_SOURCES
    src/rawdb.cpp
    src/memory/mmap_file.cpp
    src/storage/page.cpp
    src/storage/table.cpp
    src/storage/table_read.cpp
    src/storage/table_vacuum.cpp
    src/buffer/flush_handler.cpp
    src/mvcc/version_index.cpp
    src/mvcc/gc.cpp
    src/db/database.cpp
    src/db/transaction.cpp
    src/recovery/wal.cpp
    src/index/btree.cpp
    src/query/parser.cpp
    src/query/executor.cpp
    src/query/executor_ddl.cpp
    src/query/executor_filter.cpp
    src/query/executor_mutate.cpp
    src/query/executor_scan.cpp
    src/query/executor_select.cpp
    src/core/types.hpp
    src/core/config.hpp
    src/core/error.hpp
)

add_library(rawdb STATIC ${RAWDB_SOURCES})
target_include_directories(rawdb PUBLIC src include)
set_target_properties(rawdb PROPERTIES POSITION_INDEPENDENT_CODE ON)
rawdb_set_strict_options(rawdb)

# ──────────────────────────────────────────────
# Testing
# ──────────────────────────────────────────────
enable_testing()

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

FetchContent_Declare(
    pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v2.12.0.tar.gz
)
set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pybind11)

function(add_rawdb_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE rawdb gtest_main)
    target_include_directories(${name} PRIVATE src)
    rawdb_set_strict_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    # ASAN cross-DSO false positive with system libc++/libc++abi
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=alloc_dealloc_mismatch=0")
endfunction()

function(add_rawdb_bench name source)
    add_executable(${name}_bench ${source})
    target_link_libraries(${name}_bench PRIVATE rawdb benchmark::benchmark)
    target_include_directories(${name}_bench PRIVATE src)
    rawdb_set_strict_options(${name}_bench)
endfunction()

# ──────────────────────────────────────────────
# Unit tests
add_rawdb_test(core_test tests/unit/core_test.cpp)
add_rawdb_test(mmap_file_test tests/unit/mmap_file_test.cpp)
add_rawdb_test(page_test tests/unit/page_test.cpp)
add_rawdb_test(table_test tests/unit/table_test.cpp)
add_rawdb_test(flush_handler_test tests/unit/flush_handler_test.cpp)
add_rawdb_test(version_index_test tests/unit/version_index_test.cpp)
add_rawdb_test(timestamp_test tests/unit/timestamp_test.cpp)
add_rawdb_test(gc_test tests/unit/gc_test.cpp)
add_rawdb_test(database_test tests/unit/database_test.cpp)
add_rawdb_test(parser_test tests/unit/parser_test.cpp)
add_rawdb_test(filter_test tests/unit/filter_test.cpp)
add_rawdb_test(executor_test tests/unit/executor_test.cpp)
add_rawdb_test(capi_test tests/unit/capi_test.cpp)
add_rawdb_test(btree_test tests/unit/btree_test.cpp)
add_rawdb_test(concurrency_test tests/unit/concurrency_test.cpp)

# Benchmarks
add_rawdb_bench(mmap tests/bench/mmap_bench.cpp)
add_rawdb_bench(sql tests/bench/sql_bench.cpp)

add_executable(stress_test tests/bench/stress_test.cpp)
target_link_libraries(stress_test PRIVATE rawdb)

# ──────────────────────────────────────────────
# CLI Executable
# ──────────────────────────────────────────────
add_executable(rawdb_cli 
    src/cli/main.cpp 
    src/cli/repl.cpp 
    src/cli/formatter.cpp
)
target_link_libraries(rawdb_cli PRIVATE rawdb)
target_include_directories(rawdb_cli PRIVATE src)
rawdb_set_strict_options(rawdb_cli)

# ──────────────────────────────────────────────
# Python Bindings
# ──────────────────────────────────────────────
pybind11_add_module(rawdb_ext MODULE python/rawdb_python.cpp)
set_target_properties(rawdb_ext PROPERTIES OUTPUT_NAME "rawdb")
target_link_libraries(rawdb_ext PRIVATE rawdb)
target_include_directories(rawdb_ext PRIVATE src)
rawdb_set_strict_options(rawdb_ext)

# ──────────────────────────────────────────────
# Custom targets
# ──────────────────────────────────────────────
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS rawdb
)

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE RAWDB_SOURCES_FOR_FORMAT
        src/*.cpp src/*.hpp include/*.h tests/*.cpp
    )
    add_custom_target(check-format
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${RAWDB_SOURCES_FOR_FORMAT}
        COMMENT "Checking clang-format compliance"
    )
endif()
