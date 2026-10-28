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

export module lucene_core:packed_reader_iterator;

import :types;
import std;

import :data_input;
import :error;

namespace lucene {

/// Reads a `Format.PACKED` stream value by value.
///
/// Port of `PackedReaderIterator` (`util/packed/packed_ints.rs`): [`next`]
/// consumes one value, [`next(count)`] a batch, and [`ordinal`] reports how many
/// were consumed.
///
/// **Deviation:** the reference implementation reads through a buffer and can
/// `reset`; the port reads the whole stream when it is created, so a reset simply
/// rewinds an index.  The stream must therefore fit in memory, which is the case
/// for everything a builder writes.
export class PackedReaderIterator {
public:
    /// Reads `value_count` values of `bits_per_value` bits from `in`.
    [[nodiscard]] static Result<std::unique_ptr<PackedReaderIterator>> create(DataInput &in, int32_t bits_per_value,
                                                                             size_t value_count);

    /// The next value.
    [[nodiscard]] Result<int64_t> next();
    /// The next `count` values.
    [[nodiscard]] Result<std::vector<int64_t>> next(size_t count);

    /// How many values were consumed.
    [[nodiscard]] int64_t ordinal() const noexcept { return static_cast<int64_t>(position_); }
    /// How many values the stream holds.
    [[nodiscard]] size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }
    /// Whether every value was consumed.
    [[nodiscard]] bool is_exhausted() const noexcept { return position_ >= values_.size(); }

    /// Starts over at the first value.
    void reset() noexcept { position_ = 0; }

private:
    PackedReaderIterator(std::vector<int64_t> values, int32_t bits_per_value)
        : values_(std::move(values)), bits_per_value_(bits_per_value) {}

    std::vector<int64_t> values_;
    int32_t bits_per_value_;
    size_t position_{0};
};

} // namespace lucene
