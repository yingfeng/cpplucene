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

module lucene_core:packed_reader_iterator.impl;

import :packed_reader_iterator;

import :data_input;
import :error;
import :packed_stream;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<PackedReaderIterator>> PackedReaderIterator::create(DataInput &in, int32_t bits_per_value,
                                                                          size_t value_count) {
    auto values = read_packed_stream(in, value_count, bits_per_value);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    return std::unique_ptr<PackedReaderIterator>(
            new PackedReaderIterator(std::move(values.value()), bits_per_value));
}

Result<int64_t> PackedReaderIterator::next() {
    if (is_exhausted()) {
        return err(LuceneError::eof("the iteration is past the end of the packed values"));
    }
    return values_[position_++];
}

Result<std::vector<int64_t>> PackedReaderIterator::next(size_t count) {
    if (count > values_.size() - position_) {
        return err(LuceneError::eof("asked for " + std::to_string(count) + " values, " +
                                    std::to_string(values_.size() - position_) + " left"));
    }
    const auto first = values_.begin() + static_cast<std::ptrdiff_t>(position_);
    std::vector<int64_t> result(first, first + static_cast<std::ptrdiff_t>(count));
    position_ += count;
    return result;
}

} // namespace lucene
