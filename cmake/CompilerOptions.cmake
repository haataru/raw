# ──────────────────────────────────────────────
# rawDB Compiler Options
# ──────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_LINKER_TYPE LLD)

# ──────────────────────────────────────────────
# Global flags — only essential ABI settings
# (applied to ALL targets including FetchContent)
# ──────────────────────────────────────────────
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++ -lc++abi -fuse-ld=lld")

# Debug flags
string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O0 -g -fno-omit-frame-pointer")
string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG " -fsanitize=address,undefined,leak")

# Release flags
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O2 -DNDEBUG")

# ──────────────────────────────────────────────
# Strict flags — only for our own targets
# ──────────────────────────────────────────────
set(RAWDB_CXX_STRICT_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wunused
    -Wuninitialized
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)

function(rawdb_set_strict_options target)
    target_compile_options(${target} PRIVATE ${RAWDB_CXX_STRICT_FLAGS})
endfunction()
