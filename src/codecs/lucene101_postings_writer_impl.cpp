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

module lucene_core:lucene101_postings_writer.impl;

import :array_util;
import :bit_util;
import :block_term_state;
import :codec_util;
import :constants;
import :data_output;
import :debug;
import :error;
import :field_infos;
import :for_delta_util;
import :growable_byte_array_data_output;
import :impact;
import :index_output;
import :lucene101_postings_format;
import :lucene101_postings_writer;
import :numeric_doc_values;
import :pfor_util;
import :postings_util;
import :push_postings_writer_base;
import :segment_info;
import :segment_write_state;
import :types;

import std;

namespace lucene {

namespace {

constexpr size_t BLOCK_SIZE = Lucene101PostingsFormat::BLOCK_SIZE;
constexpr int32_t LEVEL1_MASK = Lucene101PostingsFormat::LEVEL1_MASK;

} // namespace

VoidResult write_vint15(DataOutput &out, int32_t v) {
    debug_check(v >= 0);
    return write_vlong15(out, static_cast<int64_t>(v));
}

VoidResult write_vlong15(DataOutput &out, int64_t v) {
    debug_check(v >= 0);
    if ((v & ~int64_t{0x7FFF}) == 0) {
        return out.write_short(static_cast<int16_t>(v));
    }
    const auto prefix = static_cast<int32_t>(0x8000 | (v & 0x7FFF));
    if (auto written = out.write_short(static_cast<int16_t>(prefix)); !written) {
        return written;
    }
    return out.write_vlong(v >> 15);
}

VoidResult write_impacts(std::span<const Impact> impacts, DataOutput &out) {
    int32_t previous_freq = 0;
    int64_t previous_norm = 0;
    for (const Impact &impact : impacts) {
        debug_check(impact.freq > previous_freq);
        debug_check(static_cast<uint64_t>(impact.norm) > static_cast<uint64_t>(previous_norm));
        const int32_t freq_delta = impact.freq - previous_freq - 1;
        // `wrapping_sub(1)` of the reference implementation: norms are compared as unsigned.
        const auto norm_delta = static_cast<int64_t>(static_cast<uint64_t>(impact.norm) -
                                                    static_cast<uint64_t>(previous_norm) - 1);
        if (norm_delta == 0) {
            // Most of the time the norm only grows by one, so everything folds
            // into a single byte.
            if (auto written = out.write_vint(freq_delta << 1); !written) {
                return written;
            }
        } else {
            if (auto written = out.write_vint((freq_delta << 1) | 1); !written) {
                return written;
            }
            if (auto written = out.write_zlong(norm_delta); !written) {
                return written;
            }
        }
        previous_freq = impact.freq;
        previous_norm = impact.norm;
    }
    return {};
}

Result<std::unique_ptr<Lucene101PostingsWriter>> Lucene101PostingsWriter::create(const SegmentWriteState &state,
                                                                                const SegmentInfo &segment_info) {
    std::unique_ptr<Lucene101PostingsWriter> writer{new Lucene101PostingsWriter()};

    const std::string meta_file =
            Lucene101PostingsFormat::file_name(segment_info.name, state.segment_suffix,
                                              Lucene101PostingsFormat::META_EXTENSION);
    const std::string doc_file =
            Lucene101PostingsFormat::file_name(segment_info.name, state.segment_suffix,
                                              Lucene101PostingsFormat::DOC_EXTENSION);

    auto meta = state.directory.create_output(meta_file, state.context);
    if (!meta) {
        return err(std::move(meta.error()));
    }
    writer->meta_out_ = std::move(meta.value());

    auto doc = state.directory.create_output(doc_file, state.context);
    if (!doc) {
        writer->close_quietly();
        return err(std::move(doc.error()));
    }
    writer->doc_out_ = std::move(doc.value());

    const std::span<const uint8_t> id(segment_info.id);
    if (auto header = CodecUtil::write_index_header(*writer->meta_out_, Lucene101PostingsFormat::META_CODEC,
                                                    Lucene101PostingsFormat::VERSION_CURRENT, id,
                                                    state.segment_suffix);
        !header) {
        writer->close_quietly();
        return err(std::move(header.error()));
    }
    if (auto header = CodecUtil::write_index_header(*writer->doc_out_, Lucene101PostingsFormat::DOC_CODEC,
                                                    Lucene101PostingsFormat::VERSION_CURRENT, id,
                                                    state.segment_suffix);
        !header) {
        writer->close_quietly();
        return err(std::move(header.error()));
    }

    const FieldInfos *field_infos = state.field_infos.get();
    const bool has_positions = field_infos != nullptr && field_infos->has_positions();
    const bool has_payloads = field_infos != nullptr && field_infos->has_payloads();
    const bool has_offsets = field_infos != nullptr && field_infos->has_offsets();

    if (has_positions) {
        writer->pos_delta_buffer_.assign(BLOCK_SIZE, 0);
        const std::string pos_file =
                Lucene101PostingsFormat::file_name(segment_info.name, state.segment_suffix,
                                                   Lucene101PostingsFormat::POS_EXTENSION);
        auto pos = state.directory.create_output(pos_file, state.context);
        if (!pos) {
            writer->close_quietly();
            return err(std::move(pos.error()));
        }
        writer->pos_out_ = std::move(pos.value());
        if (auto header = CodecUtil::write_index_header(*writer->pos_out_, Lucene101PostingsFormat::POS_CODEC,
                                                        Lucene101PostingsFormat::VERSION_CURRENT, id,
                                                        state.segment_suffix);
            !header) {
            writer->close_quietly();
            return err(std::move(header.error()));
        }

        if (has_payloads) {
            writer->payload_bytes_.assign(128, 0);
            writer->payload_length_buffer_.assign(BLOCK_SIZE, 0);
        }
        if (has_offsets) {
            writer->offset_start_delta_buffer_.assign(BLOCK_SIZE, 0);
            writer->offset_length_buffer_.assign(BLOCK_SIZE, 0);
        }
        if (has_payloads || has_offsets) {
            const std::string pay_file =
                    Lucene101PostingsFormat::file_name(segment_info.name, state.segment_suffix,
                                                       Lucene101PostingsFormat::PAY_EXTENSION);
            auto pay = state.directory.create_output(pay_file, state.context);
            if (!pay) {
                writer->close_quietly();
                return err(std::move(pay.error()));
            }
            writer->pay_out_ = std::move(pay.value());
            if (auto header = CodecUtil::write_index_header(*writer->pay_out_, Lucene101PostingsFormat::PAY_CODEC,
                                                            Lucene101PostingsFormat::VERSION_CURRENT, id,
                                                            state.segment_suffix);
                !header) {
                writer->close_quietly();
                return err(std::move(header.error()));
            }
        }
    }

    writer->doc_delta_buffer_.assign(BLOCK_SIZE, 0);
    writer->freq_buffer_.assign(BLOCK_SIZE, 0);
    return writer;
}

Result<IndexOutput *> Lucene101PostingsWriter::pos_out() {
    if (pos_out_ == nullptr) {
        return err(LuceneError::illegal_state("positions output is missing"));
    }
    return pos_out_.get();
}

Result<IndexOutput *> Lucene101PostingsWriter::pay_out() {
    if (pay_out_ == nullptr) {
        return err(LuceneError::illegal_state("payload output is missing"));
    }
    return pay_out_.get();
}

void Lucene101PostingsWriter::close_quietly() noexcept {
    for (std::unique_ptr<IndexOutput> *out : {&meta_out_, &doc_out_, &pos_out_, &pay_out_}) {
        if (*out != nullptr) {
            static_cast<void>((*out)->close());
            out->reset();
        }
    }
}

VoidResult Lucene101PostingsWriter::init(IndexOutput &terms_out, const SegmentWriteState &state,
                                        const SegmentInfo &segment_info) {
    const std::span<const uint8_t> id(segment_info.id);
    if (auto header = CodecUtil::write_index_header(terms_out, Lucene101PostingsFormat::TERMS_CODEC,
                                                    Lucene101PostingsFormat::VERSION_CURRENT, id,
                                                    state.segment_suffix);
        !header) {
        return header;
    }
    return terms_out.write_vint(static_cast<int32_t>(BLOCK_SIZE));
}

Result<TermStateEnum> Lucene101PostingsWriter::new_term_state() {
    return TermStateEnum{IntBlockTermState{}};
}

void Lucene101PostingsWriter::set_field(const FieldInfo &field_info) {
    last_state_ = IntBlockTermState{};
    field_has_norms_ = field_info.has_norms();
}

VoidResult Lucene101PostingsWriter::start_term(const FieldWriteOptions &options) {
    auto doc_pointer = doc_out_->get_file_pointer();
    if (!doc_pointer) {
        return std::unexpected(std::move(doc_pointer.error()));
    }
    doc_start_fp_ = static_cast<int64_t>(doc_pointer.value());

    if (options.write_positions && pos_out_ != nullptr) {
        auto pos_pointer = pos_out_->get_file_pointer();
        if (!pos_pointer) {
            return std::unexpected(std::move(pos_pointer.error()));
        }
        pos_start_fp_ = static_cast<int64_t>(pos_pointer.value());
        level0_last_pos_fp_ = pos_start_fp_;
        level1_last_pos_fp_ = pos_start_fp_;

        if (options.write_payloads || options.write_offsets) {
            auto pay = pay_out();
            if (!pay) {
                return std::unexpected(std::move(pay.error()));
            }
            auto pay_pointer = (*pay)->get_file_pointer();
            if (!pay_pointer) {
                return std::unexpected(std::move(pay_pointer.error()));
            }
            pay_start_fp_ = static_cast<int64_t>(pay_pointer.value());
            level0_last_pay_fp_ = pay_start_fp_;
            level1_last_pay_fp_ = pay_start_fp_;
        }
    }

    last_doc_id_ = -1;
    level0_last_doc_id_ = -1;
    level1_last_doc_id_ = -1;
    if (options.write_freqs) {
        level0_freq_norm_accumulator_.clear();
    }
    return {};
}

VoidResult Lucene101PostingsWriter::start_doc(NumericDocValues *norms, int32_t doc_id, int32_t term_doc_freq,
                                              const FieldWriteOptions &options) {
    if (doc_buffer_upto_ == BLOCK_SIZE) {
        if (auto flushed = flush_doc_block(false, options); !flushed) {
            return flushed;
        }
        doc_buffer_upto_ = 0;
    }

    const int32_t doc_delta = doc_id - last_doc_id_;
    if (doc_id < 0 || doc_delta <= 0) {
        return err(LuceneError::corrupt_index(
                std::format("docs out of order ({} <= {})", doc_id, last_doc_id_)));
    }

    const size_t idx = doc_buffer_upto_;
    doc_delta_buffer_[idx] = doc_delta;
    if (options.write_freqs) {
        freq_buffer_[idx] = term_doc_freq;
    }

    doc_id_ = doc_id;
    last_position_ = 0;
    last_start_offset_ = 0;

    if (options.write_freqs) {
        int64_t norm = 1;
        if (field_has_norms_) {
            if (norms == nullptr) {
                return err(LuceneError::illegal_state("norms should not be null"));
            }
            auto found = norms->advance_exact(doc_id);
            if (!found) {
                return std::unexpected(std::move(found.error()));
            }
            if (found.value()) {
                auto value = norms->long_value();
                if (!value) {
                    return std::unexpected(std::move(value.error()));
                }
                norm = value.value();
                debug_check(norm != 0);
            }
        }
        level0_freq_norm_accumulator_.add(term_doc_freq, norm);
    }

    return {};
}

VoidResult Lucene101PostingsWriter::add_position(int32_t position, const BytesRef *payload, int32_t start_offset,
                                                 int32_t end_offset, const FieldWriteOptions &options) {
    if (position > MAX_POSITION) {
        return err(LuceneError::corrupt_index(
                std::format("position={} is too large (> MAX_POSITION={})", position, MAX_POSITION)));
    }
    if (position < 0) {
        return err(LuceneError::corrupt_index(std::format("position={} is < 0", position)));
    }

    const size_t idx = pos_buffer_upto_;
    pos_delta_buffer_[idx] = position - last_position_;

    if (options.write_payloads) {
        const size_t payload_length =
                payload != nullptr ? static_cast<size_t>(payload->length()) : size_t{0};
        if (payload_length == 0) {
            payload_length_buffer_[idx] = 0;
        } else {
            payload_length_buffer_[idx] = static_cast<int32_t>(payload_length);
            if (payload_byte_upto_ + payload_length > payload_bytes_.size()) {
                if (auto grown = ArrayUtil::grow_capacity(payload_bytes_, payload_byte_upto_ + payload_length);
                    !grown) {
                    return grown;
                }
            }
            const std::span<const uint8_t> bytes = payload->as_slice();
            payload_bytes_.resize(payload_byte_upto_);
            payload_bytes_.insert(payload_bytes_.end(), bytes.begin(), bytes.end());
            payload_byte_upto_ += payload_length;
        }
    }

    if (options.write_offsets) {
        debug_check(start_offset >= last_start_offset_);
        debug_check(end_offset >= start_offset);
        offset_start_delta_buffer_[idx] = start_offset - last_start_offset_;
        offset_length_buffer_[idx] = end_offset - start_offset;
        last_start_offset_ = start_offset;
    }

    pos_buffer_upto_ += 1;
    last_position_ = position;

    if (pos_buffer_upto_ == BLOCK_SIZE) {
        if (pos_out_ == nullptr) {
            return err(LuceneError::illegal_state("positions output is missing"));
        }
        if (auto encoded = pfor_util_.encode(std::span<int32_t>(pos_delta_buffer_), *pos_out_); !encoded) {
            return encoded;
        }
        if (options.write_payloads) {
            if (pay_out_ == nullptr) {
                return err(LuceneError::illegal_state("payload output is missing"));
            }
            if (auto encoded = pfor_util_.encode(std::span<int32_t>(payload_length_buffer_), *pay_out_); !encoded) {
                return encoded;
            }
            if (auto written = pay_out_->write_vint(static_cast<int32_t>(payload_byte_upto_)); !written) {
                return written;
            }
            if (auto written = pay_out_->write_bytes(payload_bytes_, 0, payload_byte_upto_); !written) {
                return written;
            }
            payload_byte_upto_ = 0;
        }
        if (options.write_offsets) {
            if (pay_out_ == nullptr) {
                return err(LuceneError::illegal_state("payload output is missing"));
            }
            if (auto encoded = pfor_util_.encode(std::span<int32_t>(offset_start_delta_buffer_), *pay_out_);
                !encoded) {
                return encoded;
            }
            if (auto encoded = pfor_util_.encode(std::span<int32_t>(offset_length_buffer_), *pay_out_); !encoded) {
                return encoded;
            }
        }
        pos_buffer_upto_ = 0;
    }

    return {};
}

VoidResult Lucene101PostingsWriter::finish_doc() {
    doc_buffer_upto_ += 1;
    doc_count_ += 1;
    last_doc_id_ = doc_id_;
    return {};
}

VoidResult Lucene101PostingsWriter::flush_doc_block(bool finish_term, const FieldWriteOptions &options) {
    debug_check(doc_buffer_upto_ != 0);

    if (doc_buffer_upto_ < BLOCK_SIZE) {
        debug_check(finish_term);
        if (auto written = PostingsUtil::write_vint_block(level0_output_, std::span<int32_t>(doc_delta_buffer_),
                                                          std::span<const int32_t>(freq_buffer_), doc_buffer_upto_,
                                                          options.write_freqs);
            !written) {
            return written;
        }
    } else {
        if (options.write_freqs) {
            const std::span<const Impact> impacts =
                    level0_freq_norm_accumulator_.get_competitive_freq_norm_pairs();
            const auto n = static_cast<int32_t>(impacts.size());
            if (n > max_num_impacts_at_level0_) {
                max_num_impacts_at_level0_ = n;
            }
            if (auto written = write_impacts(impacts, scratch_output_); !written) {
                return written;
            }
            debug_check(level0_output_.size() == 0);
            const size_t scratch_len = scratch_output_.size();
            if (scratch_len > static_cast<size_t>(max_impact_num_bytes_at_level0_)) {
                max_impact_num_bytes_at_level0_ = static_cast<int32_t>(scratch_len);
            }
            if (auto written = level0_output_.write_vlong(static_cast<int64_t>(scratch_len)); !written) {
                return written;
            }
            if (auto copied = scratch_output_.copy_to(level0_output_); !copied) {
                return copied;
            }
            scratch_output_.reset();

            if (options.write_positions) {
                auto pos = pos_out();
                if (!pos) {
                    return std::unexpected(std::move(pos.error()));
                }
                auto pos_pointer = (*pos)->get_file_pointer();
                if (!pos_pointer) {
                    return std::unexpected(std::move(pos_pointer.error()));
                }
                const auto pos_fp = static_cast<int64_t>(pos_pointer.value());
                if (auto written = level0_output_.write_vlong(pos_fp - level0_last_pos_fp_); !written) {
                    return written;
                }
                if (auto written = level0_output_.write_byte(static_cast<uint8_t>(pos_buffer_upto_)); !written) {
                    return written;
                }
                level0_last_pos_fp_ = pos_fp;

                if (options.write_offsets || options.write_payloads) {
                    auto pay = pay_out();
                    if (!pay) {
                        return std::unexpected(std::move(pay.error()));
                    }
                    auto pay_pointer = (*pay)->get_file_pointer();
                    if (!pay_pointer) {
                        return std::unexpected(std::move(pay_pointer.error()));
                    }
                    const auto pay_fp = static_cast<int64_t>(pay_pointer.value());
                    if (auto written = level0_output_.write_vlong(pay_fp - level0_last_pay_fp_); !written) {
                        return written;
                    }
                    if (auto written = level0_output_.write_vint(static_cast<int32_t>(payload_byte_upto_)); !written) {
                        return written;
                    }
                    level0_last_pay_fp_ = pay_fp;
                }
            }
        }

        size_t num_skip_bytes = level0_output_.size();
        if (auto encoded = for_delta_util_.encode_deltas(std::span<int32_t>(doc_delta_buffer_), level0_output_);
            !encoded) {
            return encoded;
        }
        if (options.write_freqs) {
            if (auto encoded = pfor_util_.encode(std::span<int32_t>(freq_buffer_), level0_output_); !encoded) {
                return encoded;
            }
        }
        // The doc id delta is at least 128, so it never fits a single byte vint;
        // even 128-aligned blocks would only just qualify, so go for 2 bytes.
        if (auto written = write_vint15(scratch_output_, doc_id_ - level0_last_doc_id_); !written) {
            return written;
        }
        if (auto written = write_vlong15(scratch_output_, static_cast<int64_t>(level0_output_.size())); !written) {
            return written;
        }
        num_skip_bytes += scratch_output_.size();

        if (auto written = level1_output_.write_vlong(static_cast<int64_t>(num_skip_bytes)); !written) {
            return written;
        }
        if (auto copied = scratch_output_.copy_to(level1_output_); !copied) {
            return copied;
        }
        scratch_output_.reset();
    }

    if (auto copied = level0_output_.copy_to(level1_output_); !copied) {
        return copied;
    }
    level0_output_.reset();

    level0_last_doc_id_ = doc_id_;
    if (options.write_freqs) {
        level1_competitive_freq_norm_accumulator_.add_all(level0_freq_norm_accumulator_);
        level0_freq_norm_accumulator_.clear();
    }

    if ((doc_count_ & LEVEL1_MASK) == 0) {
        // True every 32 blocks (4096 docs).
        if (auto written = write_level1_skip_data(options); !written) {
            return written;
        }
        level1_last_doc_id_ = doc_id_;
        level1_competitive_freq_norm_accumulator_.clear();
    } else if (finish_term) {
        if (auto copied = level1_output_.copy_to(*doc_out_); !copied) {
            return copied;
        }
        level1_output_.reset();
        level1_competitive_freq_norm_accumulator_.clear();
    }

    return {};
}

VoidResult Lucene101PostingsWriter::write_level1_skip_data(const FieldWriteOptions &options) {
    if (auto written = doc_out_->write_vint(doc_id_ - level1_last_doc_id_); !written) {
        return written;
    }

    uint64_t level1_end = 0;
    if (options.write_freqs) {
        const std::span<const Impact> impacts =
                level1_competitive_freq_norm_accumulator_.get_competitive_freq_norm_pairs();
        const auto n = static_cast<int32_t>(impacts.size());
        if (n > max_num_impacts_at_level1_) {
            max_num_impacts_at_level1_ = n;
        }
        if (auto written = write_impacts(impacts, scratch_output_); !written) {
            return written;
        }
        const size_t num_impact_bytes = scratch_output_.size();
        if (num_impact_bytes > static_cast<size_t>(max_impact_num_bytes_at_level1_)) {
            max_impact_num_bytes_at_level1_ = static_cast<int32_t>(num_impact_bytes);
        }
        if (options.write_positions) {
            auto pos = pos_out();
            if (!pos) {
                return std::unexpected(std::move(pos.error()));
            }
            auto pos_pointer = (*pos)->get_file_pointer();
            if (!pos_pointer) {
                return std::unexpected(std::move(pos_pointer.error()));
            }
            const auto pos_fp = static_cast<int64_t>(pos_pointer.value());
            if (auto written = scratch_output_.write_vlong(pos_fp - level1_last_pos_fp_); !written) {
                return written;
            }
            if (auto written = scratch_output_.write_byte(static_cast<uint8_t>(pos_buffer_upto_)); !written) {
                return written;
            }
            level1_last_pos_fp_ = pos_fp;
            if (options.write_offsets || options.write_payloads) {
                auto pay = pay_out();
                if (!pay) {
                    return std::unexpected(std::move(pay.error()));
                }
                auto pay_pointer = (*pay)->get_file_pointer();
                if (!pay_pointer) {
                    return std::unexpected(std::move(pay_pointer.error()));
                }
                const auto pay_fp = static_cast<int64_t>(pay_pointer.value());
                if (auto written = scratch_output_.write_vlong(pay_fp - level1_last_pay_fp_); !written) {
                    return written;
                }
                if (auto written = scratch_output_.write_vint(static_cast<int32_t>(payload_byte_upto_)); !written) {
                    return written;
                }
                level1_last_pay_fp_ = pay_fp;
            }
        }
        const uint64_t level1_len = 2 * BitUtil::SHORT_BYTES + scratch_output_.size() + level1_output_.size();
        if (auto written = doc_out_->write_vlong(static_cast<int64_t>(level1_len)); !written) {
            return written;
        }
        auto doc_pointer = doc_out_->get_file_pointer();
        if (!doc_pointer) {
            return std::unexpected(std::move(doc_pointer.error()));
        }
        level1_end = doc_pointer.value() + level1_len;
        // At most 128 impacts of at most 2 bytes each.
        debug_check(scratch_output_.size() <= static_cast<size_t>(std::numeric_limits<int16_t>::max()));
        debug_check(scratch_output_.size() + BitUtil::SHORT_BYTES <=
                    static_cast<size_t>(std::numeric_limits<int16_t>::max()));
        if (auto written = doc_out_->write_short(
                    static_cast<int16_t>(scratch_output_.size() + BitUtil::SHORT_BYTES));
            !written) {
            return written;
        }
        if (auto written = doc_out_->write_short(static_cast<int16_t>(num_impact_bytes)); !written) {
            return written;
        }
        if (auto copied = scratch_output_.copy_to(*doc_out_); !copied) {
            return copied;
        }
        scratch_output_.reset();
    } else {
        if (auto written = doc_out_->write_vlong(static_cast<int64_t>(level1_output_.size())); !written) {
            return written;
        }
        auto doc_pointer = doc_out_->get_file_pointer();
        if (!doc_pointer) {
            return std::unexpected(std::move(doc_pointer.error()));
        }
        level1_end = doc_pointer.value() + level1_output_.size();
    }

    if (auto copied = level1_output_.copy_to(*doc_out_); !copied) {
        return copied;
    }
    level1_output_.reset();

    auto doc_pointer = doc_out_->get_file_pointer();
    if (!doc_pointer) {
        return std::unexpected(std::move(doc_pointer.error()));
    }
    debug_check(doc_pointer.value() == level1_end);
    return {};
}

VoidResult Lucene101PostingsWriter::finish_term(TermStateEnum &state, const FieldWriteOptions &options) {
    auto *int_state = std::get_if<IntBlockTermState>(&state.value_mut());
    if (int_state == nullptr) {
        return err(LuceneError::illegal_state("not IntBlockTermState"));
    }
    debug_check(int_state->base.doc_freq > 0);
    debug_check(int_state->base.doc_freq == doc_count_);

    int32_t singleton_doc_id = -1;
    if (int_state->base.doc_freq == 1) {
        singleton_doc_id = doc_delta_buffer_[0] - 1;
    } else {
        if (auto flushed = flush_doc_block(true, options); !flushed) {
            return flushed;
        }
    }

    int64_t last_pos_block_offset = -1;
    if (options.write_positions) {
        // totalTermFreq is the total number of positions (payloads, offsets)
        // associated with the current term.
        debug_check(int_state->base.total_term_freq != -1);
        if (int_state->base.total_term_freq > static_cast<int64_t>(BLOCK_SIZE)) {
            auto pos = pos_out();
            if (!pos) {
                return std::unexpected(std::move(pos.error()));
            }
            auto pos_pointer = (*pos)->get_file_pointer();
            if (!pos_pointer) {
                return std::unexpected(std::move(pos_pointer.error()));
            }
            // Record the file offset of the last position block.
            last_pos_block_offset = static_cast<int64_t>(pos_pointer.value()) - pos_start_fp_;
        }

        if (pos_buffer_upto_ > 0) {
            debug_check(pos_buffer_upto_ < BLOCK_SIZE);
            int32_t last_payload_length = -1;
            int32_t last_offset_length = -1;
            size_t payload_bytes_read_upto = 0;
            if (pos_out_ == nullptr) {
                return err(LuceneError::illegal_state("positions output is missing"));
            }
            for (size_t i = 0; i < pos_buffer_upto_; ++i) {
                const int32_t pos_delta = pos_delta_buffer_[i];
                if (options.write_payloads) {
                    const int32_t payload_length = payload_length_buffer_[i];
                    if (payload_length != last_payload_length) {
                        last_payload_length = payload_length;
                        if (auto written = pos_out_->write_vint((pos_delta << 1) | 1); !written) {
                            return written;
                        }
                        if (auto written = pos_out_->write_vint(payload_length); !written) {
                            return written;
                        }
                    } else {
                        if (auto written = pos_out_->write_vint(pos_delta << 1); !written) {
                            return written;
                        }
                    }
                    if (payload_length != 0) {
                        const auto length = static_cast<size_t>(payload_length);
                        if (auto written = pos_out_->write_bytes(payload_bytes_, payload_bytes_read_upto, length);
                            !written) {
                            return written;
                        }
                        payload_bytes_read_upto += length;
                    }
                } else {
                    if (auto written = pos_out_->write_vint(pos_delta); !written) {
                        return written;
                    }
                }
                if (options.write_offsets) {
                    const int32_t delta = offset_start_delta_buffer_[i];
                    const int32_t length = offset_length_buffer_[i];
                    if (length == last_offset_length) {
                        if (auto written = pos_out_->write_vint(delta << 1); !written) {
                            return written;
                        }
                    } else {
                        if (auto written = pos_out_->write_vint((delta << 1) | 1); !written) {
                            return written;
                        }
                        if (auto written = pos_out_->write_vint(length); !written) {
                            return written;
                        }
                        last_offset_length = length;
                    }
                }
            }
            if (options.write_payloads) {
                debug_check(payload_bytes_read_upto == payload_byte_upto_);
                payload_byte_upto_ = 0;
            }
        }
    }

    int_state->doc_start_fp = doc_start_fp_;
    int_state->pos_start_fp = pos_start_fp_;
    int_state->pay_start_fp = pay_start_fp_;
    int_state->singleton_doc_id = singleton_doc_id;
    int_state->last_pos_block_offset = last_pos_block_offset;

    doc_buffer_upto_ = 0;
    pos_buffer_upto_ = 0;
    last_doc_id_ = -1;
    doc_count_ = 0;
    return {};
}

VoidResult Lucene101PostingsWriter::encode_term_with_option(DataOutput &out, const FieldInfo & /*field_info*/,
                                                            const TermStateEnum &state, bool absolute,
                                                            const FieldWriteOptions &options) {
    const auto *int_state = std::get_if<IntBlockTermState>(&state.value());
    if (int_state == nullptr) {
        return err(LuceneError::illegal_state("not IntBlockTermState"));
    }
    if (absolute) {
        last_state_ = IntBlockTermState{};
        debug_check(last_state_.doc_start_fp == 0);
    }

    if (last_state_.singleton_doc_id != -1 && int_state->singleton_doc_id != -1 &&
        int_state->doc_start_fp == last_state_.doc_start_fp) {
        // With runs of rare values such as ID fields, the pointer increment in
        // the docs file is often 0, and some ID schemes are monotonic, so encode
        // the delta between consecutive doc ids to save space.
        const auto delta = static_cast<int64_t>(int_state->singleton_doc_id) - last_state_.singleton_doc_id;
        if (auto written = out.write_vlong((BitUtil::zig_zag_encode_i64(delta) << 1) | 1); !written) {
            return written;
        }
    } else {
        if (auto written = out.write_vlong((int_state->doc_start_fp - last_state_.doc_start_fp) << 1); !written) {
            return written;
        }
        if (int_state->singleton_doc_id != -1) {
            if (auto written = out.write_vint(int_state->singleton_doc_id); !written) {
                return written;
            }
        }
    }

    if (options.write_positions) {
        if (auto written = out.write_vlong(int_state->pos_start_fp - last_state_.pos_start_fp); !written) {
            return written;
        }
        if (options.write_payloads || options.write_offsets) {
            if (auto written = out.write_vlong(int_state->pay_start_fp - last_state_.pay_start_fp); !written) {
                return written;
            }
        }
        if (int_state->last_pos_block_offset != -1) {
            if (auto written = out.write_vlong(int_state->last_pos_block_offset); !written) {
                return written;
            }
        }
    }

    last_state_ = *int_state;
    return {};
}

VoidResult Lucene101PostingsWriter::close() {
    if (closed_) {
        return {};
    }

    VoidResult status{};
    auto record = [&status](VoidResult result) {
        if (status.has_value() && !result.has_value()) {
            status = std::move(result);
        }
    };

    record(CodecUtil::write_footer(*doc_out_));
    if (status.has_value() && pos_out_ != nullptr) {
        record(CodecUtil::write_footer(*pos_out_));
    }
    if (status.has_value() && pay_out_ != nullptr) {
        record(CodecUtil::write_footer(*pay_out_));
    }
    if (status.has_value()) {
        record(meta_out_->write_int(max_num_impacts_at_level0_));
        record(meta_out_->write_int(max_impact_num_bytes_at_level0_));
        record(meta_out_->write_int(max_num_impacts_at_level1_));
        record(meta_out_->write_int(max_impact_num_bytes_at_level1_));

        auto doc_pointer = doc_out_->get_file_pointer();
        if (!doc_pointer) {
            record(std::unexpected(std::move(doc_pointer.error())));
        } else {
            record(meta_out_->write_long(static_cast<int64_t>(doc_pointer.value())));
            if (pos_out_ != nullptr) {
                auto pos_pointer = pos_out_->get_file_pointer();
                if (!pos_pointer) {
                    record(std::unexpected(std::move(pos_pointer.error())));
                } else {
                    record(meta_out_->write_long(static_cast<int64_t>(pos_pointer.value())));
                    if (pay_out_ != nullptr) {
                        auto pay_pointer = pay_out_->get_file_pointer();
                        if (!pay_pointer) {
                            record(std::unexpected(std::move(pay_pointer.error())));
                        } else {
                            record(meta_out_->write_long(static_cast<int64_t>(pay_pointer.value())));
                        }
                    }
                }
            }
        }
        record(CodecUtil::write_footer(*meta_out_));
    }

    if (status.has_value()) {
        record(meta_out_->close());
        record(doc_out_->close());
        if (pos_out_ != nullptr) {
            record(pos_out_->close());
        }
        if (pay_out_ != nullptr) {
            record(pay_out_->close());
        }
    } else {
        close_quietly();
    }

    meta_out_.reset();
    doc_out_.reset();
    pos_out_.reset();
    pay_out_.reset();
    closed_ = true;
    return status;
}

} // namespace lucene
