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

export module lucene_core:block_tree_output_accumulator;

import :types;
import std;

import :bytes_ref;
import :data_input;
import :error;

namespace lucene {

/// The FST outputs accumulated on the path to the current block, and a
/// [`DataInput`] cursor that reads across them.
///
/// Port of `OutputAccumulator` (`core/codecs/lucene90/block_tree/segment_terms_enum.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * rlucene shares the output buffers through `Arc<Vec<u8>>`, so
///    `set_floor_data` can hand a `ByteArrayDataInput` a view into them.  A
///    [`ByteArrayDataInput`] in this port *borrows* its bytes, so
///    [`set_floor_data`] copies the unread tail into a caller owned buffer
///    instead (the floor data is short: a vint plus one label);
///  * `SegmentTermsEnum` reads the outputs directly in rlucene (same module,
///    `pub(crate)` fields); C++ needs accessors for the same thing.
export class OutputAccumulator : public DataInput {
public:
    OutputAccumulator() = default;

    /// Appends `output`, ignoring the empty (`NO_OUTPUT`) one.  A slot that
    /// already holds a different output is overwritten in place.
    void push(const BytesRef &output);

    /// Removes `output`, which must be the last one pushed.  Empty outputs are
    /// ignored, mirroring [`push`].
    void pop(const BytesRef &output);

    /// Removes the last `count` outputs.
    void pop_n(size_t count);

    [[nodiscard]] size_t output_count() const noexcept { return num_; }
    [[nodiscard]] size_t current_index() const noexcept { return current_index_; }
    [[nodiscard]] size_t index() const noexcept { return index_; }
    [[nodiscard]] const BytesRef &output_at(size_t position) const;

    /// Drops every output (the cursor is not repositioned).
    void reset() noexcept { num_ = 0; }

    /// Rewinds the cursor to the first byte of the first output.
    void prepare_read() noexcept {
        index_ = 0;
        current_index_ = 0;
    }

    /// Copies the unread tail of the last output into `floor_data`, which the
    /// caller wraps in its own [`ByteArrayDataInput`].  The cursor is not
    /// moved.
    [[nodiscard]] VoidResult set_floor_data(std::vector<uint8_t> &floor_data) const;

    // ---- DataInput ----

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> /*b*/) override {
        return err(LuceneError::unsupported_operation("OutputAccumulator::read_bytes"));
    }
    [[nodiscard]] VoidResult skip_bytes(int64_t /*num_bytes*/) override {
        return err(LuceneError::unsupported_operation("OutputAccumulator::skip_bytes"));
    }

    [[nodiscard]] std::string to_string() const override { return "OutputAccumulator"; }

private:
    std::vector<BytesRef> outputs_;
    /// Number of valid entries in [`outputs_`].
    size_t num_{0};
    size_t current_index_{0};
    size_t index_{0};
};

} // namespace lucene
