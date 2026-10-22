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

module lucene_core:push_postings_writer_base.impl;

import :block_term_state;
import :bytes_ref;
import :debug;
import :error;
import :field_infos;
import :fixed_bit_set;
import :norms_producer;
import :numeric_doc_values;
import :postings_enum;
import :postings_writer_base;
import :push_postings_writer_base;
import :terms_enum;
import :types;

import std;

namespace lucene {

PushPostingsWriterBase::PushPostingsWriterBase(std::unique_ptr<PushPostingsWriterBaseAbstract> sub)
    : sub_(std::move(sub)) {
    debug_check(sub_ != nullptr, "PushPostingsWriterBase requires a sub writer");
}

VoidResult PushPostingsWriterBase::init(IndexOutput &terms_out, const SegmentWriteState &state,
                                        const SegmentInfo &segment_info) {
    return sub_->init(terms_out, state, segment_info);
}

Result<WriteTermResult> PushPostingsWriterBase::write_term(const BytesRef & /*term*/, TermsEnum &terms_enum,
                                                           FixedBitSet &docs_seen, const NormsProducer *norms,
                                                           std::unique_ptr<PostingsEnum> postings_enum) {
    if (!field_info_.has_value()) {
        return err(LuceneError::illegal_state("postings field info is not initialized"));
    }
    const FieldInfo &field_info = field_info_.value();

    std::unique_ptr<NumericDocValues> norm_values;
    if (field_info.has_norms()) {
        if (norms == nullptr) {
            return err(LuceneError::illegal_state("norms producer is missing"));
        }
        auto values = norms->get_norms(field_info);
        if (!values) {
            return err(std::move(values.error()));
        }
        norm_values = std::move(values.value());
    }

    if (auto started = sub_->start_term(options_); !started) {
        return err(std::move(started.error()));
    }

    auto postings = terms_enum.postings_with_flags(std::move(postings_enum), enum_flags_);
    if (!postings) {
        return err(std::move(postings.error()));
    }
    PostingsEnum &postings_ref = *postings.value();

    int32_t doc_freq = 0;
    int64_t total_term_freq = 0;
    while (true) {
        auto doc_id = postings_ref.next_doc();
        if (!doc_id) {
            return err(std::move(doc_id.error()));
        }
        if (doc_id.value() == NO_MORE_DOCS) {
            break;
        }
        ++doc_freq;
        docs_seen.set(static_cast<size_t>(doc_id.value()));

        int32_t freq = -1;
        if (options_.write_freqs) {
            auto frequency = postings_ref.freq();
            if (!frequency) {
                return err(std::move(frequency.error()));
            }
            freq = frequency.value();
            total_term_freq += static_cast<int64_t>(freq);
        }

        if (auto start = sub_->start_doc(norm_values.get(), doc_id.value(), freq, options_); !start) {
            return err(std::move(start.error()));
        }

        if (options_.write_positions) {
            for (int32_t i = 0; i < freq; ++i) {
                auto position = postings_ref.next_position();
                if (!position) {
                    return err(std::move(position.error()));
                }

                // rlucene maps the payload to a borrowed `BytesRef`; the port's
                // `get_payload` returns a span into the reader's buffer, so the
                // bytes are copied into a `BytesRef` that outlives the call.
                std::optional<BytesRef> payload_storage;
                if (options_.write_payloads) {
                    auto payload = postings_ref.get_payload();
                    if (!payload) {
                        return err(std::move(payload.error()));
                    }
                    if (payload.value().has_value()) {
                        const std::span<const uint8_t> bytes = payload.value().value();
                        payload_storage = BytesRef(std::vector<uint8_t>(bytes.begin(), bytes.end()));
                    }
                }

                int32_t start_offset = -1;
                int32_t end_offset = -1;
                if (options_.write_offsets) {
                    auto start = postings_ref.start_offset();
                    if (!start) {
                        return err(std::move(start.error()));
                    }
                    auto end = postings_ref.end_offset();
                    if (!end) {
                        return err(std::move(end.error()));
                    }
                    start_offset = start.value();
                    end_offset = end.value();
                }

                if (auto added = sub_->add_position(position.value(),
                                                    payload_storage.has_value() ? &payload_storage.value() : nullptr,
                                                    start_offset, end_offset, options_);
                    !added) {
                    return err(std::move(added.error()));
                }
            }
        }

        if (auto finished = sub_->finish_doc(); !finished) {
            return err(std::move(finished.error()));
        }
    }

    if (doc_freq == 0) {
        return WriteTermResult{std::move(postings.value()), std::nullopt};
    }

    auto state = sub_->new_term_state();
    if (!state) {
        return err(std::move(state.error()));
    }
    auto block_state = state.value().get_block_term_state_mut();
    if (!block_state) {
        return err(std::move(block_state.error()));
    }
    (*block_state)->doc_freq = doc_freq;
    (*block_state)->total_term_freq = options_.write_freqs ? total_term_freq : -1;
    if (auto finished = sub_->finish_term(state.value(), options_); !finished) {
        return err(std::move(finished.error()));
    }

    WriteTermResult result;
    result.postings = std::move(postings.value());
    result.state = std::move(state.value());
    return result;
}

VoidResult PushPostingsWriterBase::encode_term(DataOutput &out, const FieldInfo &field_info,
                                               const TermStateEnum &state, bool absolute) {
    return sub_->encode_term_with_option(out, field_info, state, absolute, options_);
}

void PushPostingsWriterBase::set_field(const FieldInfo &field_info) {
    index_options_ = field_info.index_options;
    options_.write_freqs = index_options_ >= IndexOptions::DocsAndFreqs;
    options_.write_positions = index_options_ >= IndexOptions::DocsAndFreqsAndPositions;
    options_.write_offsets = index_options_ >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    options_.write_payloads = field_info.has_payloads();
    field_info_ = field_info;

    if (!options_.write_freqs) {
        enum_flags_ = 0;
    } else if (!options_.write_positions) {
        enum_flags_ = FREQS;
    } else if (!options_.write_offsets) {
        enum_flags_ = options_.write_payloads ? PAYLOADS : POSITIONS;
    } else if (options_.write_payloads) {
        enum_flags_ = PAYLOADS | OFFSETS;
    } else {
        enum_flags_ = OFFSETS;
    }

    sub_->set_field(field_info);
}

VoidResult PushPostingsWriterBase::close() {
    return sub_->close();
}

} // namespace lucene
