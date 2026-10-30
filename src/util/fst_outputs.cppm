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

export module lucene_core:fst_outputs;

import :fst_output;
import :types;
import :error;

import std;

import :data_input;
import :data_output;

namespace lucene {

/// The output algebra an [`Fst`] needs to build and traverse itself.
///
/// Port of the `Outputs` trait (`util/fst_impl/outputs.rs`).
///
/// **Deviation:** the reference implementation parametrizes every FST type with the output type and
/// relies on `Arc` pointer identity to recognize `NO_OUTPUT` ("must return the
/// same SINGLETON object").  C++ has no such parametric module boundary, so the
/// output value is a `FstOutput` and emptiness is answered by
/// [`is_no_output`] instead of pointer comparison.  Every implementation keeps
/// the same algebra: `common` is the longest common prefix (or the smaller
/// value), `subtract` removes a shared prefix and `add` prepends one.
export class FstOutputs {
public:
    virtual ~FstOutputs() = default;

    /// Name used in debug output (`Display` in the reference implementation).
    [[nodiscard]] virtual std::string_view type_name() const noexcept = 0;

    /// Longest common prefix of both outputs, e.g. `common("foobar", "food")` is
    /// `"foo"`.
    [[nodiscard]] virtual FstOutput common(const FstOutput &output1, const FstOutput &output2) const = 0;

    /// Removes `inc` from the front of `output`, e.g. `subtract("foobar", "foo")`
    /// is `"bar"`.
    [[nodiscard]] virtual FstOutput subtract(const FstOutput &output, const FstOutput &inc) const = 0;

    /// Prepends `prefix`, e.g. `add("foo", "bar")` is `"foobar"`.
    [[nodiscard]] virtual FstOutput add(const FstOutput &prefix, const FstOutput &output) const = 0;

    /// `prefix = add(prefix, output)`; `output` is a disposable scratch slot.
    virtual void add_assign(FstOutput &prefix, FstOutput &output) const;

    [[nodiscard]] virtual VoidResult write(const FstOutput &output, DataOutput &out) const = 0;

    /// Encodes a final node's output; defaults to [`write`].
    [[nodiscard]] virtual VoidResult write_final_output(const FstOutput &output, DataOutput &out) const;

    [[nodiscard]] virtual Result<FstOutput> read(DataInput &input) const = 0;

    /// Skips an output written by [`write`].
    [[nodiscard]] virtual VoidResult skip_output(DataInput &input) const;

    [[nodiscard]] virtual Result<FstOutput> read_final_output(DataInput &input) const;

    [[nodiscard]] virtual VoidResult skip_final_output(DataInput &input) const;

    /// The "no output" value of this algebra.
    [[nodiscard]] virtual FstOutput get_no_output() const = 0;

    /// `true` when `output` is this algebra's NO_OUTPUT.
    [[nodiscard]] virtual bool is_no_output(const FstOutput &output) const = 0;

    [[nodiscard]] virtual std::string output_to_string(const FstOutput &output) const = 0;

    /// Merges two outputs (only meaningful for the `SortedSet` FSTs).
    [[nodiscard]] virtual Result<FstOutput> merge(const FstOutput &first, const FstOutput &second) const;

    [[nodiscard]] virtual int64_t ram_bytes_used(const FstOutput &output) const = 0;
};

} // namespace lucene
