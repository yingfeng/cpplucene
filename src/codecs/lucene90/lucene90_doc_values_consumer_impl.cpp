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

module lucene_core:lucene90_doc_values_consumer.impl;

import :lucene90_doc_values_consumer;

import :binary_doc_values;
import :bit_util;
import :byte_array_output;
import :bytes_ref;
import :codec_util;
import :data_output;
import :direct_monotonic_writer;
import :direct_writer;
import :directory;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_iterator;
import :doc_values_producer;
import :error;
import :field_infos;
import :index_output;
import :index_types;
import :indexed_disi;
import :lucene90_doc_values_format;
import :lz4;
import :math_util;
import :numeric_doc_values;
import :segment_info;
import :segment_write_state;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :string_helper;
import :types;

import std;

namespace lucene {

namespace {

/// Exposes a doc values iterator as a [`DocIdSetIterator`], which is what
/// [`IndexedDISI::write_bitset`] consumes.
class DocValuesToDisi final : public DocIdSetIterator {
public:
    explicit DocValuesToDisi(DocValuesIterator &values) : values_(values) {}

    [[nodiscard]] int32_t doc_id() const override { return values_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return values_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return values_.advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_.cost(); }

private:
    DocValuesIterator &values_;
};

/// The dictionary of a sorted or sorted set field, as a dense list of terms.
///
/// rlucene walks the dictionary with a `TermsEnum`; here the ordinals are dense
/// and already sorted by term, so reading them in order gives the same sequence
/// without needing the `TermsEnum` adapter of the search layer.
class TermSource {
public:
    virtual ~TermSource() = default;
    [[nodiscard]] virtual Result<int64_t> value_count() = 0;
    [[nodiscard]] virtual Result<BytesRef> term(int64_t ord) = 0;
};

class SortedTermSource final : public TermSource {
public:
    explicit SortedTermSource(std::unique_ptr<SortedDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<int64_t> value_count() override {
        auto count = values_->get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        return static_cast<int64_t>(count.value());
    }

    [[nodiscard]] Result<BytesRef> term(int64_t ord) override { return values_->lookup_ord(static_cast<int32_t>(ord)); }

private:
    std::unique_ptr<SortedDocValues> values_;
};

class SortedSetTermSource final : public TermSource {
public:
    explicit SortedSetTermSource(std::unique_ptr<SortedSetDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<int64_t> value_count() override { return values_->get_value_count(); }

    [[nodiscard]] Result<BytesRef> term(int64_t ord) override { return values_->lookup_ord(ord); }

private:
    std::unique_ptr<SortedSetDocValues> values_;
};

/// A [`NumericDocValues`] seen as a single-valued [`SortedNumericDocValues`].
class NumericAsSortedNumeric final : public SortedNumericDocValues {
public:
    explicit NumericAsSortedNumeric(std::unique_ptr<NumericDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return values_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return values_->advance_exact(target); }
    [[nodiscard]] Result<int64_t> next_value() override { return values_->long_value(); }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return 1; }
    [[nodiscard]] bool is_single_valued() const override { return true; }

private:
    std::unique_ptr<NumericDocValues> values_;
};

/// A [`SortedDocValues`] seen as a single-valued [`SortedNumericDocValues`] of its
/// ordinals.
class SortedAsSortedNumeric final : public SortedNumericDocValues {
public:
    explicit SortedAsSortedNumeric(std::unique_ptr<SortedDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return values_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return values_->advance_exact(target); }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return 1; }
    [[nodiscard]] bool is_single_valued() const override { return true; }

    [[nodiscard]] Result<int64_t> next_value() override {
        auto ord = values_->ord_value();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return static_cast<int64_t>(ord.value());
    }

private:
    std::unique_ptr<SortedDocValues> values_;
};

/// A [`SortedSetDocValues`] seen as a [`SortedNumericDocValues`] of its ordinals.
class SortedSetAsSortedNumeric final : public SortedNumericDocValues {
public:
    SortedSetAsSortedNumeric(std::unique_ptr<SortedSetDocValues> values, bool single_valued)
        : values_(std::move(values)), single_valued_(single_valued) {}

    [[nodiscard]] int32_t doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return values_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return values_->advance_exact(target); }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return values_->doc_value_count(); }
    [[nodiscard]] bool is_single_valued() const override { return single_valued_; }
    [[nodiscard]] Result<int64_t> next_value() override { return values_->next_ord(); }

private:
    std::unique_ptr<SortedSetDocValues> values_;
    bool single_valued_;
};

/// A single-valued [`SortedSetDocValues`] seen as a [`SortedDocValues`], taking
/// the document's minimum ordinal (rlucene's `SortedSetSelector::Min`).
class SortedSetAsSorted final : public SortedDocValues {
public:
    explicit SortedSetAsSorted(std::unique_ptr<SortedSetDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return values_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return values_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return values_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return values_->advance_exact(target); }
    [[nodiscard]] Result<int32_t> get_value_count() const override {
        auto count = values_->get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        return static_cast<int32_t>(count.value());
    }
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override { return values_->lookup_ord(ord); }

    [[nodiscard]] Result<int32_t> ord_value() override {
        auto ord = values_->next_ord();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return static_cast<int32_t>(ord.value());
    }

private:
    std::unique_ptr<SortedSetDocValues> values_;
};

/// Projects a [`DocValuesProducer`] onto the accessor the values writer needs.
///
/// rlucene does this with one borrowed producer per case
/// (`EmptyDocValuesProducerSub1..4`); one class with a mode keeps the four
/// behaviours together.
class DocValuesView final : public DocValuesProducer {
public:
    enum class Kind {
        /// A numeric field: one value per document.
        kNumeric,
        /// A sorted field: the value is the document's ordinal.
        kSorted,
        /// A single-valued sorted set field: ordinals, and a sorted view.
        kSortedSetSingleValued,
        /// A multi-valued sorted set field: all ordinals of a document.
        kSortedSetMultiValued,
    };

    DocValuesView(DocValuesProducer &base, Kind kind) : base_(base), kind_(kind) {}

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        switch (kind_) {
            case Kind::kNumeric: {
                auto values = base_.get_numeric(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                return std::unique_ptr<SortedNumericDocValues>(new NumericAsSortedNumeric(std::move(values.value())));
            }
            case Kind::kSorted: {
                auto values = base_.get_sorted(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                return std::unique_ptr<SortedNumericDocValues>(new SortedAsSortedNumeric(std::move(values.value())));
            }
            case Kind::kSortedSetSingleValued:
            case Kind::kSortedSetMultiValued: {
                auto values = base_.get_sorted_set(field);
                if (!values.has_value()) {
                    return std::unexpected(std::move(values.error()));
                }
                return std::unique_ptr<SortedNumericDocValues>(new SortedSetAsSortedNumeric(
                        std::move(values.value()), kind_ == Kind::kSortedSetSingleValued));
            }
        }
        return err(LuceneError::illegal_state("unknown doc values view"));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        if (kind_ == Kind::kSorted) {
            return base_.get_sorted(field);
        }
        if (kind_ != Kind::kSortedSetSingleValued) {
            return err(LuceneError::illegal_state("this view only exposes sorted numerics"));
        }
        auto values = base_.get_sorted_set(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        return std::unique_ptr<SortedDocValues>(new SortedSetAsSorted(std::move(values.value())));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return base_.check_integrity(); }

    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    DocValuesProducer &base_;
    Kind kind_;
};

/// Compresses the block of terms buffered after the first `dict_length` bytes and
/// appends it to `data`; returns the uncompressed length.
Result<int32_t> compress_and_get_terms_dict_block(ByteArrayOutput &buffer, size_t dict_length,
                                                  FastCompressionHashTable &hash_table, IndexOutput &data) {
    if (buffer.size() < dict_length ||
        buffer.size() - dict_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return err(LuceneError::illegal_state("the terms dictionary block has an impossible length"));
    }
    const auto uncompressed_length = static_cast<int32_t>(buffer.size() - dict_length);
    if (auto written = data.write_vint(uncompressed_length); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto compressed = LZ4::compress_with_dictionary(buffer.bytes(), 0, static_cast<int32_t>(dict_length),
                                                         uncompressed_length, data, hash_table);
        !compressed.has_value()) {
        return std::unexpected(std::move(compressed.error()));
    }
    return uncompressed_length;
}

} // namespace

void MinMaxTracker::reset() {
    min = std::numeric_limits<int64_t>::max();
    max = std::numeric_limits<int64_t>::min();
    num_values = 0;
}

void MinMaxTracker::update_value(int64_t value) {
    min = std::min(min, value);
    max = std::max(max, value);
    ++num_values;
}

void MinMaxTracker::update_from(const MinMaxTracker &other) {
    min = std::min(min, other.min);
    max = std::max(max, other.max);
    num_values += other.num_values;
}

void MinMaxTracker::finish() {
    if (max > min) {
        const auto bits = DirectWriter::unsigned_bits_required(
                static_cast<int64_t>(static_cast<uint64_t>(max) - static_cast<uint64_t>(min)));
        space_in_bits += static_cast<int64_t>(bits) * num_values;
    }
}

void MinMaxTracker::next_block() {
    finish();
    reset();
}

SkipAccumulator::SkipAccumulator(int32_t doc_id)
    : min_doc_id(doc_id),
      max_doc_id(doc_id),
      doc_count(0),
      min_value(std::numeric_limits<int64_t>::max()),
      max_value(std::numeric_limits<int64_t>::min()) {}

bool SkipAccumulator::is_done(int32_t skip_index_interval_size, int32_t value_count, int64_t next_value,
                              int32_t next_doc) const {
    if (doc_count < skip_index_interval_size) {
        return false;
    }
    // Once the interval is full it keeps growing as long as the documents stay
    // single-valued, keep the same value and leave no gaps.
    return value_count > 1 || min_value != max_value || min_value != next_value || doc_count != next_doc - min_doc_id;
}

void SkipAccumulator::accumulate_value(int64_t value) {
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
}

void SkipAccumulator::accumulate_other(const SkipAccumulator &other) {
    max_doc_id = other.max_doc_id;
    min_value = std::min(min_value, other.min_value);
    max_value = std::max(max_value, other.max_value);
    doc_count += other.doc_count;
}

void SkipAccumulator::next_doc(int32_t doc_id) {
    max_doc_id = doc_id;
    ++doc_count;
}

SkipAccumulator SkipAccumulator::merge(const std::vector<SkipAccumulator> &list, size_t index, size_t length) {
    SkipAccumulator merged(list[index].min_doc_id);
    for (size_t i = 0; i < length; ++i) {
        merged.accumulate_other(list[index + i]);
    }
    return merged;
}

Lucene90DocValuesConsumer::Lucene90DocValuesConsumer(std::unique_ptr<IndexOutput> data,
                                                    std::unique_ptr<IndexOutput> meta, int32_t max_doc,
                                                    int32_t skip_index_interval_size)
    : data_(std::move(data)), meta_(std::move(meta)), max_doc_(max_doc),
      skip_index_interval_size_(skip_index_interval_size) {}

Lucene90DocValuesConsumer::~Lucene90DocValuesConsumer() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90DocValuesConsumer>> Lucene90DocValuesConsumer::create(const SegmentWriteState &state,
                                                                                    const SegmentInfo &segment_info) {
    return create_with_skip_index_interval(state, segment_info,
                                           Lucene90DocValuesFormat::DEFAULT_SKIP_INDEX_INTERVAL_SIZE);
}

Result<std::unique_ptr<Lucene90DocValuesConsumer>> Lucene90DocValuesConsumer::create_with_skip_index_interval(
        const SegmentWriteState &state, const SegmentInfo &segment_info, int32_t skip_index_interval_size) {
    if (skip_index_interval_size < 2) {
        return err(LuceneError::illegal_argument("the skip index interval size must be greater than 1, got " +
                                                 std::to_string(skip_index_interval_size)));
    }

    const std::span<const uint8_t> id(segment_info.id);
    const std::string &suffix = state.segment_suffix;

    const std::string data_file =
            segment_file_name(segment_info.name, suffix, Lucene90DocValuesFormat::DATA_EXTENSION);
    auto data = state.directory.create_output(data_file, state.context);
    if (!data.has_value()) {
        return std::unexpected(std::move(data.error()));
    }
    if (auto written = CodecUtil::write_index_header(*data.value(), Lucene90DocValuesFormat::DATA_CODEC,
                                                      Lucene90DocValuesFormat::VERSION_CURRENT, id, suffix);
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    const std::string meta_file =
            segment_file_name(segment_info.name, suffix, Lucene90DocValuesFormat::META_EXTENSION);
    auto meta = state.directory.create_output(meta_file, state.context);
    if (!meta.has_value()) {
        return std::unexpected(std::move(meta.error()));
    }
    if (auto written = CodecUtil::write_index_header(*meta.value(), Lucene90DocValuesFormat::META_CODEC,
                                                      Lucene90DocValuesFormat::VERSION_CURRENT, id, suffix);
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    std::unique_ptr<Lucene90DocValuesConsumer> consumer(new Lucene90DocValuesConsumer(
            std::move(data.value()), std::move(meta.value()), segment_info.max_doc, skip_index_interval_size));
    return consumer;
}

VoidResult Lucene90DocValuesConsumer::write_levels(std::vector<SkipAccumulator> &accumulators) {
    std::array<std::vector<SkipAccumulator>, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> levels;
    const size_t total = accumulators.size();
    levels[0] = std::move(accumulators);
    for (size_t i = 0; i + 1 < Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL; ++i) {
        levels[i + 1] = build_level(levels[i]);
    }

    for (size_t index = 0; index < total; ++index) {
        const size_t level_count = get_levels(index, total);
        if (auto written = data_->write_byte(static_cast<uint8_t>(level_count)); !written.has_value()) {
            return written;
        }
        // The intervals are written from the coarsest level down, so a reader that
        // only needs a coarse skip can stop early.
        for (size_t level = level_count; level > 0; --level) {
            const size_t level_index = level - 1;
            const size_t idx = index >> (Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT * level_index);
            if (idx >= levels[level_index].size()) {
                return err(LuceneError::illegal_state("the skip index level is missing an interval"));
            }
            const SkipAccumulator &accumulator = levels[level_index][idx];
            if (auto written = data_->write_int(accumulator.max_doc_id); !written.has_value()) {
                return written;
            }
            if (auto written = data_->write_int(accumulator.min_doc_id); !written.has_value()) {
                return written;
            }
            if (auto written = data_->write_long(accumulator.max_value); !written.has_value()) {
                return written;
            }
            if (auto written = data_->write_long(accumulator.min_value); !written.has_value()) {
                return written;
            }
            if (auto written = data_->write_int(accumulator.doc_count); !written.has_value()) {
                return written;
            }
        }
    }

    accumulators.clear();
    return {};
}

std::vector<SkipAccumulator> Lucene90DocValuesConsumer::build_level(
        const std::vector<SkipAccumulator> &accumulators) {
    const size_t level_size = size_t{1} << Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT;
    std::vector<SkipAccumulator> collector;
    collector.reserve(accumulators.size() / level_size);
    for (size_t group = 0; group < accumulators.size() / level_size; ++group) {
        collector.push_back(SkipAccumulator::merge(accumulators, group * level_size, level_size));
    }
    return collector;
}

size_t Lucene90DocValuesConsumer::get_levels(size_t index, size_t size) {
    if (std::countr_zero(index) >= Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT) {
        const size_t left = size - index;
        for (size_t level = Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL; level > 1; --level) {
            const size_t level_index = level - 1;
            const size_t intervals = size_t{1} << (Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT * level_index);
            if (left >= intervals && index % intervals == 0) {
                return level;
            }
        }
    }
    return 1;
}

VoidResult Lucene90DocValuesConsumer::write_skip_index(const FieldInfo &field, DocValuesProducer &producer) {
    auto start = data_->get_file_pointer();
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }

    auto values = producer.get_sorted_numeric(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }

    int64_t global_max_value = std::numeric_limits<int64_t>::min();
    int64_t global_min_value = std::numeric_limits<int64_t>::max();
    int32_t global_doc_count = 0;
    int32_t max_doc_id = -1;

    std::vector<SkipAccumulator> accumulators;
    const size_t max_accumulators =
            size_t{1} << (Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT *
                          (Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL - 1));
    std::optional<SkipAccumulator> accumulator;

    auto doc = values.value()->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto first_value = values.value()->next_value();
        if (!first_value.has_value()) {
            return std::unexpected(std::move(first_value.error()));
        }
        auto value_count = values.value()->doc_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }

        if (accumulator.has_value() && accumulator->is_done(skip_index_interval_size_, value_count.value(),
                                                            first_value.value(), doc.value())) {
            global_max_value = std::max(global_max_value, accumulator->max_value);
            global_min_value = std::min(global_min_value, accumulator->min_value);
            global_doc_count += accumulator->doc_count;
            max_doc_id = accumulator->max_doc_id;
            accumulators.push_back(*accumulator);
            accumulator.reset();
            if (accumulators.size() == max_accumulators) {
                if (auto written = write_levels(accumulators); !written.has_value()) {
                    return written;
                }
            }
        }

        if (!accumulator.has_value()) {
            accumulator.emplace(doc.value());
        }
        accumulator->next_doc(doc.value());
        accumulator->accumulate_value(first_value.value());
        for (int32_t i = 1; i < value_count.value(); ++i) {
            auto value = values.value()->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            accumulator->accumulate_value(value.value());
        }

        doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }

    if (accumulator.has_value()) {
        global_max_value = std::max(global_max_value, accumulator->max_value);
        global_min_value = std::min(global_min_value, accumulator->min_value);
        global_doc_count += accumulator->doc_count;
        max_doc_id = accumulator->max_doc_id;
        accumulators.push_back(*accumulator);
    }
    if (!accumulators.empty()) {
        if (auto written = write_levels(accumulators); !written.has_value()) {
            return written;
        }
    }

    auto end = data_->get_file_pointer();
    if (!end.has_value()) {
        return std::unexpected(std::move(end.error()));
    }
    // The metadata holds the location of the intervals plus their summary.
    if (auto written = meta_->write_long(static_cast<int64_t>(start.value())); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(end.value() - start.value())); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_long(global_max_value); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_long(global_min_value); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_int(global_doc_count); !written.has_value()) {
        return written;
    }
    return meta_->write_int(max_doc_id);
}

Result<std::pair<int32_t, int64_t>> Lucene90DocValuesConsumer::write_values(const FieldInfo &field,
                                                                           DocValuesProducer &producer, bool ords) {
    auto first_values = producer.get_sorted_numeric(field);
    if (!first_values.has_value()) {
        return std::unexpected(std::move(first_values.error()));
    }
    int64_t first_value = 0;
    {
        auto doc = first_values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() != NO_MORE_DOCS) {
            auto value = first_values.value()->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            first_value = value.value();
        }
    }

    auto values = producer.get_sorted_numeric(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }

    int32_t num_docs_with_value = 0;
    MinMaxTracker min_max = MinMaxTracker::create();
    MinMaxTracker block_min_max = MinMaxTracker::create();
    int64_t gcd = 0;
    std::optional<std::set<int64_t>> unique_values;
    if (!ords) {
        unique_values.emplace();
    }

    auto doc = values.value()->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto value_count = values.value()->doc_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }
        for (int32_t i = 0; i < value_count.value(); ++i) {
            auto value = values.value()->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            const int64_t v = value.value();

            if (gcd != 1) {
                if (v < std::numeric_limits<int64_t>::min() / 2 || v > std::numeric_limits<int64_t>::max() / 2) {
                    // The difference to the first value could overflow, and a wrong
                    // GCD would corrupt the values, so compression gives up here.
                    gcd = 1;
                } else {
                    gcd = MathUtil::gcd(gcd, static_cast<int64_t>(static_cast<uint64_t>(v) -
                                                                  static_cast<uint64_t>(first_value)));
                }
            }

            block_min_max.update_value(v);
            if (block_min_max.num_values == Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE) {
                min_max.update_from(block_min_max);
                block_min_max.next_block();
            }

            if (unique_values.has_value()) {
                const bool inserted = unique_values->insert(v).second;
                if (inserted && unique_values->size() > 256) {
                    unique_values.reset();
                }
            }
        }

        ++num_docs_with_value;
        doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }

    min_max.update_from(block_min_max);
    min_max.finish();
    block_min_max.finish();

    if (ords && min_max.num_values > 0) {
        if (min_max.min != 0) {
            return err(LuceneError::illegal_state("the minimum ordinal must be 0, got " + std::to_string(min_max.min)));
        }
        if (min_max.max != 0 && gcd != 1) {
            return err(LuceneError::illegal_state("GCD compression must never be used on ordinals, got gcd=" +
                                                  std::to_string(gcd)));
        }
    }

    const int64_t num_values = min_max.num_values;
    int64_t min = min_max.min;
    const int64_t max = min_max.max;

    auto data_pointer = data_->get_file_pointer();
    if (!data_pointer.has_value()) {
        return std::unexpected(std::move(data_pointer.error()));
    }

    if (num_docs_with_value == 0) {
        if (auto written = meta_->write_long(-2); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_long(0); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_short(-1); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(-1)); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    } else if (num_docs_with_value == max_doc_) {
        if (auto written = meta_->write_long(-1); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_long(0); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_short(-1); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(-1)); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    } else {
        const size_t offset = data_pointer.value();
        if (auto written = meta_->write_long(static_cast<int64_t>(offset)); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        auto docs = producer.get_sorted_numeric(field);
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }
        DocValuesToDisi doc_ids(*docs.value());
        auto bitset = IndexedDISI::write_bitset(doc_ids, *data_, DEFAULT_DENSE_RANK_POWER);
        if (!bitset.has_value()) {
            return std::unexpected(std::move(bitset.error()));
        }
        auto after = data_->get_file_pointer();
        if (!after.has_value()) {
            return std::unexpected(std::move(after.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(after.value() - offset)); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_short(static_cast<int16_t>(bitset.value().jump_table_entry_count));
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(bitset.value().dense_rank_power));
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    }

    if (auto written = meta_->write_long(num_values); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    int32_t num_bits_per_value = 0;
    bool do_blocks = false;
    std::optional<std::map<int64_t, int64_t>> encode;

    const auto wrapping_sub = [](int64_t left, int64_t right) {
        return static_cast<int64_t>(static_cast<uint64_t>(left) - static_cast<uint64_t>(right));
    };

    if (min >= max) {
        // Every document has the same value, which the metadata stores below.
        num_bits_per_value = 0;
        if (auto written = meta_->write_int(-1); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    } else {
        bool use_table = false;
        if (unique_values.has_value() && unique_values->size() > 1) {
            const auto range_bits = DirectWriter::unsigned_bits_required(
                    wrapping_sub(max, min) / (gcd == 0 ? 1 : gcd));
            const auto table_bits = DirectWriter::unsigned_bits_required(static_cast<int64_t>(unique_values->size()) - 1);
            use_table = table_bits < range_bits;
        }

        if (use_table) {
            const auto set_len = static_cast<int32_t>(unique_values->size());
            num_bits_per_value = DirectWriter::unsigned_bits_required(static_cast<int64_t>(set_len) - 1);
            if (auto written = meta_->write_int(set_len); !written.has_value()) {
                return std::unexpected(std::move(written.error()));
            }
            std::map<int64_t, int64_t> value_to_ord;
            int64_t ord = 0;
            for (const int64_t value : *unique_values) {
                if (auto written = meta_->write_long(value); !written.has_value()) {
                    return std::unexpected(std::move(written.error()));
                }
                value_to_ord.insert_or_assign(value, ord);
                ++ord;
            }
            encode = std::move(value_to_ord);
            min = 0;
            gcd = 1;
        } else {
            // Blocks pay off only when they shrink the encoding by at least 10%.
            do_blocks = min_max.space_in_bits > 0 && static_cast<double>(block_min_max.space_in_bits) /
                                                                  static_cast<double>(min_max.space_in_bits) <=
                                                              0.9;
            if (do_blocks) {
                num_bits_per_value = 0xFF;
                if (auto written = meta_->write_int(-2 - Lucene90DocValuesFormat::NUMERIC_BLOCK_SHIFT);
                    !written.has_value()) {
                    return std::unexpected(std::move(written.error()));
                }
            } else {
                num_bits_per_value =
                        DirectWriter::unsigned_bits_required(wrapping_sub(max, min) / (gcd == 0 ? 1 : gcd));
                if (gcd == 1 && min > 0 &&
                    DirectWriter::unsigned_bits_required(max) ==
                            DirectWriter::unsigned_bits_required(wrapping_sub(max, min))) {
                    // Shifting the range down to zero saves nothing here, so keep the
                    // original minimum and do not shift at all.
                    min = 0;
                }
                if (auto written = meta_->write_int(-1); !written.has_value()) {
                    return std::unexpected(std::move(written.error()));
                }
            }
        }
    }

    if (auto written = meta_->write_byte(static_cast<uint8_t>(num_bits_per_value)); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto written = meta_->write_long(min); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto written = meta_->write_long(gcd); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    auto start_offset = data_->get_file_pointer();
    if (!start_offset.has_value()) {
        return std::unexpected(std::move(start_offset.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(start_offset.value())); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    int64_t jump_table_offset = -1;
    if (do_blocks) {
        auto block_values = producer.get_sorted_numeric(field);
        if (!block_values.has_value()) {
            return std::unexpected(std::move(block_values.error()));
        }
        auto written_jump_table = write_values_multiple_blocks(*block_values.value(), gcd);
        if (!written_jump_table.has_value()) {
            return std::unexpected(std::move(written_jump_table.error()));
        }
        jump_table_offset = written_jump_table.value();
    } else if (num_bits_per_value != 0) {
        auto single_values = producer.get_sorted_numeric(field);
        if (!single_values.has_value()) {
            return std::unexpected(std::move(single_values.error()));
        }
        if (auto written = write_values_single_block(*single_values.value(), num_values, num_bits_per_value, min, gcd,
                                                     encode.has_value() ? &encode.value() : nullptr);
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    }

    auto values_end = data_->get_file_pointer();
    if (!values_end.has_value()) {
        return std::unexpected(std::move(values_end.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(values_end.value() - start_offset.value()));
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto written = meta_->write_long(jump_table_offset); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    return std::make_pair(num_docs_with_value, num_values);
}

VoidResult Lucene90DocValuesConsumer::write_values_single_block(SortedNumericDocValues &values, int64_t num_values,
                                                                int32_t num_bits_per_value, int64_t min, int64_t gcd,
                                                                const std::map<int64_t, int64_t> *encode) {
    auto writer = DirectWriter::get_instance(*data_, num_values, num_bits_per_value);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }

    auto doc = values.next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto value_count = values.doc_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }
        for (int32_t i = 0; i < value_count.value(); ++i) {
            auto value = values.next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            int64_t encoded = 0;
            if (encode != nullptr) {
                auto found = encode->find(value.value());
                encoded = found == encode->end() ? 0 : found->second;
            } else {
                encoded = static_cast<int64_t>(static_cast<uint64_t>(value.value()) - static_cast<uint64_t>(min)) /
                          gcd;
            }
            if (auto added = writer.value().add(encoded); !added.has_value()) {
                return added;
            }
        }
        doc = values.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }
    return writer.value().finish();
}

Result<int64_t> Lucene90DocValuesConsumer::write_values_multiple_blocks(SortedNumericDocValues &values, int64_t gcd) {
    std::vector<int64_t> offsets;
    std::vector<int64_t> buffer(static_cast<size_t>(Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE), 0);
    ByteArrayOutput encode_buffer("encode");
    size_t upto = 0;

    auto doc = values.next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto value_count = values.doc_value_count();
        if (!value_count.has_value()) {
            return std::unexpected(std::move(value_count.error()));
        }
        for (int32_t i = 0; i < value_count.value(); ++i) {
            auto value = values.next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            buffer[upto++] = value.value();
            if (upto == static_cast<size_t>(Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE)) {
                auto position = data_->get_file_pointer();
                if (!position.has_value()) {
                    return std::unexpected(std::move(position.error()));
                }
                offsets.push_back(static_cast<int64_t>(position.value()));
                if (auto written = write_block(std::span<const int64_t>(buffer), gcd, encode_buffer);
                    !written.has_value()) {
                    return std::unexpected(std::move(written.error()));
                }
                upto = 0;
            }
        }
        doc = values.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }

    if (upto > 0) {
        auto position = data_->get_file_pointer();
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        offsets.push_back(static_cast<int64_t>(position.value()));
        if (auto written = write_block(std::span<const int64_t>(buffer.data(), upto), gcd, encode_buffer);
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    }

    // Every block is written; append the offset jump table and remember where it
    // starts, which is the value the metadata stores.
    auto offsets_origo = data_->get_file_pointer();
    if (!offsets_origo.has_value()) {
        return std::unexpected(std::move(offsets_origo.error()));
    }
    for (const int64_t offset : offsets) {
        if (auto written = data_->write_long(offset); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
    }
    if (auto written = data_->write_long(static_cast<int64_t>(offsets_origo.value())); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return static_cast<int64_t>(offsets_origo.value());
}

VoidResult Lucene90DocValuesConsumer::write_block(std::span<const int64_t> values, int64_t gcd,
                                                  ByteArrayOutput &buffer) {
    if (values.empty()) {
        return err(LuceneError::illegal_state("a numeric block must hold at least one value"));
    }

    int64_t min = values[0];
    int64_t max = values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        min = std::min(min, values[i]);
        max = std::max(max, values[i]);
    }

    if (min == max) {
        if (auto written = data_->write_byte(0); !written.has_value()) {
            return written;
        }
        return data_->write_long(min);
    }

    const int32_t bits_per_value = DirectWriter::unsigned_bits_required(
            static_cast<int64_t>(static_cast<uint64_t>(max) - static_cast<uint64_t>(min)) / gcd);
    buffer.reset();

    auto writer = DirectWriter::get_instance(buffer, static_cast<int64_t>(values.size()), bits_per_value);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    for (const int64_t value : values) {
        const int64_t encoded =
                static_cast<int64_t>(static_cast<uint64_t>(value) - static_cast<uint64_t>(min)) / gcd;
        if (auto added = writer.value().add(encoded); !added.has_value()) {
            return added;
        }
    }
    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }

    if (auto written = data_->write_byte(static_cast<uint8_t>(bits_per_value)); !written.has_value()) {
        return written;
    }
    if (auto written = data_->write_long(min); !written.has_value()) {
        return written;
    }
    if (auto written = data_->write_int(static_cast<int32_t>(buffer.size())); !written.has_value()) {
        return written;
    }
    return buffer.copy_to(*data_);
}

Result<bool> Lucene90DocValuesConsumer::is_single_valued(SortedSetDocValues &values) {
    auto doc = values.next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto count = values.doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        if (count.value() > 1) {
            return false;
        }
        doc = values.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }
    return true;
}

VoidResult Lucene90DocValuesConsumer::write_terms_dict(const FieldInfo &field, DocValuesProducer &producer,
                                                       bool sorted_set) {
    std::unique_ptr<TermSource> source;
    if (sorted_set) {
        auto values = producer.get_sorted_set(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        source = std::make_unique<SortedSetTermSource>(std::move(values.value()));
    } else {
        auto values = producer.get_sorted(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        source = std::make_unique<SortedTermSource>(std::move(values.value()));
    }

    auto size = source->value_count();
    if (!size.has_value()) {
        return std::unexpected(std::move(size.error()));
    }
    if (auto written = meta_->write_vlong(size.value()); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_int(Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT); !written.has_value()) {
        return written;
    }

    ByteArrayOutput address_output("terms-addresses");
    const int64_t block_mask = Lucene90DocValuesFormat::TERMS_DICT_BLOCK_LZ4_MASK;
    const int32_t shift = Lucene90DocValuesFormat::TERMS_DICT_BLOCK_LZ4_SHIFT;
    const int64_t num_blocks = (size.value() + block_mask) >> shift;

    auto writer = DirectMonotonicWriter::get_instance(*meta_, address_output, num_blocks,
                                                       Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }

    std::vector<uint8_t> previous;
    terms_dict_buffer_.reset();
    size_t dict_length = 0;
    int32_t max_length = 0;
    int32_t max_block_length = 0;
    FastCompressionHashTable hash_table;

    auto start = data_->get_file_pointer();
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }

    for (int64_t ord = 0; ord < size.value(); ++ord) {
        auto term = source->term(ord);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        const std::span<const uint8_t> term_bytes = term.value().as_slice();
        const size_t length = term_bytes.size();

        if ((ord & block_mask) == 0) {
            if (ord != 0) {
                auto uncompressed = compress_and_get_terms_dict_block(terms_dict_buffer_, dict_length, hash_table,
                                                                      *data_);
                if (!uncompressed.has_value()) {
                    return std::unexpected(std::move(uncompressed.error()));
                }
                max_block_length = std::max(max_block_length, uncompressed.value());
                terms_dict_buffer_.reset();
            }

            auto position = data_->get_file_pointer();
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            if (auto added = writer.value().add(static_cast<int64_t>(position.value() - start.value()));
                !added.has_value()) {
                return added;
            }
            // The first term of a block is written verbatim and doubles as the
            // dictionary of the compressed suffixes.
            if (auto written = data_->write_vint(static_cast<int32_t>(length)); !written.has_value()) {
                return written;
            }
            if (auto written = data_->write_bytes(term_bytes, 0, length); !written.has_value()) {
                return written;
            }
            if (auto written = terms_dict_buffer_.write_bytes(term_bytes, 0, length); !written.has_value()) {
                return written;
            }
            dict_length = length;
        } else {
            auto prefix_length = StringHelper::bytes_difference(previous, term_bytes);
            if (!prefix_length.has_value()) {
                return std::unexpected(std::move(prefix_length.error()));
            }
            const size_t suffix_length = length - prefix_length.value();
            if (suffix_length == 0) {
                return err(LuceneError::illegal_state("the terms dictionary is not strictly increasing"));
            }
            const uint8_t prefix_code = static_cast<uint8_t>(std::min<size_t>(prefix_length.value(), 15));
            const uint8_t suffix_code = static_cast<uint8_t>(std::min<size_t>(suffix_length - 1, 15) << 4);
            if (auto written = terms_dict_buffer_.write_byte(static_cast<uint8_t>(prefix_code | suffix_code));
                !written.has_value()) {
                return written;
            }
            if (prefix_length.value() >= 15) {
                if (auto written = terms_dict_buffer_.write_vint(static_cast<int32_t>(prefix_length.value() - 15));
                    !written.has_value()) {
                    return written;
                }
            }
            if (suffix_length >= 16) {
                if (auto written = terms_dict_buffer_.write_vint(static_cast<int32_t>(suffix_length - 16));
                    !written.has_value()) {
                    return written;
                }
            }
            if (auto written = terms_dict_buffer_.write_bytes(term_bytes, prefix_length.value(), suffix_length);
                !written.has_value()) {
                return written;
            }
        }

        max_length = std::max(max_length, static_cast<int32_t>(length));
        previous.assign(term_bytes.begin(), term_bytes.end());
    }

    // The last block is only compressed when it holds suffixes.
    if (terms_dict_buffer_.size() > dict_length) {
        auto uncompressed = compress_and_get_terms_dict_block(terms_dict_buffer_, dict_length, hash_table, *data_);
        if (!uncompressed.has_value()) {
            return std::unexpected(std::move(uncompressed.error()));
        }
        max_block_length = std::max(max_block_length, uncompressed.value());
    }

    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }

    if (auto written = meta_->write_int(max_length); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_int(max_block_length); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(start.value())); !written.has_value()) {
        return written;
    }
    auto dict_end = data_->get_file_pointer();
    if (!dict_end.has_value()) {
        return std::unexpected(std::move(dict_end.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(dict_end.value() - start.value()));
        !written.has_value()) {
        return written;
    }

    auto addresses_start = data_->get_file_pointer();
    if (!addresses_start.has_value()) {
        return std::unexpected(std::move(addresses_start.error()));
    }
    if (auto copied = address_output.copy_to(*data_); !copied.has_value()) {
        return copied;
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(addresses_start.value())); !written.has_value()) {
        return written;
    }
    auto addresses_end = data_->get_file_pointer();
    if (!addresses_end.has_value()) {
        return std::unexpected(std::move(addresses_end.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(addresses_end.value() - addresses_start.value()));
        !written.has_value()) {
        return written;
    }

    return write_terms_index(field, producer, sorted_set);
}

VoidResult Lucene90DocValuesConsumer::write_terms_index(const FieldInfo &field, DocValuesProducer &producer,
                                                        bool sorted_set) {
    std::unique_ptr<TermSource> source;
    if (sorted_set) {
        auto values = producer.get_sorted_set(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        source = std::make_unique<SortedSetTermSource>(std::move(values.value()));
    } else {
        auto values = producer.get_sorted(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        source = std::make_unique<SortedTermSource>(std::move(values.value()));
    }

    auto size = source->value_count();
    if (!size.has_value()) {
        return std::unexpected(std::move(size.error()));
    }
    if (auto written = meta_->write_int(Lucene90DocValuesFormat::TERMS_DICT_REVERSE_INDEX_SHIFT);
        !written.has_value()) {
        return written;
    }

    auto start = data_->get_file_pointer();
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }
    const int64_t reverse_mask = Lucene90DocValuesFormat::TERMS_DICT_REVERSE_INDEX_MASK;
    const int32_t reverse_shift = Lucene90DocValuesFormat::TERMS_DICT_REVERSE_INDEX_SHIFT;
    const int64_t num_blocks = 1 + ((size.value() + reverse_mask) >> reverse_shift);

    ByteArrayOutput address_output("terms-index-addresses");
    auto writer = DirectMonotonicWriter::get_instance(*meta_, address_output, num_blocks,
                                                       Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }

    std::vector<uint8_t> previous;
    int64_t offset = 0;
    for (int64_t ord = 0; ord < size.value(); ++ord) {
        auto term = source->term(ord);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        const std::span<const uint8_t> term_bytes = term.value().as_slice();

        if ((ord & reverse_mask) == 0) {
            if (auto added = writer.value().add(offset); !added.has_value()) {
                return added;
            }
            if (ord != 0) {
                // Only the bytes that decide the order of this term are stored.
                auto sort_key_length = StringHelper::sort_key_length(previous, term_bytes);
                if (!sort_key_length.has_value()) {
                    return std::unexpected(std::move(sort_key_length.error()));
                }
                offset += static_cast<int64_t>(sort_key_length.value());
                if (auto written = data_->write_bytes(term_bytes, 0, sort_key_length.value()); !written.has_value()) {
                    return written;
                }
            }
        } else if ((ord & reverse_mask) == reverse_mask) {
            previous.assign(term_bytes.begin(), term_bytes.end());
        }
    }

    if (auto added = writer.value().add(offset); !added.has_value()) {
        return added;
    }
    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }

    if (auto written = meta_->write_long(static_cast<int64_t>(start.value())); !written.has_value()) {
        return written;
    }
    auto index_end = data_->get_file_pointer();
    if (!index_end.has_value()) {
        return std::unexpected(std::move(index_end.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(index_end.value() - start.value()));
        !written.has_value()) {
        return written;
    }

    auto addresses_start = data_->get_file_pointer();
    if (!addresses_start.has_value()) {
        return std::unexpected(std::move(addresses_start.error()));
    }
    if (auto copied = address_output.copy_to(*data_); !copied.has_value()) {
        return copied;
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(addresses_start.value())); !written.has_value()) {
        return written;
    }
    auto addresses_end = data_->get_file_pointer();
    if (!addresses_end.has_value()) {
        return std::unexpected(std::move(addresses_end.error()));
    }
    return meta_->write_long(static_cast<int64_t>(addresses_end.value() - addresses_start.value()));
}

VoidResult Lucene90DocValuesConsumer::do_add_sorted_field(const FieldInfo &field, DocValuesProducer &producer,
                                                          bool add_type_byte) {
    if (field.doc_values_skip_index != DocValuesSkipIndexType::None) {
        if (auto written = write_skip_index(field, producer); !written.has_value()) {
            return written;
        }
    }
    if (add_type_byte) {
        // 0 marks a single-valued field.
        if (auto written = meta_->write_byte(0); !written.has_value()) {
            return written;
        }
    }
    if (auto written = write_values(field, producer, /*ords=*/true); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return write_terms_dict(field, producer, /*sorted_set=*/false);
}

VoidResult Lucene90DocValuesConsumer::do_add_sorted_numeric_field(const FieldInfo &field, DocValuesProducer &producer,
                                                                  bool ords) {
    if (field.doc_values_skip_index != DocValuesSkipIndexType::None) {
        if (auto written = write_skip_index(field, producer); !written.has_value()) {
            return written;
        }
    }
    if (ords) {
        // 1 marks a multi-valued field.
        if (auto written = meta_->write_byte(1); !written.has_value()) {
            return written;
        }
    }

    auto values = write_values(field, producer, ords);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    const int32_t num_docs_with_field = values.value().first;
    const int64_t num_values = values.value().second;

    if (auto written = meta_->write_int(num_docs_with_field); !written.has_value()) {
        return written;
    }
    if (num_values > static_cast<int64_t>(num_docs_with_field)) {
        // More than one value per document: the metadata points at the per-document
        // offsets into the value list.
        auto start = data_->get_file_pointer();
        if (!start.has_value()) {
            return std::unexpected(std::move(start.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(start.value())); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_vint(Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
            !written.has_value()) {
            return written;
        }

        const int64_t count = static_cast<int64_t>(num_docs_with_field) + 1;
        auto addresses_writer = DirectMonotonicWriter::get_instance(
                *meta_, *data_, count, Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
        if (!addresses_writer.has_value()) {
            return std::unexpected(std::move(addresses_writer.error()));
        }

        int64_t address = 0;
        if (auto added = addresses_writer.value().add(address); !added.has_value()) {
            return added;
        }

        auto per_doc = producer.get_sorted_numeric(field);
        if (!per_doc.has_value()) {
            return std::unexpected(std::move(per_doc.error()));
        }
        auto doc = per_doc.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        while (doc.value() != NO_MORE_DOCS) {
            auto count_here = per_doc.value()->doc_value_count();
            if (!count_here.has_value()) {
                return std::unexpected(std::move(count_here.error()));
            }
            address += count_here.value();
            if (auto added = addresses_writer.value().add(address); !added.has_value()) {
                return added;
            }
            doc = per_doc.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
        }

        if (auto finished = addresses_writer.value().finish(); !finished.has_value()) {
            return finished;
        }
        auto end = data_->get_file_pointer();
        if (!end.has_value()) {
            return std::unexpected(std::move(end.error()));
        }
        return meta_->write_long(static_cast<int64_t>(end.value() - start.value()));
    }
    return {};
}

VoidResult Lucene90DocValuesConsumer::add_numeric_field(const FieldInfo &field, DocValuesProducer &producer) {
    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_byte(Lucene90DocValuesFormat::NUMERIC); !written.has_value()) {
        return written;
    }
    DocValuesView view(producer, DocValuesView::Kind::kNumeric);
    if (field.doc_values_skip_index != DocValuesSkipIndexType::None) {
        if (auto written = write_skip_index(field, view); !written.has_value()) {
            return written;
        }
    }
    if (auto written = write_values(field, view, /*ords=*/false); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return {};
}

VoidResult Lucene90DocValuesConsumer::add_binary_field(const FieldInfo &field, DocValuesProducer &producer) {
    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_byte(Lucene90DocValuesFormat::BINARY); !written.has_value()) {
        return written;
    }

    auto start = data_->get_file_pointer();
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(start.value())); !written.has_value()) {
        return written;
    }

    int32_t num_docs_with_field = 0;
    size_t min_length = std::numeric_limits<int32_t>::max();
    size_t max_length = 0;
    {
        auto values = producer.get_binary(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        auto doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        while (doc.value() != NO_MORE_DOCS) {
            ++num_docs_with_field;
            auto value = values.value()->binary_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            const std::span<const uint8_t> bytes = value.value().as_slice();
            if (auto written = data_->write_bytes(bytes, 0, bytes.size()); !written.has_value()) {
                return written;
            }
            min_length = std::min(min_length, bytes.size());
            max_length = std::max(max_length, bytes.size());
            doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
        }
    }
    if (max_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return err(LuceneError::illegal_state("a doc value is too long to be encoded"));
    }
    if (num_docs_with_field == 0) {
        min_length = 0;
    }

    auto data_end = data_->get_file_pointer();
    if (!data_end.has_value()) {
        return std::unexpected(std::move(data_end.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(data_end.value() - start.value()));
        !written.has_value()) {
        return written;
    }

    if (num_docs_with_field == 0 || num_docs_with_field == max_doc_) {
        if (auto written = meta_->write_long(num_docs_with_field == 0 ? -2 : -1); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_long(0); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_short(-1); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(-1)); !written.has_value()) {
            return written;
        }
    } else {
        auto offset = data_->get_file_pointer();
        if (!offset.has_value()) {
            return std::unexpected(std::move(offset.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(offset.value())); !written.has_value()) {
            return written;
        }
        auto values = producer.get_binary(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        DocValuesToDisi doc_ids(*values.value());
        auto bitset = IndexedDISI::write_bitset(doc_ids, *data_, DEFAULT_DENSE_RANK_POWER);
        if (!bitset.has_value()) {
            return std::unexpected(std::move(bitset.error()));
        }
        auto docs_end = data_->get_file_pointer();
        if (!docs_end.has_value()) {
            return std::unexpected(std::move(docs_end.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(docs_end.value() - offset.value()));
            !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_short(static_cast<int16_t>(bitset.value().jump_table_entry_count));
            !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(bitset.value().dense_rank_power));
            !written.has_value()) {
            return written;
        }
    }

    if (auto written = meta_->write_int(num_docs_with_field); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_int(static_cast<int32_t>(min_length)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_int(static_cast<int32_t>(max_length)); !written.has_value()) {
        return written;
    }

    if (max_length > min_length) {
        // Variable width values: the metadata holds the end address of every value.
        auto addresses_start = data_->get_file_pointer();
        if (!addresses_start.has_value()) {
            return std::unexpected(std::move(addresses_start.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(addresses_start.value())); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_vint(Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
            !written.has_value()) {
            return written;
        }

        auto writer = DirectMonotonicWriter::get_instance(*meta_, *data_,
                                                           static_cast<int64_t>(num_docs_with_field) + 1,
                                                           Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
        if (!writer.has_value()) {
            return std::unexpected(std::move(writer.error()));
        }

        int64_t address = 0;
        if (auto added = writer.value().add(address); !added.has_value()) {
            return added;
        }
        auto values = producer.get_binary(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        auto doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        while (doc.value() != NO_MORE_DOCS) {
            auto value = values.value()->binary_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            address += static_cast<int64_t>(value.value().as_slice().size());
            if (auto added = writer.value().add(address); !added.has_value()) {
                return added;
            }
            doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
        }
        if (auto finished = writer.value().finish(); !finished.has_value()) {
            return finished;
        }
        auto addresses_end = data_->get_file_pointer();
        if (!addresses_end.has_value()) {
            return std::unexpected(std::move(addresses_end.error()));
        }
        return meta_->write_long(static_cast<int64_t>(addresses_end.value() - addresses_start.value()));
    }
    return {};
}

VoidResult Lucene90DocValuesConsumer::add_sorted_field(const FieldInfo &field, DocValuesProducer &producer) {
    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_byte(Lucene90DocValuesFormat::SORTED); !written.has_value()) {
        return written;
    }
    DocValuesView view(producer, DocValuesView::Kind::kSorted);
    return do_add_sorted_field(field, view, /*add_type_byte=*/false);
}

VoidResult Lucene90DocValuesConsumer::add_sorted_numeric_field(const FieldInfo &field, DocValuesProducer &producer) {
    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_byte(Lucene90DocValuesFormat::SORTED_NUMERIC); !written.has_value()) {
        return written;
    }
    return do_add_sorted_numeric_field(field, producer, /*ords=*/false);
}

VoidResult Lucene90DocValuesConsumer::add_sorted_set_field(const FieldInfo &field, DocValuesProducer &producer) {
    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }
    if (auto written = meta_->write_byte(Lucene90DocValuesFormat::SORTED_SET); !written.has_value()) {
        return written;
    }

    auto single_valued = [&]() -> Result<bool> {
        auto values = producer.get_sorted_set(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        return is_single_valued(*values.value());
    }();

    if (!single_valued.has_value()) {
        return std::unexpected(std::move(single_valued.error()));
    }

    if (single_valued.value()) {
        // A single-valued sorted set is stored exactly like a sorted field.
        DocValuesView view(producer, DocValuesView::Kind::kSortedSetSingleValued);
        return do_add_sorted_field(field, view, /*add_type_byte=*/true);
    }

    DocValuesView view(producer, DocValuesView::Kind::kSortedSetMultiValued);
    if (auto written = do_add_sorted_numeric_field(field, view, /*ords=*/true); !written.has_value()) {
        return written;
    }
    return write_terms_dict(field, producer, /*sorted_set=*/true);
}

VoidResult Lucene90DocValuesConsumer::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;

    if (meta_ != nullptr) {
        if (auto written = meta_->write_int(-1); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_footer(*meta_); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_footer(*data_); !written.has_value()) {
            return written;
        }
    }

    VoidResult result{};
    if (data_ != nullptr) {
        if (auto closed = data_->close(); !closed.has_value()) {
            result = closed;
        }
        data_.reset();
    }
    if (meta_ != nullptr) {
        if (auto closed = meta_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
        meta_.reset();
    }
    return result;
}

} // namespace lucene
