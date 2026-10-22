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

export module lucene_core:term_vectors_consumer;

import :types;
import std;

import :byte_block_pool;
import :directory;
import :error;
import :field_infos;
import :int_block_pool;
import :segment_info;
import :term_vectors_consumer_per_field;
import :term_vectors_format;
import :term_vectors_writer;

namespace lucene {

/// Feeds the term vectors of the documents being indexed to the codec.
///
/// Port of `TermVectorsConsumer` (`index/term_vectors_consumer.rs`), reduced to
/// the default (non-sorting) hook: index sorting is not ported, so the
/// `Sorting`/`Test` variants of the reference implementation's `TermVectorsConsumerHook` have no
/// equivalent here.
///
/// The consumer owns the [`TermVectorsConsumerPerField`] of every field of the
/// segment (the reference implementation keeps them in the indexing chain's `PerField` array and
/// reaches them back through it, which the port avoids) and writes their
/// vectors out at the end of each document.
///
/// **Deviations** (C++ mechanics only):
///  * the directory, the format and the segment info are captured at
///    construction instead of being passed to every call;
///  * `flush` takes the document count instead of asking a `SegmentInfo`
///    (the port has no `SegmentInfo::max_doc()` accessor), and the RAM
///    accounting (`Accountable`) is gone.
export class TermVectorsConsumer {
public:
    TermVectorsConsumer(Directory &directory, const TermVectorsFormat &format, SegmentInfo &segment_info);

    TermVectorsConsumer(const TermVectorsConsumer &) = delete;
    TermVectorsConsumer &operator=(const TermVectorsConsumer &) = delete;
    TermVectorsConsumer(TermVectorsConsumer &&) noexcept = delete;
    TermVectorsConsumer &operator=(TermVectorsConsumer &&) noexcept = delete;
    ~TermVectorsConsumer() = default;

    /// Records that at least one field of the segment stores term vectors.
    void set_has_vectors() noexcept { has_vectors_ = true; }
    [[nodiscard]] bool has_vectors() const noexcept { return has_vectors_; }

    /// Id of the next document the codec writer expects.
    [[nodiscard]] int32_t last_doc_id() const noexcept { return last_doc_id_; }

    /// Called at the start of every document.
    [[nodiscard]] VoidResult start_document();

    /// Creates the per-field writer of a field that is indexed for the first
    /// time in this segment.  The returned pointer is owned by the consumer and
    /// stays valid for its lifetime.
    [[nodiscard]] Result<TermVectorsConsumerPerField *> add_field(std::shared_ptr<FieldInfo> field_info);

    /// Called at the end of every document: writes the vectors that were
    /// buffered by the per-field writers.
    ///
    /// `int_pool` / `byte_pool` are the term-vectors pools (`IndexContext`'s
    /// `term_vectors_int_pool` / `term_vectors_byte_pool`), `term_byte_pool` is
    /// the one shared with the postings.
    [[nodiscard]] VoidResult finish_document(int32_t doc_id, IntBlockPool &int_pool, ByteBlockPool &byte_pool,
                                             ByteBlockPool &term_byte_pool);

    /// Writes the empty documents up to `max_doc - 1`, finishes and closes the
    /// codec writer.
    [[nodiscard]] VoidResult flush(int32_t max_doc);

    /// Drops everything buffered and closes the codec writer.
    [[nodiscard]] VoidResult abort(IntBlockPool &int_pool, ByteBlockPool &byte_pool);

    /// Number of per-field writers created for this segment.
    [[nodiscard]] size_t num_per_fields() const noexcept { return per_fields_.size(); }

    /// Whether the codec writer has been created (i.e. a vector was written).
    [[nodiscard]] bool is_initialized() const noexcept { return writer_ != nullptr; }

private:
    /// Emits the empty documents up to `doc_id - 1`.
    [[nodiscard]] VoidResult fill(int32_t doc_id);
    /// Creates the codec writer on first use; resets [`last_doc_id`].
    [[nodiscard]] VoidResult init_writer();

    Directory *directory_{nullptr};
    const TermVectorsFormat *format_{nullptr};
    SegmentInfo *segment_info_{nullptr};
    std::unique_ptr<TermVectorsWriter> writer_;
    bool has_vectors_{false};
    int32_t last_doc_id_{0};
    /// Every per-field writer of the segment, in creation order.
    std::vector<std::unique_ptr<TermVectorsConsumerPerField>> per_fields_;
    /// Scratch list of the per-field writers of the current document, ordered
    /// by field name.
    std::vector<TermVectorsConsumerPerField *> to_flush_;
};

} // namespace lucene
