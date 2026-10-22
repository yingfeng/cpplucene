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

module lucene_core:freq_prox_terms_writer_per_field.impl;

import :freq_prox_terms_writer_per_field;

import :array_util;
import :attribute_source;
import :attributes;
import :byte_block_pool;
import :bytes_ref;
import :debug;
import :error;
import :field_infos;
import :field_invert_state;
import :index_types;
import :indexable_field;
import :indexing_context;
import :int_block_pool;
import :parallel_postings_array;
import :terms_hash_per_field;
import :types;

import std;

namespace lucene {

namespace {

/// One stream for doc(+freq), a second one for positions/offsets.
[[nodiscard]] int32_t stream_count_for(IndexOptions index_options) {
    return index_options >= IndexOptions::DocsAndFreqsAndPositions ? 2 : 1;
}

} // namespace

// ---------------------------------------------------------------------------
// FreqProxPostingsArray
// ---------------------------------------------------------------------------

FreqProxPostingsArray::FreqProxPostingsArray(size_t size, bool write_freqs, bool write_prox, bool write_offsets)
        : ParallelPostingsArray(size), last_doc_ids(size, 0), last_doc_codes(size, 0) {
    if (write_freqs) {
        term_freqs.emplace(size, 0);
    }
    if (write_prox) {
        last_positions.emplace(size, 0);
    } else {
        debug_check(!write_offsets, "offsets require positions");
    }
    if (write_prox && write_offsets) {
        last_offsets.emplace(size, 0);
    }
}

size_t FreqProxPostingsArray::bytes_per_posting() const {
    size_t bytes = ParallelPostingsArray::BYTES_PER_POSTING + 2 * sizeof(int32_t);
    if (last_positions.has_value()) {
        bytes += sizeof(int32_t);
    }
    if (last_offsets.has_value()) {
        bytes += sizeof(int32_t);
    }
    if (term_freqs.has_value()) {
        bytes += sizeof(int32_t);
    }
    return bytes;
}

VoidResult FreqProxPostingsArray::copy_to(size_t new_size) {
    if (auto grown = ParallelPostingsArray::copy_to(new_size); !grown.has_value()) {
        return grown;
    }
    if (auto grown = ArrayUtil::grow_exact(last_doc_ids, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    if (auto grown = ArrayUtil::grow_exact(last_doc_codes, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    if (last_positions.has_value()) {
        if (auto grown = ArrayUtil::grow_exact(last_positions.value(), new_size, [] { return int32_t{0}; });
            !grown.has_value()) {
            return grown;
        }
    }
    if (last_offsets.has_value()) {
        if (auto grown = ArrayUtil::grow_exact(last_offsets.value(), new_size, [] { return int32_t{0}; });
            !grown.has_value()) {
            return grown;
        }
    }
    if (term_freqs.has_value()) {
        if (auto grown = ArrayUtil::grow_exact(term_freqs.value(), new_size, [] { return int32_t{0}; });
            !grown.has_value()) {
            return grown;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// FreqProxTermsHashType
// ---------------------------------------------------------------------------

std::unique_ptr<PostingsArray> FreqProxTermsHashType::new_per_field(size_t size) const {
    const bool has_freq = index_options_ >= IndexOptions::DocsAndFreqs;
    const bool has_prox = index_options_ >= IndexOptions::DocsAndFreqsAndPositions;
    const bool has_offsets = index_options_ >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    return std::make_unique<FreqProxPostingsArray>(size, has_freq, has_prox, has_offsets);
}

// ---------------------------------------------------------------------------
// FreqProxTermsWriterPerField
// ---------------------------------------------------------------------------

FreqProxTermsWriterPerField::FreqProxTermsWriterPerField(std::shared_ptr<FieldInfo> field_info,
                                                         TermVectorsPerField *next_per_field)
        : field_info_(std::move(field_info)),
          next_per_field_(next_per_field),
          base_(stream_count_for(field_info_->index_options),
                std::make_unique<FreqProxTermsHashType>(field_info_->index_options), field_info_->index_options) {
    const auto index_options = field_info_->index_options;
    has_freq_ = index_options >= IndexOptions::DocsAndFreqs;
    has_prox_ = index_options >= IndexOptions::DocsAndFreqsAndPositions;
    has_offsets_ = index_options >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
}

VoidResult FreqProxTermsWriterPerField::write_prox(size_t term_id, int32_t prox_code,
                                                   const FieldInvertState &field_state,
                                                   const AttributeSource &attributes, IntBlockPool &int_pool,
                                                   ByteBlockPool &byte_pool) {
    const std::optional<BytesRef> *payload = nullptr;
    if (const auto *attribute = attributes.get_attribute<PayloadAttribute>(); attribute != nullptr) {
        payload = &attribute->payload();
    }

    if (payload != nullptr && payload->has_value() && payload->value().length() > 0) {
        const auto &bytes = payload->value();
        if (auto written = base_.write_vint(1, (prox_code << 1) | 1, int_pool, byte_pool); !written.has_value()) {
            return written;
        }
        if (auto written = base_.write_vint(1, static_cast<int32_t>(bytes.length()), int_pool, byte_pool);
            !written.has_value()) {
            return written;
        }
        if (auto written = base_.write_bytes(1, bytes.as_slice(), 0, bytes.length(), int_pool, byte_pool);
            !written.has_value()) {
            return written;
        }
        saw_payloads_ = true;
    } else {
        if (auto written = base_.write_vint(1, prox_code << 1, int_pool, byte_pool); !written.has_value()) {
            return written;
        }
    }

    auto postings = freq_prox_array();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    if (!postings.value()->last_positions.has_value()) {
        return err(LuceneError::illegal_state("last_positions not initialized"));
    }
    (*postings.value()->last_positions)[term_id] = field_state.position();
    return {};
}

VoidResult FreqProxTermsWriterPerField::write_offsets(size_t term_id, int32_t offset_accum,
                                                      const AttributeSource &attributes, IntBlockPool &int_pool,
                                                      ByteBlockPool &byte_pool) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (const auto *attribute = attributes.get_attribute<OffsetAttribute>(); attribute != nullptr) {
        start = attribute->start_offset();
        end = attribute->end_offset();
    }

    const auto start_offset = offset_accum + static_cast<int32_t>(start);
    const auto end_offset = offset_accum + static_cast<int32_t>(end);

    auto postings = freq_prox_array();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    if (!postings.value()->last_offsets.has_value()) {
        return err(LuceneError::illegal_state("last_offsets not available"));
    }

    auto &last_offsets = postings.value()->last_offsets.value();
    const auto last_offset = last_offsets.at(term_id);
    debug_check(start_offset - last_offset >= 0, "start_offset must not go backwards");
    const auto v1 = start_offset - last_offset;
    const auto v2 = end_offset - start_offset;
    last_offsets[term_id] = start_offset;

    if (auto written = base_.write_vint(1, v1, int_pool, byte_pool); !written.has_value()) {
        return written;
    }
    return base_.write_vint(1, v2, int_pool, byte_pool);
}

Result<int32_t> FreqProxTermsWriterPerField::get_term_freq(const AttributeSource &attributes) const {
    int32_t freq = 1;
    if (const auto *attribute = attributes.get_attribute<TermFrequencyAttribute>(); attribute != nullptr) {
        freq = static_cast<int32_t>(attribute->term_frequency());
    }

    if (freq != 1 && has_prox_) {
        return err(LuceneError::illegal_state("field \"" + field_info_->name +
                                              "\": cannot index positions while using custom "
                                              "TermFrequencyAttribute"));
    }
    return freq;
}

Result<FreqProxPostingsArray *> FreqProxTermsWriterPerField::freq_prox_array() {
    auto *postings = base_.postings_array_mut();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings_array not initialized"));
    }
    auto *freq_prox = dynamic_cast<FreqProxPostingsArray *>(postings);
    if (freq_prox == nullptr) {
        return err(LuceneError::illegal_state("expected a FreqProx posting array"));
    }
    return freq_prox;
}

VoidResult FreqProxTermsWriterPerField::finish(int32_t field_index) {
    if (next_per_field_ != nullptr) {
        if (auto finished = next_per_field_->finish(field_index); !finished.has_value()) {
            return finished;
        }
    }
    if (saw_payloads_) {
        field_info_->store_payloads = true;
    }
    return {};
}

void FreqProxTermsWriterPerField::reset(ByteBlockPool &byte_pool) {
    base_.reset(byte_pool);
    if (next_per_field_ != nullptr) {
        next_per_field_->reset(byte_pool);
    }
}

VoidResult FreqProxTermsWriterPerField::start(const IndexableField &field, bool first, ByteBlockPool &byte_pool) {
    if (next_per_field_ != nullptr) {
        if (auto started = next_per_field_->start(field, first, byte_pool); !started.has_value()) {
            return started;
        }
    }
    return {};
}

VoidResult FreqProxTermsWriterPerField::add_with_bytes_ref(const BytesRef *term_bytes, int32_t doc_id,
                                                           FieldInvertState &field_state,
                                                           AttributeSource &attributes, IndexContext &context) {
    debug_check(base_.assert_doc_id(doc_id), "the document ids must be non-decreasing");

    // This writer is first in the chain, so it must intern the term text.
    std::optional<BytesRef> fallback;
    const BytesRef *bytes = term_bytes;
    if (bytes == nullptr) {
        if (const auto *bytes_attribute = attributes.get_attribute<BytesTermAttribute>(); bytes_attribute != nullptr) {
            const auto slice = bytes_attribute->bytes_ref();
            fallback.emplace(BytesRef(std::vector<uint8_t>(slice.begin(), slice.end())));
        } else if (const auto *term_attribute = attributes.get_attribute<CharTermAttribute>();
                   term_attribute != nullptr) {
            fallback.emplace(BytesRef::from_string(term_attribute->text()));
        } else {
            return err(LuceneError::illegal_state("term bytes and attribute source bytes are both missing"));
        }
        bytes = &fallback.value();
    }

    auto added = base_.bytes_hash().add(*bytes, context.byte_pool);
    if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
    }

    size_t term_id = 0;
    if (added.value() >= 0) {
        term_id = static_cast<size_t>(added.value());
        if (auto initialized =
                    base_.init_stream_slices(term_id, doc_id, context.freq_prox_term_int_pool, context.byte_pool);
            !initialized.has_value()) {
            return initialized;
        }
        if (auto inverted = new_term(term_id, doc_id, field_state, attributes, context.freq_prox_term_int_pool,
                                     context.byte_pool);
            !inverted.has_value()) {
            return inverted;
        }
    } else {
        auto positioned = base_.position_stream_slice(added.value(), doc_id);
        if (!positioned.has_value()) {
            return std::unexpected(std::move(positioned.error()));
        }
        term_id = positioned.value();
        if (auto inverted = add_term(term_id, doc_id, field_state, attributes, context.freq_prox_term_int_pool,
                                     context.byte_pool);
            !inverted.has_value()) {
            return inverted;
        }
    }

    if (next_per_field_ != nullptr && next_per_field_->do_vectors()) {
        const auto *postings = base_.postings_array();
        if (postings == nullptr) {
            return err(LuceneError::illegal_state("postings_array not initialized"));
        }
        const auto text_start = postings->text_starts.at(term_id);
        if (auto vectored = next_per_field_->add_with_text_start(
                    text_start, doc_id, field_state, attributes, context.term_vectors_int_pool,
                    context.term_vectors_byte_pool, context.byte_pool);
            !vectored.has_value()) {
            return vectored;
        }
    }
    return {};
}

VoidResult FreqProxTermsWriterPerField::new_term(size_t term_id, int32_t doc_id, FieldInvertState &field_state,
                                                 const AttributeSource &attributes, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool) {
    auto freq = get_term_freq(attributes);
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }
    const int32_t tf = freq.value();

    auto array = freq_prox_array();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    auto *postings = array.value();

    postings->last_doc_ids[term_id] = doc_id;

    if (!has_freq_) {
        debug_check(!postings->term_freqs.has_value(), "term freqs must be absent without indexed freqs");
        postings->last_doc_codes[term_id] = doc_id;
        field_state.set_max_term_frequency(std::max(field_state.max_term_frequency(), 1));
    } else {
        postings->last_doc_codes[term_id] = doc_id << 1;
        if (!postings->term_freqs.has_value()) {
            return err(LuceneError::illegal_state("term_freqs not initialized"));
        }
        postings->term_freqs.value()[term_id] = tf;

        if (has_prox_) {
            if (auto written = write_prox(term_id, field_state.position(), field_state, attributes, int_pool,
                                          byte_pool);
                !written.has_value()) {
                return written;
            }
            if (has_offsets_) {
                if (auto written = write_offsets(term_id, field_state.offset(), attributes, int_pool, byte_pool);
                    !written.has_value()) {
                    return written;
                }
            }
        } else {
            debug_check(!has_offsets_, "offsets require positions");
        }

        field_state.set_max_term_frequency(std::max(field_state.max_term_frequency(), tf));
    }
    field_state.set_unique_term_count(field_state.unique_term_count() + 1);
    return {};
}

VoidResult FreqProxTermsWriterPerField::add_term(size_t term_id, int32_t doc_id, FieldInvertState &field_state,
                                                 const AttributeSource &attributes, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool) {
    auto freq = get_term_freq(attributes);
    if (!freq.has_value()) {
        return std::unexpected(std::move(freq.error()));
    }
    const int32_t tf = freq.value();

    auto array = freq_prox_array();
    if (!array.has_value()) {
        return std::unexpected(std::move(array.error()));
    }
    auto *postings = array.value();

    if (has_freq_) {
        debug_check(postings->term_freqs.has_value() && postings->term_freqs.value()[term_id] > 0,
                    "an existing term must have a positive frequency");
    }

    if (!has_freq_) {
        debug_check(!postings->term_freqs.has_value(), "term freqs must be absent without indexed freqs");
        if (tf != 1) {
            return err(LuceneError::illegal_state("field \"" + field_info_->name +
                                                  "\": must index term freq while using custom "
                                                  "TermFrequencyAttribute"));
        }

        if (doc_id != postings->last_doc_ids[term_id]) {
            debug_check(doc_id > postings->last_doc_ids[term_id], "the document ids must be non-decreasing");
            const auto code = postings->last_doc_codes[term_id];
            postings->last_doc_codes[term_id] = doc_id - postings->last_doc_ids[term_id];
            postings->last_doc_ids[term_id] = doc_id;
            field_state.set_unique_term_count(field_state.unique_term_count() + 1);
            return base_.write_vint(0, code, int_pool, byte_pool);
        }
        return {};
    }

    if (doc_id != postings->last_doc_ids[term_id]) {
        debug_check(doc_id > postings->last_doc_ids[term_id], "the document ids must be non-decreasing");
        if (!postings->term_freqs.has_value()) {
            return err(LuceneError::illegal_state("term_freqs not initialized"));
        }
        const auto previous_freq = postings->term_freqs.value()[term_id];

        // Now that the doc freq of the previous document is known, write it and
        // the lastDocCode.
        if (auto written = base_.write_vint(0, previous_freq == 1 ? (postings->last_doc_codes[term_id] | 1)
                                                                 : postings->last_doc_codes[term_id],
                                            int_pool, byte_pool);
            !written.has_value()) {
            return written;
        }
        if (previous_freq != 1) {
            if (auto written = base_.write_vint(0, previous_freq, int_pool, byte_pool); !written.has_value()) {
                return written;
            }
        }

        // Init the freq for the current document.
        postings->term_freqs.value()[term_id] = tf;
        field_state.set_max_term_frequency(std::max(field_state.max_term_frequency(), tf));

        postings->last_doc_codes[term_id] = (doc_id - postings->last_doc_ids[term_id]) << 1;
        postings->last_doc_ids[term_id] = doc_id;

        if (has_prox_ && has_offsets_) {
            if (!postings->last_offsets.has_value()) {
                return err(LuceneError::illegal_state("last_offsets not initialized"));
            }
            postings->last_offsets.value()[term_id] = 0;
        }

        if (has_prox_) {
            if (auto written = write_prox(term_id, field_state.position(), field_state, attributes, int_pool,
                                          byte_pool);
                !written.has_value()) {
                return written;
            }
            if (has_offsets_) {
                if (auto written = write_offsets(term_id, field_state.offset(), attributes, int_pool, byte_pool);
                    !written.has_value()) {
                    return written;
                }
            }
        } else {
            debug_check(!has_offsets_, "offsets require positions");
        }
        field_state.set_unique_term_count(field_state.unique_term_count() + 1);
        return {};
    }

    if (!postings->term_freqs.has_value()) {
        return err(LuceneError::illegal_state("term_freqs not initialized"));
    }
    auto &term_freqs = postings->term_freqs.value();
    const auto accumulated = static_cast<int64_t>(term_freqs[term_id]) + tf;
    if (accumulated > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::illegal_state("term frequency overflow"));
    }
    term_freqs[term_id] = static_cast<int32_t>(accumulated);
    field_state.set_max_term_frequency(std::max(field_state.max_term_frequency(), term_freqs[term_id]));

    if (has_prox_) {
        if (!postings->last_positions.has_value()) {
            return err(LuceneError::illegal_state("last_positions not initialized"));
        }
        const auto delta = field_state.position() - postings->last_positions.value()[term_id];
        if (auto written = write_prox(term_id, delta, field_state, attributes, int_pool, byte_pool);
            !written.has_value()) {
            return written;
        }
        if (has_offsets_) {
            if (auto written = write_offsets(term_id, field_state.offset(), attributes, int_pool, byte_pool);
                !written.has_value()) {
                return written;
            }
        }
    }
    return {};
}

} // namespace lucene
