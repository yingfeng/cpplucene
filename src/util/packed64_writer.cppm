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

export module lucene_core:packed64_writer;

import :types;
import std;

import :data_output;
import :error;
import :packed_ints;

namespace lucene {

/// Writes values one by one into a `Format.PACKED` stream.
///
/// Port of `PackedWriter` (`util/packed/packed_ints.rs`).
///
/// **Deviation:** the reference implementation bit-packs straight into the output
/// through a small buffer; the port collects the values and writes them in one go
/// when [`finish`] runs, which costs `8 * value_count` bytes of memory and no
/// measurable time for the sizes a builder here produces.
export class Packed64Writer final : public PackedIntsWriter {
public:
    /// Writes `value_count` values of `bits_per_value` bits into `out`.
    ///
    /// The values are checked against `bits_per_value` like the reference
    /// implementation does, so a value that does not fit is reported instead of
    /// being truncated.
    Packed64Writer(DataOutput &out, int32_t bits_per_value, size_t value_count);

    [[nodiscard]] VoidResult add(int64_t value) override;
    /// Writes the values collected so far; a second call is an error.
    [[nodiscard]] VoidResult finish() override;

    [[nodiscard]] int32_t bits_per_value() const override { return bits_per_value_; }
    [[nodiscard]] int64_t ordinal() const override { return static_cast<int64_t>(values_.size()); }

    /// Whether [`finish`] ran.
    [[nodiscard]] bool is_finished() const noexcept { return finished_; }

private:
    DataOutput *out_;
    int32_t bits_per_value_;
    size_t value_count_;
    std::vector<int64_t> values_;
    bool finished_{false};
};

} // namespace lucene
