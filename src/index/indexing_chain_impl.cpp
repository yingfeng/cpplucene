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

module lucene_core:indexing_chain.impl;

import :indexing_chain;

import :analyzer;
import :array_util;
import :attribute_source;
import :attributes;
import :binary_doc_values_writer;
import :bm25_similarity;
import :bytes_ref;
import :byte_block_pool;
import :codec;
import :constants;
import :directory;
import :doc_values_consumer;
import :doc_values_format;
import :doc_values_writer;
import :document;
import :error;
import :field_infos;
import :field_infos_builder;
import :field_infos_format;
import :field_invert_state;
import :field_schema;
import :field_type;
import :fields_consumer;
import :freq_prox_fields;
import :freq_prox_terms_writer;
import :index_types;
import :indexable_field;
import :indexing_context;
import :int_block_pool;
import :norm_values_writer;
import :norms_consumer;
import :norms_format;
import :norms_producer;
import :number;
import :numeric_doc_values_writer;
import :postings_format;
import :segment_info;
import :segment_write_state;
import :similarity;
import :sorted_doc_values_writer;
import :sorted_numeric_doc_values_writer;
import :sorted_set_doc_values_writer;
import :stored_fields_consumer;
import :stored_fields_format;
import :term_vectors_consumer;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

/// Bytes of the longest stored string: `MAX_ARRAY_LENGTH / MAX_UTF8_BYTES_PER_CHAR`.
constexpr size_t MAX_STORED_STRING_LENGTH = ArrayUtil::MAX_ARRAY_LENGTH / 4;
/// Bytes of the longest term, for the "immense term" diagnostics.
constexpr size_t MAX_TERM_LENGTH = ArrayUtil::MAX_ARRAY_LENGTH / 4;
/// Initial number of buckets of the per-field hash (the reference implementation's `INITIAL_CAPACITY`).
constexpr size_t INITIAL_HASH_BUCKETS = 16;

[[nodiscard]] size_t bucket_of(std::string_view name, size_t mask) {
    return std::hash<std::string_view>{}(name) & mask;
}

/// Dispatches the normas of a segment by field name.
///
/// The postings writer reads the norms of each document back while flushing
/// (they feed its skip data), so the chain hands the buffered norms of every
/// field to it.  The reference implementation passes the same values through its own
/// `NormsProducer` implementations.
class ChainNormsProducer final : public NormsProducer {
public:
    explicit ChainNormsProducer(std::vector<std::pair<std::string, std::unique_ptr<NormsProducer>>> producers)
        : producers_(std::move(producers)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override {
        for (const auto &[name, producer] : producers_) {
            if (name == field.name) {
                return producer->get_norms(field);
            }
        }
        return err(LuceneError::illegal_state("no buffered norms for field \"" + field.name + "\""));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::vector<std::pair<std::string, std::unique_ptr<NormsProducer>>> producers_;
};

/// The first `limit` bytes of `text`, for the immense-term message.
[[nodiscard]] std::string prefix_of(std::string_view text, size_t limit = 30) {
    return std::string(text.substr(0, std::min(limit, text.size())));
}

} // namespace

/// Everything the chain keeps between documents.
struct IndexingChain::Impl {
    /// One distinct field name of the segment.
    struct PerField {
        PerField(std::string name, bool is_reserved) : schema(std::move(name)), reserved(is_reserved) {}

        FieldSchema schema;
        bool reserved{false};
        std::shared_ptr<FieldInfo> field_info;
        std::optional<FieldInvertState> invert_state;
        std::optional<FreqProxTermsWriterPerField> terms_hash_per_field;
        std::unique_ptr<DocValuesWriter> doc_values_writer;
        std::unique_ptr<NormValuesWriter> norms;
        int64_t field_gen{-1};
        /// Index of the next [`PerField`] in the same bucket, `-1` for the last.
        int32_t next{-1};
        bool first{false};
        size_t idx_in_doc_field{0};

        void reset(int32_t doc_id) {
            first = true;
            schema.reset(doc_id);
        }
    };

    Impl(Directory &directory, const Codec &codec, SegmentInfo &segment_info,
         int32_t index_created_version_major, Analyzer &analyzer, std::shared_ptr<const Similarity> similarity)
        : directory_(directory),
          codec_(codec),
          segment_info_(segment_info),
          index_created_version_major_(index_created_version_major),
          analyzer_(analyzer),
          similarity_(std::move(similarity)),
          stored_fields_(directory, require_stored_fields(codec), segment_info) {
        if (similarity_ == nullptr) {
            auto created = BM25Similarity::default_similarity();
            if (!created.has_value()) {
                throw std::runtime_error("cannot create the default similarity: " +
                                         created.error().to_string());
            }
            similarity_ = std::make_shared<const BM25Similarity>(std::move(created.value()));
        }
        // The term vectors consumer is owned by the postings writer, which is
        // what the reference implementation's `FreqProxTermsWriter::next_terms_hash` does.
        std::unique_ptr<TermVectorsConsumer> term_vectors;
        if (const auto *format = codec.term_vectors_format(); format != nullptr) {
            term_vectors = std::make_unique<TermVectorsConsumer>(directory, *format, segment_info);
        }
        terms_hash_ = std::make_unique<FreqProxTermsWriter>(std::move(term_vectors));
        field_hash_.assign(INITIAL_HASH_BUCKETS, -1);
        hash_mask_ = INITIAL_HASH_BUCKETS - 1;
    }

    [[nodiscard]] static const StoredFieldsFormat &require_stored_fields(const Codec &codec) {
        const auto *format = codec.stored_fields_format();
        if (format == nullptr) {
            // The constructor cannot fail, so this is a programming error: a
            // codec that writes segments must provide stored fields.
            throw std::runtime_error("the codec does not provide a stored fields format");
        }
        return *format;
    }

    // ---- per-field table ----

    [[nodiscard]] std::optional<size_t> get_per_field(std::string_view name) const {
        for (int32_t index = field_hash_[bucket_of(name, hash_mask_)]; index >= 0;
             index = per_fields_[static_cast<size_t>(index)]->next) {
            if (per_fields_[static_cast<size_t>(index)]->schema.name() == name) {
                return static_cast<size_t>(index);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] Result<size_t> get_or_add_per_field(const IndexableField &field, bool is_reserved,
                                                      size_t idx_in_doc_field) {
        const std::string_view name = field.name();
        if (auto existing = get_per_field(name); existing.has_value()) {
            return existing.value();
        }
        const size_t index = per_fields_.size();
        auto per_field = std::make_unique<PerField>(std::string(name), is_reserved);
        per_field->idx_in_doc_field = idx_in_doc_field;
        const size_t bucket = bucket_of(name, hash_mask_);
        per_field->next = field_hash_[bucket];
        field_hash_[bucket] = static_cast<int32_t>(index);
        per_fields_.push_back(std::move(per_field));
        if (per_fields_.size() > field_hash_.size()) {
            rehash();
        }
        return index;
    }

    void rehash() {
        const size_t new_size = field_hash_.size() * 2;
        field_hash_.assign(new_size, -1);
        hash_mask_ = new_size - 1;
        for (size_t index = 0; index < per_fields_.size(); ++index) {
            const size_t bucket = bucket_of(per_fields_[index]->schema.name(), hash_mask_);
            per_fields_[index]->next = field_hash_[bucket];
            field_hash_[bucket] = static_cast<int32_t>(index);
        }
    }

    // ---- document ----

    [[nodiscard]] VoidResult process_document(int32_t doc_id, const Document &document,
                                              FieldInfosBuilder &field_infos) {
        const int64_t field_gen = next_field_gen_++;
        fields_.clear();
        doc_fields_.clear();

        if (auto started = terms_hash_->start_document(); !started.has_value()) {
            return started;
        }
        if (auto started = stored_fields_.start_document(doc_id); !started.has_value()) {
            return std::unexpected(std::move(started.error()));
        }

        const std::vector<const IndexableField *> document_fields = document.get_fields();

        // First pass: build the schema of every distinct field name, so all the
        // instances of a multi-valued field are in hand before any of them is
        // inverted (an analyzer reuses its token stream).
        for (const IndexableField *field : document_fields) {
            if (field == nullptr) {
                return err(LuceneError::illegal_argument("a document field is null"));
            }
            const bool is_reserved = field->is_reserved();
            const size_t idx_in_doc_field = doc_fields_.size();
            auto index = get_or_add_per_field(*field, is_reserved, idx_in_doc_field);
            if (!index.has_value()) {
                return std::unexpected(std::move(index.error()));
            }
            PerField &per_field = *per_fields_[index.value()];
            if (per_field.reserved != is_reserved) {
                return err(LuceneError::illegal_argument("\"" + std::string(field->name()) +
                                                         "\" is a reserved field and should not be added to "
                                                         "any document"));
            }
            if (per_field.field_gen != field_gen) {
                fields_.push_back(index.value());
                per_field.field_gen = field_gen;
                per_field.reset(doc_id);
            }
            doc_fields_.push_back(index.value());
            if (auto updated = update_doc_field_schema(per_field, field->field_type());
                !updated.has_value()) {
                return updated;
            }
        }

        // New field names: create their `FieldInfo`; known ones: verify that this
        // document uses them the same way the segment does.
        for (const size_t index : fields_) {
            PerField &per_field = *per_fields_[index];
            if (per_field.field_info != nullptr) {
                if (auto same = per_field.schema.assert_same_schema(*per_field.field_info);
                    !same.has_value()) {
                    return same;
                }
            } else if (auto added = initialize_field_info(per_field, field_infos); !added.has_value()) {
                return added;
            }
        }

        // Second pass: index every field instance.
        size_t indexed_field_count = 0;
        fields_.clear();
        for (size_t i = 0; i < document_fields.size(); ++i) {
            const size_t index = doc_fields_[i];
            bool indexed = false;
            if (auto processed = process_field(doc_id, *document_fields[i], index, indexed);
                !processed.has_value()) {
                return processed;
            }
            if (indexed) {
                fields_.push_back(index);
                ++indexed_field_count;
            }
        }

        for (size_t i = 0; i < indexed_field_count; ++i) {
            if (auto finished = finish_per_field(doc_id, *per_fields_[fields_[i]]);
                !finished.has_value()) {
                return finished;
            }
        }
        if (auto finished = stored_fields_.finish_document(); !finished.has_value()) {
            return finished;
        }
        return terms_hash_->finish_document(doc_id, context_.term_vectors_int_pool,
                                            context_.term_vectors_byte_pool, context_.byte_pool);
    }

    [[nodiscard]] VoidResult update_doc_field_schema(PerField &per_field, const FieldType &field_type) {
        FieldSchema &schema = per_field.schema;
        if (field_type.index_options() != IndexOptions::None) {
            if (auto set = schema.set_index_options(field_type.index_options(), field_type.omit_norms(),
                                                    field_type.store_term_vectors());
                !set.has_value()) {
                return set;
            }
        } else {
            if (auto verified = verify_unindexed_field_type(schema.name(), field_type);
                !verified.has_value()) {
                return verified;
            }
        }

        if (field_type.doc_values_type() != DocValuesType::None) {
            if (auto set = schema.set_doc_values(field_type.doc_values_type(),
                                                 field_type.doc_values_skip_index_type());
                !set.has_value()) {
                return set;
            }
        } else if (field_type.doc_values_skip_index_type() != DocValuesSkipIndexType::None) {
            return err(LuceneError::illegal_argument("field '" + schema.name() +
                                                     "' cannot have a doc values skip index without doc values"));
        }

        if (field_type.point_dimension_count() != 0) {
            return err(LuceneError::unsupported_operation(
                    "point values are not indexed by this port (field=\"" + schema.name() + "\")"));
        }
        if (field_type.vector_dimension() != 0) {
            return err(LuceneError::unsupported_operation(
                    "vector values are not indexed by this port (field=\"" + schema.name() + "\")"));
        }
        if (!field_type.attributes().empty()) {
            return schema.update_attributes(field_type.attributes());
        }
        return {};
    }

    [[nodiscard]] static VoidResult verify_unindexed_field_type(std::string_view name,
                                                               const FieldType &field_type) {
        const std::string suffix = " for a field that is not indexed (field=\"" + std::string(name) + "\")";
        if (field_type.store_term_vectors()) {
            return err(LuceneError::illegal_argument("cannot store term vectors" + suffix));
        }
        if (field_type.store_term_vector_positions()) {
            return err(LuceneError::illegal_argument("cannot store term vector positions" + suffix));
        }
        if (field_type.store_term_vector_offsets()) {
            return err(LuceneError::illegal_argument("cannot store term vector offsets" + suffix));
        }
        if (field_type.store_term_vector_payloads()) {
            return err(LuceneError::illegal_argument("cannot store term vector payloads" + suffix));
        }
        return {};
    }

    [[nodiscard]] VoidResult initialize_field_info(PerField &per_field, FieldInfosBuilder &field_infos) {
        const FieldSchema &schema = per_field.schema;
        FieldInfo info;
        info.name = schema.name();
        info.number = 0; // assigned by the builder
        info.store_term_vector = schema.store_term_vector();
        info.omit_norms = schema.omit_norms();
        // `store_payloads` is set while indexing, once a payload is seen.
        info.store_payloads = false;
        info.index_options = schema.index_options();
        info.doc_values_type = schema.doc_values_type();
        info.doc_values_skip_index = schema.doc_values_skip_index();
        for (const auto &[key, value] : schema.attributes()) {
            info.attributes.emplace(key, value);
        }

        auto added = field_infos.add(std::move(info));
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        per_field.field_info = added.value();

        if (per_field.field_info->index_options != IndexOptions::None) {
            if (auto state = set_invert_state(per_field); !state.has_value()) {
                return state;
            }
        }

        switch (per_field.field_info->doc_values_type) {
            case DocValuesType::None:
                break;
            case DocValuesType::Numeric:
                per_field.doc_values_writer =
                        std::make_unique<NumericDocValuesWriter>(per_field.field_info);
                break;
            case DocValuesType::Binary:
                per_field.doc_values_writer =
                        std::make_unique<BinaryDocValuesWriter>(per_field.field_info);
                break;
            case DocValuesType::Sorted:
                per_field.doc_values_writer =
                        std::make_unique<SortedDocValuesWriter>(per_field.field_info);
                break;
            case DocValuesType::SortedNumeric:
                per_field.doc_values_writer =
                        std::make_unique<SortedNumericDocValuesWriter>(per_field.field_info);
                break;
            case DocValuesType::SortedSet:
                per_field.doc_values_writer =
                        std::make_unique<SortedSetDocValuesWriter>(per_field.field_info);
                break;
        }
        return {};
    }

    [[nodiscard]] VoidResult set_invert_state(PerField &per_field) {
        const auto &info = per_field.field_info;
        per_field.invert_state = FieldInvertState(index_created_version_major_, info->name,
                                                 info->index_options);
        auto added = terms_hash_->add_field(info);
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        per_field.terms_hash_per_field = std::move(added.value());
        if (!info->omit_norms) {
            // Even if no document ever sets a norm, the segment still writes
            // norms for the field.
            per_field.norms = std::make_unique<NormValuesWriter>(info);
        }
        return {};
    }

    [[nodiscard]] VoidResult process_field(int32_t doc_id, const IndexableField &field, size_t index,
                                           bool &indexed) {
        PerField &per_field = *per_fields_[index];
        const FieldType &type = field.field_type();
        indexed = false;

        if (type.index_options() != IndexOptions::None) {
            const bool first = per_field.first;
            if (auto inverted = invert(doc_id, field, per_field, first); !inverted.has_value()) {
                return inverted;
            }
            per_field.first = false;
            // Only the first instance of a field in a document counts for the
            // per-field bookkeeping (the norm pass and the term vectors).
            indexed = first;
        }

        if (type.stored()) {
            FieldValue stored;
            if (const auto text = field.string_value(); text.has_value()) {
                const std::string_view value = text.value();
                if (value.size() > MAX_STORED_STRING_LENGTH) {
                    return err(LuceneError::illegal_argument("stored field \"" + std::string(field.name()) +
                                                             "\" is too large (" +
                                                             std::to_string(value.size()) +
                                                             " bytes) to store"));
                }
                stored = std::string(value);
            } else if (const auto bytes = field.binary_value(); bytes.has_value()) {
                stored = bytes.value();
            } else if (const auto number = field.numeric_value(); number.has_value()) {
                stored = number.value();
            } else {
                return err(LuceneError::illegal_argument("Cannot store a null value"));
            }
            if (auto written = stored_fields_.write_field(*per_field.field_info, stored);
                !written.has_value()) {
                return written;
            }
        }

        if (type.doc_values_type() != DocValuesType::None) {
            return index_doc_value(doc_id, per_field, field);
        }
        return {};
    }

    [[nodiscard]] VoidResult index_doc_value(int32_t doc_id, PerField &per_field,
                                             const IndexableField &field) {
        const std::string &name = per_field.field_info->name;
        DocValuesWriter *writer = per_field.doc_values_writer.get();
        if (writer == nullptr) {
            return err(LuceneError::illegal_state("field=\"" + name + "\": no doc values writer"));
        }
        switch (per_field.field_info->doc_values_type) {
            case DocValuesType::None:
                return err(LuceneError::illegal_state("field=\"" + name + "\": no doc values writer"));
            case DocValuesType::Numeric: {
                auto &numeric = dynamic_cast<NumericDocValuesWriter &>(*writer);
                const auto number = field.numeric_value();
                if (!number.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": null value not allowed"));
                }
                auto value = number_i64(number.value());
                if (!value.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": numeric value out of range"));
                }
                return numeric.add_value(doc_id, value.value());
            }
            case DocValuesType::Binary: {
                auto &binary = dynamic_cast<BinaryDocValuesWriter &>(*writer);
                const auto bytes = field.binary_value();
                if (!bytes.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": null value not allowed"));
                }
                return binary.add_value(doc_id, std::span<const uint8_t>(bytes.value()));
            }
            case DocValuesType::Sorted: {
                auto &sorted = dynamic_cast<SortedDocValuesWriter &>(*writer);
                const auto bytes = field.binary_value();
                if (!bytes.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": null value not allowed"));
                }
                return sorted.add_value(doc_id, std::span<const uint8_t>(bytes.value()));
            }
            case DocValuesType::SortedNumeric: {
                auto &sorted_numeric = dynamic_cast<SortedNumericDocValuesWriter &>(*writer);
                const auto number = field.numeric_value();
                if (!number.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": null value not allowed"));
                }
                auto value = number_i64(number.value());
                if (!value.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": numeric value out of range"));
                }
                return sorted_numeric.add_value(doc_id, value.value());
            }
            case DocValuesType::SortedSet: {
                auto &sorted_set = dynamic_cast<SortedSetDocValuesWriter &>(*writer);
                const auto bytes = field.binary_value();
                if (!bytes.has_value()) {
                    return err(LuceneError::illegal_argument("field=\"" + name +
                                                             "\": null value not allowed"));
                }
                return sorted_set.add_value(doc_id, std::span<const uint8_t>(bytes.value()));
            }
        }
        return err(LuceneError::illegal_state("field=\"" + name + "\": no doc values writer"));
    }

    /// The integer value of `number`, or `nullopt` when it does not fit (the
    /// float/double conversions round towards zero, as the reference implementation's `to_i64`).
    [[nodiscard]] static std::optional<int64_t> number_i64(const Number &number) {
        return std::visit(
                [](const auto &value) -> std::optional<int64_t> {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_integral_v<T>) {
                        return static_cast<int64_t>(value);
                    } else {
                        if (!std::isfinite(static_cast<double>(value))) {
                            return std::nullopt;
                        }
                        const double magnitude = std::trunc(static_cast<double>(value));
                        if (magnitude < -9223372036854775808.0 || magnitude > 9223372036854775807.0) {
                            return std::nullopt;
                        }
                        return static_cast<int64_t>(magnitude);
                    }
                },
                number);
    }

    // ---- inversion ----

    [[nodiscard]] VoidResult invert(int32_t doc_id, const IndexableField &field, PerField &per_field,
                                    bool first) {
        if (first) {
            if (!per_field.invert_state.has_value()) {
                return err(LuceneError::illegal_state("invert_state not initialized"));
            }
            per_field.invert_state->reset();
        }
        switch (field.invertable_type()) {
            case InvertableType::Binary:
                return invert_term(doc_id, field, per_field, first);
            case InvertableType::TokenStream:
                return invert_token_stream(doc_id, field, per_field, first);
        }
        return err(LuceneError::illegal_state("unknown invertable type"));
    }

    [[nodiscard]] VoidResult invert_token_stream(int32_t doc_id, const IndexableField &field,
                                                 PerField &per_field, bool first) {
        const std::string &field_name = per_field.schema.name();
        if (!per_field.terms_hash_per_field.has_value()) {
            return err(LuceneError::illegal_state("terms hash is missing"));
        }
        FreqProxTermsWriterPerField &terms = *per_field.terms_hash_per_field;
        if (auto started = terms.start(field, first, context_.byte_pool); !started.has_value()) {
            return started;
        }

        auto stream_result = field.token_stream(analyzer_);
        if (!stream_result.has_value()) {
            return std::unexpected(std::move(stream_result.error()));
        }
        TokenStream *stream = stream_result.value();
        if (stream == nullptr) {
            return err(LuceneError::illegal_state("Analyzer token_stream is not initialized"));
        }
        AttributeSource &attributes = stream->attributes();
        auto *position_increment = attributes.add_attribute<PositionIncrementAttribute>();
        auto *offsets = attributes.add_attribute<OffsetAttribute>();
        auto *term_frequency = attributes.add_attribute<TermFrequencyAttribute>();
        auto *char_term = attributes.add_attribute<CharTermAttribute>();

        FieldInvertState &state = *per_field.invert_state;

        if (auto reset = stream->reset(); !reset.has_value()) {
            return reset;
        }
        for (;;) {
            auto has_next = stream->increment_token();
            if (!has_next.has_value()) {
                (void)stream->end();
                return std::unexpected(std::move(has_next.error()));
            }
            if (!has_next.value()) {
                break;
            }

            const int32_t position_increment_value =
                    static_cast<int32_t>(position_increment->position_increment());
            const int64_t new_position =
                    static_cast<int64_t>(state.position()) + position_increment_value;
            if (new_position < state.last_position()) {
                if (position_increment_value == 0) {
                    return err(LuceneError::illegal_argument("first position increment must be > 0 (got 0) for field '" +
                                                             field_name + "'"));
                }
                if (position_increment_value < 0) {
                    return err(LuceneError::illegal_argument(
                            "position increment must be > 0 (got " + std::to_string(position_increment_value) +
                            ") for field '" + field_name + "'"));
                }
                return err(LuceneError::illegal_argument(
                        "position overflowed int32 max (got pos_incr=" +
                        std::to_string(position_increment_value) +
                        " last_position=" + std::to_string(state.last_position()) +
                        " position=" + std::to_string(new_position) + ") for field '" + field_name + "'"));
            }
            if (new_position > MAX_POSITION) {
                return err(LuceneError::illegal_argument("position " + std::to_string(new_position) +
                                                         " too large for field " + field_name));
            }
            if (position_increment_value == 0) {
                state.set_num_overlap(state.num_overlap() + 1);
            }
            state.set_position(static_cast<int32_t>(new_position));
            state.set_last_position(state.position());

            const int64_t start = static_cast<int64_t>(state.offset()) + offsets->start_offset();
            const int64_t end = static_cast<int64_t>(state.offset()) + offsets->end_offset();
            if (start < state.last_start_offset() || end < start) {
                return err(LuceneError::illegal_argument(
                        "startOffset must be non-negative, and endOffset must be >= startOffset, and offsets "
                        "must not go backwards: start=" + std::to_string(start) +
                        " end=" + std::to_string(end) + " last_start=" +
                        std::to_string(state.last_start_offset()) + " for field " + field_name));
            }
            state.set_last_start_offset(static_cast<int32_t>(start));

            const int64_t new_length = static_cast<int64_t>(state.length()) + term_frequency->term_frequency();
            if (new_length > std::numeric_limits<int32_t>::max()) {
                return err(LuceneError::illegal_argument("too many tokens for field " + field_name));
            }
            state.set_length(static_cast<int32_t>(new_length));

            auto added = terms.add_with_bytes_ref(nullptr, doc_id, state, attributes, context_);
            if (!added.has_value()) {
                if (added.error().kind() == ErrorKind::kMaxBytesLengthExceeded) {
                    return err(LuceneError::illegal_argument(
                            "Document contains at least one immense term in field=\"" + field_name +
                            "\" (whose UTF8 encoding is longer than the max length " +
                            std::to_string(MAX_TERM_LENGTH) +
                            "), all of which were skipped. Please correct the analyzer to not produce such "
                            "terms. The prefix of the first immense term is: '" +
                            prefix_of(char_term->text()) + "...'"));
                }
                return added;
            }
        }
        if (auto ended = stream->end(); !ended.has_value()) {
            return ended;
        }
        state.set_offset(state.offset() + static_cast<int32_t>(offsets->end_offset()));

        // **Deviation:** the reference implementation adds the analyzer's position increment gap and
        // offset gap here.  This port's `Analyzer` has no gap API, which is the
        // same as an analyzer whose gaps are zero (the default in Lucene).
        return {};
    }

    [[nodiscard]] VoidResult invert_term(int32_t doc_id, const IndexableField &field, PerField &per_field,
                                         bool first) {
        const auto binary_value = field.binary_value();
        if (!binary_value.has_value()) {
            return err(LuceneError::illegal_argument(
                    "Field " + std::string(field.name()) +
                    " returns TERM for invertable_type() and null for binary_value(), which is illegal"));
        }
        const FieldType &type = field.field_type();
        if (type.tokenized() || type.index_options() > IndexOptions::DocsAndFreqs ||
            type.store_term_vector_positions() || type.store_term_vector_offsets() ||
            type.store_term_vector_payloads()) {
            return err(LuceneError::illegal_argument(
                    "Fields that are tokenized or index proximity data must produce a non-null TokenStream, but " +
                    std::string(field.name()) + " did not"));
        }
        if (!per_field.terms_hash_per_field.has_value()) {
            return err(LuceneError::illegal_state("terms hash is missing"));
        }
        FieldInvertState &state = *per_field.invert_state;
        state.set_position(state.position() + 1);
        // Java's `invertTerm` adds one to the length; the reference implementation adds two, which
        // double counts the single token this path inverts.
        state.set_length(state.length() + 1);
        FreqProxTermsWriterPerField &terms = *per_field.terms_hash_per_field;
        if (auto started = terms.start(field, first, context_.byte_pool); !started.has_value()) {
            return started;
        }
        AttributeSource attributes;
        const BytesRef term(binary_value.value());
        auto added = terms.add_with_bytes_ref(&term, doc_id, state, attributes, context_);
        if (!added.has_value()) {
            if (added.error().kind() == ErrorKind::kMaxBytesLengthExceeded) {
                return err(LuceneError::illegal_argument(
                        "Document contains at least one immense term in field=\"" + per_field.schema.name() +
                        "\" (whose length is longer than the max length " + std::to_string(MAX_TERM_LENGTH) +
                        "), all of which were skipped. The prefix of the first immense term is: '" +
                        prefix_of(std::string_view(reinterpret_cast<const char *>(term.as_slice().data()),
                                                   term.length())) +
                        "...'"));
            }
            return added;
        }
        return {};
    }

    [[nodiscard]] VoidResult finish_per_field(int32_t doc_id, PerField &per_field) {
        const auto &info = per_field.field_info;
        if (info == nullptr) {
            return err(LuceneError::illegal_state("field info is missing"));
        }
        if (!info->omit_norms) {
            if (!per_field.invert_state.has_value()) {
                return err(LuceneError::illegal_state("invert state is missing"));
            }
            const FieldInvertState &state = *per_field.invert_state;
            int64_t norm = 0;
            if (state.length() != 0) {
                auto computed = similarity_->compute_norm(state);
                if (!computed.has_value()) {
                    return std::unexpected(std::move(computed.error()));
                }
                norm = computed.value();
                if (norm == 0) {
                    return err(LuceneError::illegal_state("Similarity returned 0 for non-empty field " +
                                                          info->name));
                }
            }
            if (per_field.norms == nullptr) {
                return err(LuceneError::illegal_state("norms writer is missing"));
            }
            if (auto added = per_field.norms->add_value(doc_id, norm); !added.has_value()) {
                return added;
            }
        }
        if (!per_field.terms_hash_per_field.has_value()) {
            return err(LuceneError::illegal_state("terms hash is missing"));
        }
        // The postings writer is what records that the field has payloads, and
        // it does so before the `.fnm` is written.
        if (per_field.terms_hash_per_field->saw_payloads()) {
            info->store_payloads = true;
        }
        return per_field.terms_hash_per_field->finish(static_cast<int32_t>(per_field.idx_in_doc_field));
    }

    // ---- flush ----

    [[nodiscard]] VoidResult flush(const SegmentWriteState &state) {
        const FieldInfos *infos = state.field_infos.get();
        if (infos == nullptr) {
            return err(LuceneError::illegal_state("the field infos of the segment are not built"));
        }
        if (auto written = write_norms(state, *infos); !written.has_value()) {
            return written;
        }
        if (auto written = write_doc_values(state, *infos); !written.has_value()) {
            return written;
        }
        if (auto written = stored_fields_.flush(segment_info_.max_doc); !written.has_value()) {
            return written;
        }
        if (auto written = write_postings(state); !written.has_value()) {
            return written;
        }
        if (auto written = write_term_vectors(); !written.has_value()) {
            return written;
        }
        // The `.fnm` is written last: the postings writer may still have set
        // `store_payloads` while flushing.
        return codec_.field_infos_format().write(state.directory, segment_info_, state.segment_suffix,
                                                 *infos, state.context);
    }

    [[nodiscard]] VoidResult write_norms(const SegmentWriteState &state, const FieldInfos &infos) {
        if (!infos.has_norms()) {
            return {};
        }
        const NormsFormat *format = codec_.norms_format();
        if (format == nullptr) {
            return err(LuceneError::illegal_state("the codec does not provide a norms format"));
        }
        auto consumer = format->norms_consumer(state, segment_info_);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        for (const FieldInfo &info : infos.fields()) {
            // The final `omit_norms` decides: it may have changed since the
            // field was first added.
            if (info.omit_norms || info.index_options == IndexOptions::None) {
                continue;
            }
            auto index = get_per_field(info.name);
            if (!index.has_value()) {
                return err(LuceneError::illegal_state("per-field entry is missing for \"" + info.name + "\""));
            }
            NormValuesWriter *norms = per_fields_[index.value()]->norms.get();
            if (norms == nullptr) {
                return err(LuceneError::illegal_state("norms writer is missing for \"" + info.name + "\""));
            }
            if (auto written = norms->flush_norms(*consumer.value()); !written.has_value()) {
                (void)consumer.value()->close();
                return written;
            }
        }
        return consumer.value()->close();
    }

    [[nodiscard]] VoidResult write_doc_values(const SegmentWriteState &state, const FieldInfos &infos) {
        if (!infos.has_doc_values()) {
            return {};
        }
        const DocValuesFormat *format = codec_.doc_values_format();
        if (format == nullptr) {
            return err(LuceneError::illegal_state("the codec does not provide a doc values format"));
        }
        auto consumer = format->fields_consumer(state, segment_info_);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        for (const auto &per_field : per_fields_) {
            if (per_field->doc_values_writer == nullptr) {
                continue;
            }
            if (auto written = per_field->doc_values_writer->flush(*consumer.value()); !written.has_value()) {
                (void)consumer.value()->close();
                return written;
            }
        }
        return consumer.value()->close();
    }

    [[nodiscard]] VoidResult write_postings(const SegmentWriteState &state) {
        std::vector<FreqProxTermsWriterPerField> to_flush;
        to_flush.reserve(per_fields_.size());
        for (auto &per_field : per_fields_) {
            if (per_field->invert_state.has_value() && per_field->terms_hash_per_field.has_value()) {
                to_flush.push_back(std::move(*per_field->terms_hash_per_field));
                per_field->terms_hash_per_field.reset();
            }
        }
        if (to_flush.empty()) {
            return {};
        }
        auto prepared = terms_hash_->prepare_fields(std::move(to_flush), context_.byte_pool);
        if (!prepared.has_value()) {
            return std::unexpected(std::move(prepared.error()));
        }
        if (prepared.value().empty()) {
            return {};
        }
        const PostingsFormat *format = codec_.postings_format();
        if (format == nullptr) {
            return err(LuceneError::illegal_state("the codec does not provide a postings format"));
        }
        auto consumer = format->fields_consumer(state, segment_info_);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        // The postings writer needs the norms of the documents it is writing.
        std::vector<std::pair<std::string, std::unique_ptr<NormsProducer>>> norms;
        norms.reserve(per_fields_.size());
        for (auto &per_field : per_fields_) {
            if (per_field->norms == nullptr || per_field->field_info == nullptr) {
                continue;
            }
            auto producer = per_field->norms->get_norms_producer();
            if (!producer.has_value()) {
                (void)consumer.value()->close();
                return std::unexpected(std::move(producer.error()));
            }
            norms.emplace_back(per_field->field_info->name, std::move(producer.value()));
        }
        ChainNormsProducer norms_producer(std::move(norms));

        // `FreqProxFields` shares the pools rather than owning them; the chain
        // outlives the flush, so the aliasing pointers are safe.
        std::shared_ptr<IntBlockPool> int_pool(&context_.freq_prox_term_int_pool, [](IntBlockPool *) {});
        std::shared_ptr<ByteBlockPool> byte_pool(&context_.byte_pool, [](ByteBlockPool *) {});
        FreqProxFields fields(std::move(prepared.value()), std::move(int_pool), std::move(byte_pool));
        // On flush the postings do not read norms back (only a merge does).
        if (auto written = consumer.value()->write(state, segment_info_, fields, &norms_producer);
            !written.has_value()) {
            (void)consumer.value()->close();
            return written;
        }
        return consumer.value()->close();
    }

    /// Writes the term vectors buffered for the segment.
    ///
    /// The reference implementation finishes them from `FreqProxTermsWriter::close`, i.e. inside the
    /// postings write; the port writes the postings and the vectors as two
    /// separate steps because the postings writer no longer owns the flush.
    [[nodiscard]] VoidResult write_term_vectors() {
        TermVectorsConsumer *consumer = terms_hash_->next_terms_hash();
        if (consumer == nullptr || !consumer->is_initialized()) {
            return {};
        }
        return consumer->flush(segment_info_.max_doc);
    }

    [[nodiscard]] VoidResult abort() {
        if (auto aborted = terms_hash_->abort(context_.freq_prox_term_int_pool, context_.byte_pool,
                                             context_.term_vectors_int_pool,
                                             context_.term_vectors_byte_pool);
            !aborted.has_value()) {
            return aborted;
        }
        for (auto &per_field : per_fields_) {
            per_field->invert_state.reset();
            per_field->terms_hash_per_field.reset();
            per_field->doc_values_writer.reset();
            per_field->norms.reset();
        }
        return stored_fields_.abort();
    }

    Directory &directory_;
    const Codec &codec_;
    SegmentInfo &segment_info_;
    int32_t index_created_version_major_;
    Analyzer &analyzer_;
    std::shared_ptr<const Similarity> similarity_;

    StoredFieldsConsumer stored_fields_;
    std::unique_ptr<FreqProxTermsWriter> terms_hash_;
    IndexContext context_;

    std::vector<std::unique_ptr<PerField>> per_fields_;
    std::vector<int32_t> field_hash_;
    size_t hash_mask_{0};
    int64_t next_field_gen_{0};

    /// Scratch buffers reused by every document.
    std::vector<size_t> fields_;
    std::vector<size_t> doc_fields_;
};

IndexingChain::IndexingChain(Directory &directory, const Codec &codec, SegmentInfo &segment_info,
                            int32_t index_created_version_major, Analyzer &analyzer,
                            std::shared_ptr<const Similarity> similarity)
    : impl_(std::make_unique<Impl>(directory, codec, segment_info, index_created_version_major, analyzer,
                                   std::move(similarity))) {}

IndexingChain::~IndexingChain() = default;

VoidResult IndexingChain::process_document(int32_t doc_id, const Document &document,
                                           FieldInfosBuilder &field_infos) {
    return impl_->process_document(doc_id, document, field_infos);
}

VoidResult IndexingChain::flush(const SegmentWriteState &state) { return impl_->flush(state); }

VoidResult IndexingChain::abort() { return impl_->abort(); }

size_t IndexingChain::num_per_fields() const noexcept { return impl_->per_fields_.size(); }

int64_t IndexingChain::next_field_gen() const noexcept { return impl_->next_field_gen_; }

} // namespace lucene
