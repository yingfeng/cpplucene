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

export module lucene_core:freq_prox_terms_writer;

import :types;
import std;

import :byte_block_pool;
import :error;
import :field_infos;
import :freq_prox_terms_writer_per_field;
import :int_block_pool;
import :term_vectors_consumer;
import :terms_hash;

namespace lucene {

/// The `freq`/`prox` terms hash of one indexing buffer.
///
/// Port of `FreqProxTermsWriter` (`index/freq_prox_terms_writer.rs`), the
/// aggregate that owns the per-field writers.  Like the reference implementation it owns the
/// [`TermVectorsConsumer`] that receives the term vectors; the consumer may be
/// `nullptr` when the indexing buffer does not write term vectors at all.
///
/// **Deviations:**
///  * `flush` is split: [`prepare_fields`] does the part that belongs to the
///    indexing chain (sort every non-empty field, order the fields by name,
///    build a [`FreqProxFields`]); writing the postings through
///    `PostingsFormat` and applying flush deletes belongs to the segment
///    writer, which owns the codec and the write state;
///  * no RAM accounting (`bytes_used`).
export class FreqProxTermsWriter {
public:
    /// Takes ownership of the consumer that receives the term vectors.
    explicit FreqProxTermsWriter(std::unique_ptr<TermVectorsConsumer> next_terms_hash = nullptr);

    FreqProxTermsWriter(const FreqProxTermsWriter &) = delete;
    FreqProxTermsWriter &operator=(const FreqProxTermsWriter &) = delete;
    FreqProxTermsWriter(FreqProxTermsWriter &&) noexcept = default;
    FreqProxTermsWriter &operator=(FreqProxTermsWriter &&) noexcept = default;
    ~FreqProxTermsWriter() = default;

    [[nodiscard]] TermVectorsConsumer *next_terms_hash() const noexcept { return next_terms_hash_.get(); }

    /// Records that at least one field of the segment stores term vectors.
    void set_has_vectors() noexcept { has_vectors_ = true; }
    [[nodiscard]] bool has_vectors() const noexcept { return has_vectors_; }

    /// Creates the per-field writer for a field that is indexed for the first
    /// time in this segment.
    [[nodiscard]] Result<FreqProxTermsWriterPerField> add_field(std::shared_ptr<FieldInfo> field_info);

    /// Called at the start of every document.
    [[nodiscard]] VoidResult start_document();
    /// Called at the end of every document (merges the term vectors).
    [[nodiscard]] VoidResult finish_document(int32_t doc_id, IntBlockPool &int_pool, ByteBlockPool &next_byte_pool,
                                             ByteBlockPool &byte_pool);

    /// Drops everything buffered since the last flush.
    [[nodiscard]] VoidResult abort(IntBlockPool &int_pool, ByteBlockPool &byte_pool, IntBlockPool &next_int_pool,
                                   ByteBlockPool &next_byte_pool);

    /// Sorts the terms of every non-empty field and orders the fields by name.
    [[nodiscard]] Result<std::vector<std::shared_ptr<FreqProxTermsWriterPerField>>>
    prepare_fields(std::vector<FreqProxTermsWriterPerField> fields_to_flush, const ByteBlockPool &byte_pool);

    /// Orders two per-field writers by field name (the reference implementation's `Ord`).
    [[nodiscard]] static int32_t compare(const FreqProxTermsWriterPerField &left,
                                         const FreqProxTermsWriterPerField &right) {
        return left.compare(right);
    }

private:
    /// Owned: the per-field writers keep a borrowed pointer into it.
    std::unique_ptr<TermVectorsConsumer> next_terms_hash_;
    bool has_vectors_{false};
    TermsHash base_;
};

} // namespace lucene
