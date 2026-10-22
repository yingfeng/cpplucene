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

module lucene_core:term_vectors_consumer_per_field.impl;

import :term_vectors_consumer_per_field;

import :array_util;
import :attribute_source;
import :attributes;
import :byte_block_pool;
import :byte_slice_reader;
import :bytes_ref;
import :bytes_ref_block_pool;
import :data_input;
import :debug;
import :error;
import :field_infos;
import :field_invert_state;
import :index_types;
import :indexable_field;
import :int_block_pool;
import :parallel_postings_array;
import :term_vectors_writer;
import :terms_hash_per_field;
import :types;

import std;

namespace lucene {

// ---------------------------------------------------------------------------
// TermVectorsPostingsArray
// ---------------------------------------------------------------------------

TermVectorsPostingsArray::TermVectorsPostingsArray(size_t size)
        : ParallelPostingsArray(size), freqs(size, 0), last_offsets(size, 0), last_positions(size, 0) {}

size_t TermVectorsPostingsArray::bytes_per_posting() const {
    return ParallelPostingsArray::BYTES_PER_POSTING + 3 * sizeof(int32_t);
}

VoidResult TermVectorsPostingsArray::copy_to(size_t new_size) {
    if (auto grown = ParallelPostingsArray::copy_to(new_size); !grown.has_value()) {
        return grown;
    }
    if (auto grown = ArrayUtil::grow_exact(freqs, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    if (auto grown = ArrayUtil::grow_exact(last_offsets, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    return ArrayUtil::grow_exact(last_positions, new_size, [] { return int32_t{0}; });
}

// ---------------------------------------------------------------------------
// TermVectorsTermsHashType
// ---------------------------------------------------------------------------

std::unique_ptr<PostingsArray> TermVectorsTermsHashType::new_per_field(size_t size) const {
    return std::make_unique<TermVectorsPostingsArray>(size);
}

// ---------------------------------------------------------------------------
// TermVectorsConsumerPerField
// ---------------------------------------------------------------------------

TermVectorsConsumerPerField::TermVectorsConsumerPerField(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)),
          base_(2, std::make_unique<TermVectorsTermsHashType>(), field_info_->index_options) {
    debug_check(field_info_->index_options != IndexOptions::None,
                "term vectors need a field that is indexed");
}

VoidResult TermVectorsConsumerPerField::start(const IndexableField &field, bool first, ByteBlockPool &byte_pool) {
    debug_check(field.field_type().index_options() != IndexOptions::None,
                "the field must be indexed with at least Docs");

    const auto &type = field.field_type();
    // A new document starts here, so any vector of the previous one has been
    // written out already.
    pending_flush_ = false;

    if (first) {
        if (base_.get_num_terms() != 0) {
            // Only necessary if the previous document hit a non-aborting error
            // while writing vectors of this field.
            base_.reset(byte_pool);
        }

        if (auto reinitialized = base_.reinit_hash(); !reinitialized.has_value()) {
            return reinitialized;
        }

        has_payloads_ = false;
        do_vectors_ = type.store_term_vectors();

        if (do_vectors_) {
            do_vector_positions_ = type.store_term_vector_positions();
            // Somewhat confusingly, unlike postings, offsets may be stored
            // without positions.
            do_vector_offsets_ = type.store_term_vector_offsets();

            if (do_vector_positions_) {
                do_vector_payloads_ = type.store_term_vector_payloads();
            } else {
                do_vector_payloads_ = false;
                if (type.store_term_vector_payloads()) {
                    return err(LuceneError::illegal_argument(
                            "cannot index term vector payloads without term vector positions (field=\"" +
                            std::string(field.name()) + "\")"));
                }
            }
        } else {
            if (type.store_term_vector_offsets()) {
                return err(LuceneError::illegal_argument(
                        "cannot index term vector offsets when term vectors are not indexed (field=\"" +
                        std::string(field.name()) + "\")"));
            }
            if (type.store_term_vector_positions()) {
                return err(LuceneError::illegal_argument(
                        "cannot index term vector positions when term vectors are not indexed (field=\"" +
                        std::string(field.name()) + "\")"));
            }
            if (type.store_term_vector_payloads()) {
                return err(LuceneError::illegal_argument(
                        "cannot index term vector payloads when term vectors are not indexed (field=\"" +
                        std::string(field.name()) + "\")"));
            }
        }
    } else {
        if (do_vectors_ != type.store_term_vectors()) {
            return err(LuceneError::illegal_argument(
                    "all instances of a given field name must have the same term vectors settings "
                    "(storeTermVectors changed for field=\"" +
                    std::string(field.name()) + "\")"));
        }
        if (do_vector_positions_ != type.store_term_vector_positions()) {
            return err(LuceneError::illegal_argument(
                    "all instances of a given field name must have the same term vectors settings "
                    "(storeTermVectorPositions changed for field=\"" +
                    std::string(field.name()) + "\")"));
        }
        if (do_vector_offsets_ != type.store_term_vector_offsets()) {
            return err(LuceneError::illegal_argument(
                    "all instances of a given field name must have the same term vectors settings "
                    "(storeTermVectorOffsets changed for field=\"" +
                    std::string(field.name()) + "\")"));
        }
        if (do_vector_payloads_ != type.store_term_vector_payloads()) {
            return err(LuceneError::illegal_argument(
                    "all instances of a given field name must have the same term vectors settings "
                    "(storeTermVectorPayloads changed for field=\"" +
                    std::string(field.name()) + "\")"));
        }
    }
    return {};
}

VoidResult TermVectorsConsumerPerField::add_with_text_start(int32_t text_start, int32_t doc_id,
                                                            const FieldInvertState &field_state,
                                                            const AttributeSource &attributes, IntBlockPool &int_pool,
                                                            ByteBlockPool &byte_pool,
                                                            ByteBlockPool &main_byte_pool) {
    // Secondary entry point (for the 2nd and subsequent terms hashes): the
    // token text has already been interned into `text_start`, so the hash is
    // keyed by that pool offset instead of by the bytes.
    auto added = base_.bytes_hash().add_by_pool_offset(text_start, main_byte_pool);
    if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
    }

    if (added.value() >= 0) {
        const auto term_id = static_cast<size_t>(added.value());
        if (auto initialized = base_.init_stream_slices(term_id, doc_id, int_pool, byte_pool);
            !initialized.has_value()) {
            return initialized;
        }
        return new_term(term_id, field_state, attributes, int_pool, byte_pool);
    }

    auto positioned = base_.position_stream_slice(added.value(), doc_id);
    if (!positioned.has_value()) {
        return std::unexpected(std::move(positioned.error()));
    }
    return add_term(positioned.value(), field_state, attributes, int_pool, byte_pool);
}

Result<TermVectorsPostingsArray *> TermVectorsConsumerPerField::postings() {
    auto *array = base_.postings_array_mut();
    if (array == nullptr) {
        return err(LuceneError::illegal_state("postings_array not initialized"));
    }
    auto *term_vectors = dynamic_cast<TermVectorsPostingsArray *>(array);
    if (term_vectors == nullptr) {
        return err(LuceneError::illegal_state("expected a TermVectors posting array"));
    }
    return term_vectors;
}

Result<int32_t> TermVectorsConsumerPerField::get_term_freq(const AttributeSource &attributes) const {
    int32_t freq = 1;
    if (const auto *attribute = attributes.get_attribute<TermFrequencyAttribute>(); attribute != nullptr) {
        freq = static_cast<int32_t>(attribute->term_frequency());
    }

    if (freq != 1) {
        if (do_vector_positions_) {
            return err(LuceneError::illegal_argument(
                    "field \"" + field_info_->name +
                    "\": cannot index term vector positions while using custom TermFrequencyAttribute"));
        }
        if (do_vector_offsets_) {
            return err(LuceneError::illegal_argument(
                    "field \"" + field_info_->name +
                    "\": cannot index term vector offsets while using custom TermFrequencyAttribute"));
        }
    }
    return freq;
}

VoidResult TermVectorsConsumerPerField::write_prox(size_t term_id, const FieldInvertState &field_state,
                                                   const AttributeSource &attributes, IntBlockPool &int_pool,
                                                   ByteBlockPool &byte_pool) {
    auto array = postings();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    auto *postings_array = array.value();

    std::optional<int32_t> last_offset;
    std::optional<int32_t> last_position;

    if (do_vector_offsets_) {
        int32_t start = 0;
        int32_t end = 0;
        if (const auto *attribute = attributes.get_attribute<OffsetAttribute>(); attribute != nullptr) {
            start = static_cast<int32_t>(attribute->start_offset());
            end = static_cast<int32_t>(attribute->end_offset());
        }

        const auto start_offset = field_state.offset() + start;
        const auto end_offset = field_state.offset() + end;

        if (auto written = base_.write_vint(1, start_offset - postings_array->last_offsets[term_id], int_pool,
                                           byte_pool);
            !written.has_value()) {
            return written;
        }
        if (auto written = base_.write_vint(1, end_offset - start_offset, int_pool, byte_pool); !written.has_value()) {
            return written;
        }
        last_offset = end_offset;
    }

    if (do_vector_positions_) {
        const auto pos = field_state.position() - postings_array->last_positions[term_id];

        const std::optional<BytesRef> *payload = nullptr;
        if (do_vector_payloads_) {
            if (const auto *attribute = attributes.get_attribute<PayloadAttribute>(); attribute != nullptr) {
                payload = &attribute->payload();
            }
        }

        if (payload != nullptr && payload->has_value() && payload->value().length() > 0) {
            const auto &bytes = payload->value();
            if (auto written = base_.write_vint(0, (pos << 1) | 1, int_pool, byte_pool); !written.has_value()) {
                return written;
            }
            if (auto written = base_.write_vint(0, static_cast<int32_t>(bytes.length()), int_pool, byte_pool);
                !written.has_value()) {
                return written;
            }
            if (auto written = base_.write_bytes(0, bytes.as_slice(), 0, bytes.length(), int_pool, byte_pool);
                !written.has_value()) {
                return written;
            }
            has_payloads_ = true;
        } else {
            if (auto written = base_.write_vint(0, pos << 1, int_pool, byte_pool); !written.has_value()) {
                return written;
            }
        }

        last_position = field_state.position();
    }

    if (last_offset.has_value()) {
        postings_array->last_offsets[term_id] = last_offset.value();
    }
    if (last_position.has_value()) {
        postings_array->last_positions[term_id] = last_position.value();
    }
    return {};
}

VoidResult TermVectorsConsumerPerField::new_term(size_t term_id, const FieldInvertState &field_state,
                                                 const AttributeSource &attributes, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool) {
    auto freq = get_term_freq(attributes);
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }
    auto array = postings();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    auto *postings_array = array.value();
    postings_array->freqs[term_id] = freq.value();
    postings_array->last_offsets[term_id] = 0;
    postings_array->last_positions[term_id] = 0;
    return write_prox(term_id, field_state, attributes, int_pool, byte_pool);
}

VoidResult TermVectorsConsumerPerField::add_term(size_t term_id, const FieldInvertState &field_state,
                                                 const AttributeSource &attributes, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool) {
    auto freq = get_term_freq(attributes);
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }
    auto array = postings();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    array.value()->freqs[term_id] += freq.value();
    return write_prox(term_id, field_state, attributes, int_pool, byte_pool);
}

VoidResult TermVectorsConsumerPerField::finish(int32_t /*field_index*/) {
    if (!do_vectors_ || base_.get_num_terms() == 0) {
        return {};
    }
    // The reference implementation records the index of the per-field writer in the consumer here;
    // the consumer owns the writers in this port, so a flag is enough.
    pending_flush_ = true;
    return {};
}

void TermVectorsConsumerPerField::reset(ByteBlockPool &byte_pool) { base_.reset(byte_pool); }

VoidResult TermVectorsConsumerPerField::finish_document(TermVectorsWriter &writer, const IntBlockPool &int_pool,
                                                        const ByteBlockPool &byte_pool,
                                                        const ByteBlockPool &term_byte_pool) {
    if (!do_vectors_) {
        return {};
    }
    do_vectors_ = false;

    if (auto written = write_to_writer(writer, int_pool, byte_pool, term_byte_pool); !written.has_value()) {
        return written;
    }
    pending_flush_ = false;
    return {};
}

VoidResult TermVectorsConsumerPerField::write_to_writer(TermVectorsWriter &writer, const IntBlockPool &int_pool,
                                                        const ByteBlockPool &byte_pool,
                                                        const ByteBlockPool &term_byte_pool) {
    const auto num_postings = base_.get_num_terms();
    if (auto sorted = base_.sort_terms(term_byte_pool); !sorted.has_value()) {
        return sorted;
    }
    const auto &term_ids = base_.get_sorted_term_ids();

    auto array = postings();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    auto *postings_array = array.value();

    if (auto started = writer.start_field(*field_info_, static_cast<int32_t>(num_postings), do_vector_positions_,
                                          do_vector_offsets_, has_payloads_);
        !started.has_value()) {
        return started;
    }

    ByteSliceReader offset_reader(byte_pool);
    ByteSliceReader position_reader(byte_pool);
    for (size_t i = 0; i < num_postings; ++i) {
        const auto term_id = static_cast<size_t>(term_ids[i]);
        const auto freq = postings_array->freqs[term_id];

        // The term text lives in the pool shared with the postings; the length
        // prefix written by `BytesRefBlockPool` gives the extent.
        const auto position =
                term_byte_pool_.fill_bytes_ref(postings_array->text_starts[term_id], term_byte_pool);
        const auto &block = term_byte_pool.buffer(position.block_index);
        const auto term = std::span<const uint8_t>(block.data() + position.offset, position.length);

        if (auto started = writer.start_term(term, freq); !started.has_value()) {
            return started;
        }

        if (do_vector_positions_ || do_vector_offsets_) {
            DataInput *positions = nullptr;
            if (do_vector_positions_) {
                if (auto initialized = base_.init_reader(position_reader, term_id, 0, int_pool);
                    !initialized.has_value()) {
                    return initialized;
                }
                positions = &position_reader;
            }
            DataInput *offsets = nullptr;
            if (do_vector_offsets_) {
                if (auto initialized = base_.init_reader(offset_reader, term_id, 1, int_pool); !initialized.has_value()) {
                    return initialized;
                }
                offsets = &offset_reader;
            }
            if (auto added = writer.add_prox(static_cast<size_t>(freq), positions, offsets); !added.has_value()) {
                return added;
            }
        }
    }

    if (auto finished = writer.finish_field(); !finished.has_value()) {
        return finished;
    }
    // The field only counts as storing vectors once one has been written; the
    // `.fnm` header is derived from this flag at flush time.
    field_info_->store_term_vector = true;
    return {};
}

} // namespace lucene
