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

export module lucene_core:lucene90_norms_producer;

import :types;
import :error;

import std;

import :field_infos;
import :index_input;
import :lucene90_norms_format;
import :norms_producer;
import :numeric_doc_values;
import :segment_info;
import :segment_read_state;

namespace lucene {

/// Reads the `.nvd`/`.nvm` pair of one segment.
///
/// Port of `Lucene90NormsProducer`
/// (`codecs/lucene90/lucene90_norms_producer.rs`): the whole metadata file is
/// parsed up front into one entry per field, and [`get_norms`] builds a fresh
/// [`NumericDocValues`] cursor on demand — a dense cursor when every document has
/// a norm, a sparse one pairing [`IndexedDISI`] with the values otherwise.
///
/// **Deviation:** the reference implementation's `get_merge_instance` (the merging producer) is not
/// ported, because merging needs `MergeState`; [`get_merge_instance`] therefore
/// returns `nullptr`, which tells a merger to merge through [`get_norms`].
export class Lucene90NormsProducer final : public NormsProducer {
public:
    /// Opens the two files and parses the metadata.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90NormsProducer>> create(const SegmentReadState &state,
                                                                              const SegmentInfo &segment_info);

    ~Lucene90NormsProducer() override;

    Lucene90NormsProducer(const Lucene90NormsProducer &) = delete;
    Lucene90NormsProducer &operator=(const Lucene90NormsProducer &) = delete;

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override;

    [[nodiscard]] VoidResult check_integrity() const override;

    [[nodiscard]] VoidResult close() const override;

    /// Per-field metadata, as stored in the `.nvm` file.
    struct NormsEntry {
        /// `-2`: no document has a norm; `-1`: every document has one; otherwise
        /// the file pointer of the [`IndexedDISI`].
        int64_t docs_with_field_offset{-2};
        int64_t docs_with_field_length{0};
        int32_t jump_table_entry_count{-1};
        int8_t dense_rank_power{-1};
        int32_t num_docs_with_field{0};
        /// `0`: every norm equals [`norms_address`]; otherwise the file pointer of
        /// the values.
        int8_t bytes_per_norm{-1};
        int64_t norms_address{0};
    };

    /// The parsed metadata of `field_num`, or `nullptr`.
    [[nodiscard]] const NormsEntry *entry(int32_t field_num) const noexcept;

private:
    Lucene90NormsProducer(std::shared_ptr<IndexInput> data, std::map<int32_t, NormsEntry> entries, int32_t max_doc);

    /// `mutable` because [`CloseableRef::close`] is `const` and drops the handle.
    mutable std::shared_ptr<IndexInput> data_;
    std::map<int32_t, NormsEntry> entries_;
    int32_t max_doc_{0};
};

} // namespace lucene
