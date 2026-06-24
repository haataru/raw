#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace rawdb::simd
{

enum class Op
{
    kEq,
    kGt,
    kLt,
    kGe,
    kLe
};

// ──────────────────────────────────────────────
// Scalar Implementation (Fallback)
// ──────────────────────────────────────────────
template <typename T>
void filter_scalar(const T *data, size_t count, T value, Op op, std::vector<size_t> &matching)
{
    for (size_t i = 0; i < count; ++i) {
        bool match = false;
        switch (op) {
            case Op::kEq:
                match = (data[i] == value);
                break;
            case Op::kGt:
                match = (data[i] > value);
                break;
            case Op::kLt:
                match = (data[i] < value);
                break;
            case Op::kGe:
                match = (data[i] >= value);
                break;
            case Op::kLe:
                match = (data[i] <= value);
                break;
        }
        if (match)
            matching.push_back(i);
    }
}

// ──────────────────────────────────────────────
// AVX2 Implementation
// ──────────────────────────────────────────────
template <typename T>
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("avx2")))
#endif
void filter_avx2(const T* data, size_t count, T value, Op op, std::vector<size_t>& matching)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    size_t i = 0;

    if constexpr (std::is_same_v<T, int32_t>) {
        __m256i val_vec = _mm256_set1_epi32(value);
        for (; i + 7 < count; i += 8) {
            __m256i data_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            __m256i cmp_res;
            switch (op) {
                case Op::kEq:
                    cmp_res = _mm256_cmpeq_epi32(data_vec, val_vec);
                    break;
                case Op::kGt:
                    cmp_res = _mm256_cmpgt_epi32(data_vec, val_vec);
                    break;
                case Op::kLt:
                    cmp_res = _mm256_cmpgt_epi32(val_vec, data_vec);
                    break;
                case Op::kGe: {
                    __m256i gt = _mm256_cmpgt_epi32(val_vec, data_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
                case Op::kLe: {
                    __m256i gt = _mm256_cmpgt_epi32(data_vec, val_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
            }
            int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp_res));
            if (mask) {
                for (int bit = 0; bit < 8; ++bit) {
                    if ((mask >> bit) & 1)
                        matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }
    else if constexpr (std::is_same_v<T, int64_t>) {
        __m256i val_vec = _mm256_set1_epi64x(value);
        for (; i + 3 < count; i += 4) {
            __m256i data_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            __m256i cmp_res;
            switch (op) {
                case Op::kEq:
                    cmp_res = _mm256_cmpeq_epi64(data_vec, val_vec);
                    break;
                case Op::kGt:
                    cmp_res = _mm256_cmpgt_epi64(data_vec, val_vec);
                    break;
                case Op::kLt:
                    cmp_res = _mm256_cmpgt_epi64(val_vec, data_vec);
                    break;
                case Op::kGe: {
                    __m256i gt = _mm256_cmpgt_epi64(val_vec, data_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
                case Op::kLe: {
                    __m256i gt = _mm256_cmpgt_epi64(data_vec, val_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
            }
            int mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp_res));
            if (mask) {
                for (int bit = 0; bit < 4; ++bit) {
                    if ((mask >> bit) & 1)
                        matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }
    else if constexpr (std::is_same_v<T, double>) {
        __m256d val_vec = _mm256_set1_pd(value);
        for (; i + 3 < count; i += 4) {
            __m256d data_vec = _mm256_loadu_pd(data + i);
            __m256d cmp_res;
            switch (op) {
                case Op::kEq:
                    cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_EQ_OQ);
                    break;
                case Op::kGt:
                    cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_GT_OQ);
                    break;
                case Op::kLt:
                    cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_LT_OQ);
                    break;
                case Op::kGe:
                    cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_GE_OQ);
                    break;
                case Op::kLe:
                    cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_LE_OQ);
                    break;
            }
            int mask = _mm256_movemask_pd(cmp_res);
            if (mask) {
                for (int bit = 0; bit < 4; ++bit) {
                    if ((mask >> bit) & 1)
                        matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }

    for (; i < count; ++i) {
        bool match = false;
        switch (op) {
            case Op::kEq:
                match = (data[i] == value);
                break;
            case Op::kGt:
                match = (data[i] > value);
                break;
            case Op::kLt:
                match = (data[i] < value);
                break;
            case Op::kGe:
                match = (data[i] >= value);
                break;
            case Op::kLe:
                match = (data[i] <= value);
                break;
        }
        if (match)
            matching.push_back(i);
    }
#else
    filter_scalar(data, count, value, op, matching);
#endif
}

// ──────────────────────────────────────────────
// AVX-512 Implementation
// ──────────────────────────────────────────────
template <typename T>
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("avx512f,avx512dq")))
#endif
void filter_avx512(const T* data, size_t count, T value, Op op, std::vector<size_t>& matching)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    size_t i = 0;

    if constexpr (std::is_same_v<T, int32_t>) {
        __m512i val_vec = _mm512_set1_epi32(value);
        for (; i + 15 < count; i += 16) {
            __m512i data_vec = _mm512_loadu_si512(reinterpret_cast<const void *>(data + i));
            __mmask16 mask = 0;
            switch (op) {
                case Op::kEq:
                    mask = _mm512_cmp_epi32_mask(data_vec, val_vec, _MM_CMPINT_EQ);
                    break;
                case Op::kGt:
                    mask = _mm512_cmp_epi32_mask(data_vec, val_vec, _MM_CMPINT_GT);
                    break;
                case Op::kLt:
                    mask = _mm512_cmp_epi32_mask(data_vec, val_vec, _MM_CMPINT_LT);
                    break;
                case Op::kGe:
                    mask = _mm512_cmp_epi32_mask(data_vec, val_vec, _MM_CMPINT_GE);
                    break;
                case Op::kLe:
                    mask = _mm512_cmp_epi32_mask(data_vec, val_vec, _MM_CMPINT_LE);
                    break;
            }
            while (mask) {
                int bit = __builtin_ctz(mask);
                matching.push_back(i + static_cast<size_t>(bit));
                mask &= mask - 1;
            }
        }
    }
    else if constexpr (std::is_same_v<T, int64_t>) {
        __m512i val_vec = _mm512_set1_epi64(value);
        for (; i + 7 < count; i += 8) {
            __m512i data_vec = _mm512_loadu_si512(reinterpret_cast<const void *>(data + i));
            __mmask8 mask = 0;
            switch (op) {
                case Op::kEq:
                    mask = _mm512_cmp_epi64_mask(data_vec, val_vec, _MM_CMPINT_EQ);
                    break;
                case Op::kGt:
                    mask = _mm512_cmp_epi64_mask(data_vec, val_vec, _MM_CMPINT_GT);
                    break;
                case Op::kLt:
                    mask = _mm512_cmp_epi64_mask(data_vec, val_vec, _MM_CMPINT_LT);
                    break;
                case Op::kGe:
                    mask = _mm512_cmp_epi64_mask(data_vec, val_vec, _MM_CMPINT_GE);
                    break;
                case Op::kLe:
                    mask = _mm512_cmp_epi64_mask(data_vec, val_vec, _MM_CMPINT_LE);
                    break;
            }
            while (mask) {
                int bit = __builtin_ctz(mask);
                matching.push_back(i + static_cast<size_t>(bit));
                mask &= mask - 1;
            }
        }
    }
    else if constexpr (std::is_same_v<T, double>) {
        __m512d val_vec = _mm512_set1_pd(value);
        for (; i + 7 < count; i += 8) {
            __m512d data_vec = _mm512_loadu_pd(data + i);
            __mmask8 mask = 0;
            switch (op) {
                case Op::kEq:
                    mask = _mm512_cmp_pd_mask(data_vec, val_vec, _CMP_EQ_OQ);
                    break;
                case Op::kGt:
                    mask = _mm512_cmp_pd_mask(data_vec, val_vec, _CMP_GT_OQ);
                    break;
                case Op::kLt:
                    mask = _mm512_cmp_pd_mask(data_vec, val_vec, _CMP_LT_OQ);
                    break;
                case Op::kGe:
                    mask = _mm512_cmp_pd_mask(data_vec, val_vec, _CMP_GE_OQ);
                    break;
                case Op::kLe:
                    mask = _mm512_cmp_pd_mask(data_vec, val_vec, _CMP_LE_OQ);
                    break;
            }
            while (mask) {
                int bit = __builtin_ctz(mask);
                matching.push_back(i + static_cast<size_t>(bit));
                mask &= mask - 1;
            }
        }
    }

    for (; i < count; ++i) {
        bool match = false;
        switch (op) {
            case Op::kEq:
                match = (data[i] == value);
                break;
            case Op::kGt:
                match = (data[i] > value);
                break;
            case Op::kLt:
                match = (data[i] < value);
                break;
            case Op::kGe:
                match = (data[i] >= value);
                break;
            case Op::kLe:
                match = (data[i] <= value);
                break;
        }
        if (match)
            matching.push_back(i);
    }
#else
    filter_scalar(data, count, value, op, matching);
#endif
}

// ──────────────────────────────────────────────
// Dynamic Dispatcher
// ──────────────────────────────────────────────
template <typename T>
void filter(const T *data, size_t count, T value, Op op, std::vector<size_t> &matching)
{
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq")) {
        filter_avx512(data, count, value, op, matching);
    }
    else if (__builtin_cpu_supports("avx2")) {
        filter_avx2(data, count, value, op, matching);
    }
    else {
        filter_scalar(data, count, value, op, matching);
    }
#else
    // Fallback for MSVC or non-x86 architectures
    filter_scalar(data, count, value, op, matching);
#endif
}

} // namespace rawdb::simd
