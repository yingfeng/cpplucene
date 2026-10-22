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

export module lucene_core:field_invert_state;

import :types;
import std;

import :index_types;

namespace lucene {

/// Number of terms, positions, offsets and overlaps collected while indexing
/// one field of one document — the input of
/// [`Similarity::compute_norm`](lucene_core:similarity).
///
/// Port of `FieldInvertState` (`core/index/field_invert_state.rs`).
export class FieldInvertState {
public:
    FieldInvertState() = default;

    /// A state for `name` with `index_options`, zeroed.
    FieldInvertState(int32_t index_created_version_major, std::string name, IndexOptions index_options);

    /// A state with every counter supplied.
    [[nodiscard]] static FieldInvertState with_states(int32_t index_created_version_major, std::string name,
                                                      IndexOptions index_options, int32_t position, int32_t length,
                                                      int32_t num_overlap, int32_t offset,
                                                      int32_t max_term_frequency, int32_t unique_term_count);

    /// Re-initializes the counters for the next document.
    void reset() noexcept;

    [[nodiscard]] int32_t position() const noexcept { return position_; }
    [[nodiscard]] int32_t length() const noexcept { return length_; }
    void set_length(int32_t length) noexcept { length_ = length; }

    [[nodiscard]] int32_t num_overlap() const noexcept { return num_overlap_; }
    void set_num_overlap(int32_t num_overlap) noexcept { num_overlap_ = num_overlap; }

    [[nodiscard]] int32_t offset() const noexcept { return offset_; }
    void set_offset(int32_t offset) noexcept { offset_ = offset; }

    [[nodiscard]] int32_t max_term_frequency() const noexcept { return max_term_frequency_; }
    [[nodiscard]] int32_t unique_term_count() const noexcept { return unique_term_count_; }
    [[nodiscard]] const std::string &name() const noexcept { return name_; }
    [[nodiscard]] int32_t index_created_version_major() const noexcept { return index_created_version_major_; }
    [[nodiscard]] IndexOptions index_options() const noexcept { return index_options_; }

    // Setters the indexer uses while it walks the tokens.
    void set_position(int32_t position) noexcept { position_ = position; }
    void set_max_term_frequency(int32_t value) noexcept { max_term_frequency_ = value; }
    void set_unique_term_count(int32_t value) noexcept { unique_term_count_ = value; }
    void set_last_start_offset(int32_t value) noexcept { last_start_offset_ = value; }
    void set_last_position(int32_t value) noexcept { last_position_ = value; }

    [[nodiscard]] int32_t last_start_offset() const noexcept { return last_start_offset_; }
    [[nodiscard]] int32_t last_position() const noexcept { return last_position_; }

private:
    int32_t index_created_version_major_{6};
    std::string name_;
    IndexOptions index_options_{IndexOptions::None};
    int32_t position_{-1};
    int32_t length_{0};
    int32_t num_overlap_{0};
    int32_t offset_{0};
    int32_t max_term_frequency_{0};
    int32_t unique_term_count_{0};
    // tracked across field instances (the multi-valued case)
    int32_t last_start_offset_{0};
    int32_t last_position_{0};
};

} // namespace lucene
