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

export module lucene_core:fst_int_sequence_outputs;

import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :ints_ref;
import :fst_outputs;

namespace lucene {

/// [`FstOutputs`] whose values are sequences of ints (a prefix monoid).
///
/// Port of `util/fst_impl/int_sequence_outputs.rs`.  The value type is
/// [`IntsRef`]; an empty sequence is NO_OUTPUT.
export class FstIntSequenceOutputs final : public FstOutputs {
public:
    /// The shared instance (Lucene's `IntSequenceOutputs.getSingleton()`).
    [[nodiscard]] static const FstIntSequenceOutputs &singleton();

    [[nodiscard]] std::string_view type_name() const noexcept override { return "IntSequenceOutputs"; }

    [[nodiscard]] FstOutput common(const FstOutput &output1, const FstOutput &output2) const override;
    [[nodiscard]] FstOutput subtract(const FstOutput &output, const FstOutput &inc) const override;
    [[nodiscard]] FstOutput add(const FstOutput &prefix, const FstOutput &output) const override;

    [[nodiscard]] VoidResult write(const FstOutput &output, DataOutput &out) const override;
    [[nodiscard]] Result<FstOutput> read(DataInput &input) const override;

    [[nodiscard]] FstOutput get_no_output() const override;
    [[nodiscard]] bool is_no_output(const FstOutput &output) const override;
    [[nodiscard]] std::string output_to_string(const FstOutput &output) const override;
    [[nodiscard]] int64_t ram_bytes_used(const FstOutput &output) const override;
};

} // namespace lucene
