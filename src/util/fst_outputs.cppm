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
/// **Deviation:** rlucene parametrizes every FST type with the output type and
/// relies on `Arc` pointer identity to recognize `NO_OUTPUT` ("must return the
/// same SINGLETON object").  C++ has no such parametric module boundary, so the
/// output value is a `std::any` and emptiness is answered by
/// [`is_no_output`] instead of pointer comparison.  Every implementation keeps
/// the same algebra: `common` is the longest common prefix (or the smaller
/// value), `subtract` removes a shared prefix and `add` prepends one.
export class FstOutputs {
public:
    virtual ~FstOutputs() = default;

    /// Name used in debug output (`Display` in rlucene).
    [[nodiscard]] virtual std::string_view type_name() const noexcept = 0;

    /// Longest common prefix of both outputs, e.g. `common("foobar", "food")` is
    /// `"foo"`.
    [[nodiscard]] virtual std::any common(const std::any &output1, const std::any &output2) const = 0;

    /// Removes `inc` from the front of `output`, e.g. `subtract("foobar", "foo")`
    /// is `"bar"`.
    [[nodiscard]] virtual std::any subtract(const std::any &output, const std::any &inc) const = 0;

    /// Prepends `prefix`, e.g. `add("foo", "bar")` is `"foobar"`.
    [[nodiscard]] virtual std::any add(const std::any &prefix, const std::any &output) const = 0;

    /// `prefix = add(prefix, output)`; `output` is a disposable scratch slot.
    virtual void add_assign(std::any &prefix, std::any &output) const;

    [[nodiscard]] virtual VoidResult write(const std::any &output, DataOutput &out) const = 0;

    /// Encodes a final node's output; defaults to [`write`].
    [[nodiscard]] virtual VoidResult write_final_output(const std::any &output, DataOutput &out) const;

    [[nodiscard]] virtual Result<std::any> read(DataInput &input) const = 0;

    /// Skips an output written by [`write`].
    [[nodiscard]] virtual VoidResult skip_output(DataInput &input) const;

    [[nodiscard]] virtual Result<std::any> read_final_output(DataInput &input) const;

    [[nodiscard]] virtual VoidResult skip_final_output(DataInput &input) const;

    /// The "no output" value of this algebra.
    [[nodiscard]] virtual std::any get_no_output() const = 0;

    /// `true` when `output` is this algebra's NO_OUTPUT.
    [[nodiscard]] virtual bool is_no_output(const std::any &output) const = 0;

    [[nodiscard]] virtual std::string output_to_string(const std::any &output) const = 0;

    /// Merges two outputs (only meaningful for the `SortedSet` FSTs).
    [[nodiscard]] virtual Result<std::any> merge(const std::any &first, const std::any &second) const;

    [[nodiscard]] virtual int64_t ram_bytes_used(const std::any &output) const = 0;
};

} // namespace lucene
