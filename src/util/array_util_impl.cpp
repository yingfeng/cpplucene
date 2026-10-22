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

module lucene_core:array_util.impl;

import :array_util;

import :types;
import std;

namespace lucene {

Result<size_t> ArrayUtil::oversize(size_t min_target_size, size_t bytes_per_element) {
    if (min_target_size == 0) {
        // Wait until at least one element is requested.
        return 0;
    }

    size_t max_array_length = MAX_ARRAY_LENGTH;
    if (bytes_per_element != 0) {
        constexpr size_t kSizeTMax = std::numeric_limits<size_t>::max() >> 1; // isize::MAX
        max_array_length = std::min(MAX_ARRAY_LENGTH, kSizeTMax / bytes_per_element);
    }

    if (min_target_size > max_array_length) {
        return err(LuceneError::illegal_argument(
            std::format("requested vector size {} exceeds maximum vector length {} for elements of {} bytes",
                        min_target_size,
                        max_array_length,
                        bytes_per_element)));
    }

    // Asymptotic exponential growth by one eighth favors spending a bit more CPU
    // in order to avoid tying up too much unused RAM.
    size_t extra = min_target_size >> 3;
    if (extra < 3) {
        // For very small vectors, grow faster.
        extra = 3;
    }

    const size_t grown = min_target_size + extra < min_target_size ? max_array_length : min_target_size + extra;
    return std::min(grown, max_array_length);
}

int32_t ArrayUtil::hash_code(std::span<const char32_t> array, size_t start, size_t end) {
    int32_t code = 0;
    for (size_t i = end; i-- > start;) {
        code = static_cast<int32_t>(static_cast<uint32_t>(code) * 31u + static_cast<uint32_t>(array[i]));
    }
    return code;
}

} // namespace lucene
