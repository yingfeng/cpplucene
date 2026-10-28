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

export module lucene_core:filter_codec_reader;

import :types;
import std;

import :bits;
import :codec_reader;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :fields_producer;
import :leaf_metadata;
import :norms_producer;
import :points_reader;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

/// Live docs of a filtered reader: either a single bit set or the conjunction
/// of a "hard" one and the one the wrapped reader reported.
///
/// Port of `FilterCodecReaderBits` (`index/filter_codec_reader.rs`).
export class FilterCodecReaderBits final : public Bits {
public:
    /// Wraps `bits` unchanged.
    explicit FilterCodecReaderBits(std::shared_ptr<const Bits> bits);

    /// Concatenates two sets: a document is live only when it is live in both.
    static std::shared_ptr<FilterCodecReaderBits> mixed(std::shared_ptr<const Bits> hard_live_docs,
                                                        std::shared_ptr<const Bits> wrapped_live_docs);

    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override;
    [[nodiscard]] std::string to_string() const override;

private:
    FilterCodecReaderBits(std::shared_ptr<const Bits> hard, std::shared_ptr<const Bits> wrapped, bool mixed);

    std::shared_ptr<const Bits> hard_;
    std::shared_ptr<const Bits> wrapped_;
    bool mixed_;
};

/// A [`CodecReader`] that delegates everything to another one.
///
/// Port of `FilterCodecReaderImpl` (`index/filter_codec_reader.rs`).  Only the
/// six reader getters and the reader level properties are forwarded: the
/// [`CodecReader`] base implementations of `terms`, doc values, stored fields
/// and term vectors are written in terms of them, so they are inherited for
/// free.
///
/// **Deviations:** the delegate is a `shared_ptr` (the reference implementation
/// stores it by value and clones it) and the cache-helper plumbing is not
/// ported.
export class FilterCodecReader : public CodecReader {
public:
    explicit FilterCodecReader(std::shared_ptr<CodecReader> delegate);

    /// The wrapped reader.
    [[nodiscard]] CodecReader &delegate() const noexcept { return *delegate_; }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return delegate_->max_doc(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return delegate_->num_docs(); }
    [[nodiscard]] Result<int32_t> num_deleted_docs() const override { return delegate_->num_deleted_docs(); }
    [[nodiscard]] Result<bool> has_deletions() const override { return delegate_->has_deletions(); }

    // ---- LeafReader ----

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return delegate_->field_infos(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return delegate_->live_docs(); }
    [[nodiscard]] const LeafMetaData &leaf_meta_data() const override { return delegate_->leaf_meta_data(); }
    [[nodiscard]] std::string to_string() const override;

    // ---- CodecReader ----

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return delegate_->get_fields_reader(); }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return delegate_->get_term_vectors_reader(); }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return delegate_->get_norms_reader(); }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return delegate_->get_doc_values_reader(); }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return delegate_->get_postings_reader(); }
    [[nodiscard]] PointsReader *get_points_reader() override { return delegate_->get_points_reader(); }

    [[nodiscard]] VoidResult check_integrity() override { return delegate_->check_integrity(); }

protected:
    std::shared_ptr<CodecReader> delegate_;
};

} // namespace lucene
