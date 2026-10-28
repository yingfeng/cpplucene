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

module lucene_core:lsb_radix_sorter.impl;

import :lsb_radix_sorter;
import :types;
import :error;

import std;

namespace lucene {

void LSBRadixSorter::build_histogram(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram,
                                     size_t shift) {
    for (size_t i = 0; i < len; ++i) {
        const size_t bucket = (static_cast<uint32_t>(array[i]) >> shift) & 0xFFU;
        histogram[bucket] += 1;
    }
}

void LSBRadixSorter::sum_histogram(std::span<size_t, 256> histogram) {
    size_t accum = 0;
    for (size_t &count : histogram) {
        const size_t current = count;
        count = accum;
        accum += current;
    }
}

void LSBRadixSorter::reorder(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram,
                             size_t shift, std::span<int32_t> dest) {
    for (size_t i = 0; i < len; ++i) {
        const int32_t value = array[i];
        const size_t bucket = (static_cast<uint32_t>(value) >> shift) & 0xFFU;
        const size_t index = histogram[bucket];
        dest[index] = value;
        histogram[bucket] += 1;
    }
}

bool LSBRadixSorter::sort_pass(std::span<const int32_t> array, size_t len, std::span<size_t, 256> histogram,
                               size_t shift, std::span<int32_t> dest) {
    std::fill(histogram.begin(), histogram.end(), size_t{0});
    build_histogram(array, len, histogram, shift);
    if (histogram[0] == len) {
        return false;
    }
    sum_histogram(histogram);
    reorder(array, len, histogram, shift, dest);
    return true;
}

void LSBRadixSorter::insertion_sort(std::span<int32_t> array, size_t off, size_t len) {
    const size_t end = off + len;
    for (size_t i = off + 1; i < end; ++i) {
        for (size_t j = i; j > off; --j) {
            if (array[j - 1] > array[j]) {
                std::swap(array[j - 1], array[j]);
            } else {
                break;
            }
        }
    }
}

VoidResult LSBRadixSorter::sort(size_t num_bits, std::span<int32_t> array, size_t len) {
    if (len > array.size()) {
        return err(LuceneError::illegal_argument(
            std::format("len {} is larger than the array size {}", len, array.size())));
    }
    if (len < LSB_INSERTION_SORT_THRESHOLD) {
        insertion_sort(array, 0, len);
        return {};
    }
    if (buffer_.size() < len) {
        buffer_.resize(len);
    }

    bool swapped = false;
    std::span<int32_t> current = array.first(len);
    std::span<int32_t> buffer = std::span<int32_t>(buffer_.data(), len);

    for (size_t shift = 0; shift < num_bits; shift += 8) {
        if (sort_pass(current, len, histogram_, shift, buffer)) {
            std::swap(current, buffer);
            swapped = !swapped;
        }
    }
    if (swapped) {
        std::copy(current.begin(), current.end(), array.begin());
    }
    return {};
}

} // namespace lucene
