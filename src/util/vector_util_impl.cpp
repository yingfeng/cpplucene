/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

module;

#if defined(__AVX2__)
#include <immintrin.h>
#endif

module lucene_core:vector_util.impl;

import :vector_util;

import std;

namespace lucene {

size_t VectorUtil::find_next_geq(std::span<const int32_t> buffer, int32_t target, size_t from, size_t to) {
    if (from >= to) {
        return to;
    }
    const int32_t *const values = buffer.data();
    size_t i = from;
#if defined(__AVX2__)
    // `values[j] >= target` is `values[j] > target - 1`; the comparison saturates
    // at the type bounds, so the decrement is safe for every document id.
    const __m256i threshold = _mm256_set1_epi32(target - 1);
    for (; i + 8 <= to; i += 8) {
        const __m256i lanes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(values + i));
        const __m256i above = _mm256_cmpgt_epi32(lanes, threshold);
        const auto mask = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(above)));
        if (mask != 0U) {
            return i + static_cast<size_t>(std::countr_zero(mask));
        }
    }
#elif defined(__SSE2__)
    const __m128i threshold = _mm_set1_epi32(target - 1);
    for (; i + 4 <= to; i += 4) {
        const __m128i lanes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(values + i));
        const __m128i above = _mm_cmpgt_epi32(lanes, threshold);
        const auto mask = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(above)));
        if (mask != 0U) {
            return i + static_cast<size_t>(std::countr_zero(mask));
        }
    }
#endif
    for (; i < to; ++i) {
        if (values[i] >= target) {
            return i;
        }
    }
    return to;
}

} // namespace lucene
