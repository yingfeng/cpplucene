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

export module lucene_core:binary_range_doc_values;

import :types;
import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_values_iterator;
import :error;

namespace lucene {

/// A binary doc values cursor over the encoded `[min, max]` ranges a range doc
/// values field stores, decoding the packed value of the current document.
///
/// Port of `BinaryRangeDocValues` (`core/document/binary_range_doc_values.rs`).
export class BinaryRangeDocValues final : public BinaryDocValues {
public:
    /// `num_bytes_per_dimension` is twice the width of one encoded value, because
    /// a dimension holds both its minimum and its maximum.
    BinaryRangeDocValues(std::unique_ptr<BinaryDocValues> in, size_t num_dims, size_t num_bytes_per_dimension);

    /// The packed range of the current document.
    [[nodiscard]] std::span<const uint8_t> packed_value() const noexcept { return packed_value_; }

    // ---- DocValuesIterator ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;

    // ---- BinaryDocValues ----

    /// The undecoded value of the current document.
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    [[nodiscard]] VoidResult decode_ranges();

    std::unique_ptr<BinaryDocValues> in_;
    std::vector<uint8_t> packed_value_;
    size_t num_dims_;
    size_t num_bytes_per_dimension_;
};

} // namespace lucene
