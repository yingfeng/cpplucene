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

export module lucene_core:lucene90_doc_values_consumer;

import :types;
import :error;

import std;

import :byte_array_output;
import :doc_values_consumer;
import :doc_values_producer;
import :field_infos;
import :index_output;
import :lucene90_doc_values_format;
import :segment_info;
import :segment_write_state;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

/// Tracks the value range of one block of numeric values, and how many bits the
/// packed encoding needs.
///
/// Port of `MinMaxTracker` (`lucene90_doc_values_consumer.rs`).
export struct MinMaxTracker {
    int64_t min{0};
    int64_t max{0};
    int64_t num_values{0};
    /// Bits used by the values accumulated by the finished blocks.
    int64_t space_in_bits{0};

    static MinMaxTracker create() {
        MinMaxTracker tracker;
        tracker.reset();
        return tracker;
    }

    void reset();
    void update_value(int64_t value);
    void update_from(const MinMaxTracker &other);
    /// Adds the bits of the current values to [`space_in_bits`].
    void finish();
    /// [`finish`] and start a new block.
    void next_block();
};

/// One skip-index interval: a doc-id range plus the value range of the documents
/// inside it.
///
/// Port of `SkipAccumulator` (`lucene90_doc_values_consumer.rs`).
export struct SkipAccumulator {
    int32_t min_doc_id{0};
    int32_t max_doc_id{0};
    int32_t doc_count{0};
    int64_t min_value{0};
    int64_t max_value{0};

    explicit SkipAccumulator(int32_t doc_id);

    /// Whether the interval is full and the next document opens a new one.
    [[nodiscard]] bool is_done(int32_t skip_index_interval_size, int32_t value_count, int64_t next_value,
                               int32_t next_doc) const;
    void accumulate_value(int64_t value);
    void accumulate_other(const SkipAccumulator &other);
    void next_doc(int32_t doc_id);

    /// Merges `length` accumulators of `list` starting at `index` into one.
    [[nodiscard]] static SkipAccumulator merge(const std::vector<SkipAccumulator> &list, size_t index, size_t length);
};

/// Writes the `.dvd`/`.dvm` pair of one segment.
///
/// Port of `Lucene90DocValuesConsumer`
/// (`codecs/lucene90/lucene90_doc_values_consumer.rs`).  Both outputs are opened
/// and header-stamped up front; every field appends
/// `fieldNumber, typeByte, ...` to the metadata and its values to the data file,
/// and [`close`] writes the terminating `-1` field number plus both footers.
///
/// The values of a field are always visited as [`SortedNumericDocValues`], so a
/// numeric field is seen as one value per document and a sorted or sorted-set
/// field as its ordinals.
export class Lucene90DocValuesConsumer final : public DocValuesConsumer {
public:
    /// Opens the two outputs for `state`.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90DocValuesConsumer>> create(const SegmentWriteState &state,
                                                                                  const SegmentInfo &segment_info);

    /// Like [`create`] but with an explicit skip-index interval size.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90DocValuesConsumer>> create_with_skip_index_interval(
            const SegmentWriteState &state, const SegmentInfo &segment_info, int32_t skip_index_interval_size);

    ~Lucene90DocValuesConsumer() override;

    Lucene90DocValuesConsumer(const Lucene90DocValuesConsumer &) = delete;
    Lucene90DocValuesConsumer &operator=(const Lucene90DocValuesConsumer &) = delete;

    [[nodiscard]] VoidResult add_numeric_field(const FieldInfo &field, DocValuesProducer &producer) override;

    [[nodiscard]] VoidResult add_binary_field(const FieldInfo &field, DocValuesProducer &producer) override;

    [[nodiscard]] VoidResult add_sorted_field(const FieldInfo &field, DocValuesProducer &producer) override;

    [[nodiscard]] VoidResult add_sorted_numeric_field(const FieldInfo &field, DocValuesProducer &producer) override;

    [[nodiscard]] VoidResult add_sorted_set_field(const FieldInfo &field, DocValuesProducer &producer) override;

    [[nodiscard]] VoidResult close() override;

private:
    Lucene90DocValuesConsumer(std::unique_ptr<IndexOutput> data, std::unique_ptr<IndexOutput> meta, int32_t max_doc,
                              int32_t skip_index_interval_size);

    /// Writes the multi-level skip index of `field` and its summary to the
    /// metadata.  The values are read as sorted numerics.
    [[nodiscard]] VoidResult write_skip_index(const FieldInfo &field, DocValuesProducer &producer);

    /// Collapses `accumulators` into their levels, writes them and empties the
    /// vector.
    [[nodiscard]] VoidResult write_levels(std::vector<SkipAccumulator> &accumulators);

    [[nodiscard]] static std::vector<SkipAccumulator> build_level(const std::vector<SkipAccumulator> &accumulators);

    /// How many levels the accumulator at `index` of a group of `size` needs.
    [[nodiscard]] static size_t get_levels(size_t index, size_t size);

    /// Encodes the values of `field`; returns `(docs_with_value, num_values)`.
    ///
    /// The values come from `producer` as sorted numerics: `ords` says they are
    /// ordinals, which forbids GCD compression and requires the minimum to be 0.
    [[nodiscard]] Result<std::pair<int32_t, int64_t>> write_values(const FieldInfo &field,
                                                                  DocValuesProducer &producer, bool ords);

    [[nodiscard]] VoidResult write_values_single_block(SortedNumericDocValues &values, int64_t num_values,
                                                       int32_t num_bits_per_value, int64_t min, int64_t gcd,
                                                       const std::map<int64_t, int64_t> *encode);

    /// Returns the offset of the block jump table.
    [[nodiscard]] Result<int64_t> write_values_multiple_blocks(SortedNumericDocValues &values, int64_t gcd);

    [[nodiscard]] VoidResult write_block(std::span<const int64_t> values, int64_t gcd, ByteArrayOutput &buffer);

    [[nodiscard]] VoidResult do_add_sorted_field(const FieldInfo &field, DocValuesProducer &producer,
                                                 bool add_type_byte);

    /// Writes the term dictionary of a sorted (`sorted_set == false`) or sorted
    /// set field, followed by its reverse index.
    [[nodiscard]] VoidResult write_terms_dict(const FieldInfo &field, DocValuesProducer &producer, bool sorted_set);

    /// Writes the ordinal-to-term mapping of a sorted or sorted set field.
    [[nodiscard]] VoidResult write_terms_index(const FieldInfo &field, DocValuesProducer &producer, bool sorted_set);

    [[nodiscard]] VoidResult do_add_sorted_numeric_field(const FieldInfo &field, DocValuesProducer &producer,
                                                         bool ords);

    [[nodiscard]] static Result<bool> is_single_valued(SortedSetDocValues &values);

    std::unique_ptr<IndexOutput> data_;
    std::unique_ptr<IndexOutput> meta_;
    int32_t max_doc_{0};
    int32_t skip_index_interval_size_{Lucene90DocValuesFormat::DEFAULT_SKIP_INDEX_INTERVAL_SIZE};
    bool closed_{false};
    /// Scratch buffer for one block of the term dictionary; keeps its capacity
    /// between blocks.
    ByteArrayOutput terms_dict_buffer_{"terms-dict"};
};

} // namespace lucene
