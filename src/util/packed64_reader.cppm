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

export module lucene_core:packed64_reader;

import :types;
import std;

import :error;
import :packed_ints;

namespace lucene {

/// An immutable, in-memory array of packed integers.
///
/// Port of `Packed64Reader` (`util/packed/packed64.rs`, the immutable half this
/// port keeps separate from [`Packed64`](lucene_core:packed64)).  The values are
/// held as plain `int64_t`s: the packed width only matters on disk, and a narrower
/// in-memory representation would buy nothing here.
///
/// **Deviation:** the reference implementation keeps the values in 64-bit words and
/// offers bulk decoders per width; the port stores one `int64_t` per value and
/// reports the memory it actually uses.
export class Packed64Reader final : public PackedIntsReader {
public:
    Packed64Reader(std::vector<int64_t> values, int32_t bits_per_value);

    [[nodiscard]] int64_t get(size_t index) const override;
    [[nodiscard]] size_t size() const override { return values_.size(); }
    [[nodiscard]] int32_t bits_per_value() const override { return bits_per_value_; }

    /// The values, as stored.
    [[nodiscard]] const std::vector<int64_t> &values() const noexcept { return values_; }

    /// The bytes the values occupy.
    [[nodiscard]] int64_t ram_bytes_used() const noexcept override {
        return static_cast<int64_t>(values_.size() * sizeof(int64_t));
    }

private:
    std::vector<int64_t> values_;
    int32_t bits_per_value_;
};

} // namespace lucene
