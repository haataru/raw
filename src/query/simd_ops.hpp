#pragma once

#include <vector>
#include <cstdint>
#include <type_traits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace rawdb::simd
{

enum class Op {
    kEq,
    kGt,
    kLt,
    kGe,
    kLe
};

template <typename T>
void filter(const T* data, size_t count, T value, Op op, std::vector<size_t>& matching)
{
#if defined(__AVX2__)
    size_t i = 0;
    
    // INT32 (8 elements at a time)
    if constexpr (std::is_same_v<T, int32_t>) {
        __m256i val_vec = _mm256_set1_epi32(value);
        for (; i + 7 < count; i += 8) {
            __m256i data_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i cmp_res;
            switch(op) {
                case Op::kEq: cmp_res = _mm256_cmpeq_epi32(data_vec, val_vec); break;
                case Op::kGt: cmp_res = _mm256_cmpgt_epi32(data_vec, val_vec); break;
                // For Lt, we do (val > data)
                case Op::kLt: cmp_res = _mm256_cmpgt_epi32(val_vec, data_vec); break;
                // AVX2 doesn't have native cmple/cmpge for integers, so we synthesize them:
                // a >= b is same as ~(b > a) which is ~(val > data)
                case Op::kGe: {
                    __m256i gt = _mm256_cmpgt_epi32(val_vec, data_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
                // a <= b is same as ~(a > b) which is ~(data > val)
                case Op::kLe: {
                    __m256i gt = _mm256_cmpgt_epi32(data_vec, val_vec);
                    cmp_res = _mm256_xor_si256(gt, _mm256_set1_epi32(-1));
                    break;
                }
            }
            int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp_res));
            if (mask) {
                // mask is 8 bits (one for each 32-bit element)
                for (int bit = 0; bit < 8; ++bit) {
                    if ((mask >> bit) & 1) matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }
    // INT64 (4 elements at a time)
    else if constexpr (std::is_same_v<T, int64_t>) {
        __m256i val_vec = _mm256_set1_epi64x(value);
        for (; i + 3 < count; i += 4) {
            __m256i data_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i cmp_res;
            switch(op) {
                case Op::kEq: cmp_res = _mm256_cmpeq_epi64(data_vec, val_vec); break;
                case Op::kGt: cmp_res = _mm256_cmpgt_epi64(data_vec, val_vec); break;
                case Op::kLt: cmp_res = _mm256_cmpgt_epi64(val_vec, data_vec); break;
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
                    if ((mask >> bit) & 1) matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }
    // FLOAT64 (4 elements at a time)
    else if constexpr (std::is_same_v<T, double>) {
        __m256d val_vec = _mm256_set1_pd(value);
        for (; i + 3 < count; i += 4) {
            __m256d data_vec = _mm256_loadu_pd(data + i);
            __m256d cmp_res;
            switch(op) {
                case Op::kEq: cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_EQ_OQ); break;
                case Op::kGt: cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_GT_OQ); break;
                case Op::kLt: cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_LT_OQ); break;
                case Op::kGe: cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_GE_OQ); break;
                case Op::kLe: cmp_res = _mm256_cmp_pd(data_vec, val_vec, _CMP_LE_OQ); break;
            }
            int mask = _mm256_movemask_pd(cmp_res);
            if (mask) {
                for (int bit = 0; bit < 4; ++bit) {
                    if ((mask >> bit) & 1) matching.push_back(i + static_cast<size_t>(bit));
                }
            }
        }
    }
    
    // Process remaining elements sequentially
    for (; i < count; ++i) {
        bool match = false;
        switch(op) {
            case Op::kEq: match = (data[i] == value); break;
            case Op::kGt: match = (data[i] > value); break;
            case Op::kLt: match = (data[i] < value); break;
            case Op::kGe: match = (data[i] >= value); break;
            case Op::kLe: match = (data[i] <= value); break;
        }
        if (match) matching.push_back(i);
    }
#else
    // Fallback scalar implementation if AVX2 is not enabled
    for (size_t i = 0; i < count; ++i) {
        bool match = false;
        switch(op) {
            case Op::kEq: match = (data[i] == value); break;
            case Op::kGt: match = (data[i] > value); break;
            case Op::kLt: match = (data[i] < value); break;
            case Op::kGe: match = (data[i] >= value); break;
            case Op::kLe: match = (data[i] <= value); break;
        }
        if (match) matching.push_back(i);
    }
#endif
}

} // namespace rawdb::simd
