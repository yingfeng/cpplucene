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

module lucene_core:lucene90_compressing_term_vectors_writer.impl;

import :lucene90_compressing_term_vectors_writer;

import :bit_util;
import :block_packed_writer;
import :byte_array_output;
import :bytes_ref;
import :codec_util;
import :compression;
import :data_input;
import :direct_writer;
import :directory;
import :error;
import :field_infos;
import :index_output;
import :io_context;
import :lucene90_compressing_term_vectors_format;
import :lucene90_fields_index_writer;
import :packed_ints;
import :packed_stream;
import :segment_info;
import :string_helper;
import :term_vectors_writer;
import :types;

import std;

namespace lucene {

namespace {

/// Grows `values` to hold at least `size` entries, zero filling the gap.
void ensure_size(std::vector<int32_t> &values, size_t size) {
    if (values.size() < size) {
        values.resize(size, 0);
    }
}

/// `DirectWriter::bits_required(TV_POSITIONS | TV_OFFSETS | TV_PAYLOADS)`, i.e. the width
/// of a field-flags value.  Computed once because the flags never change.
int32_t flags_bits() {
    static const int32_t bits = [] {
        auto required = DirectWriter::bits_required(static_cast<int64_t>(TV_POSITIONS | TV_OFFSETS | TV_PAYLOADS));
        return required.has_value() ? required.value() : int32_t{4};
    }();
    return bits;
}

} // namespace

// ---- FieldData / DocData ----

void Lucene90CompressingTermVectorsWriter::FieldData::add_term(int32_t freq, int32_t prefix_length,
                                                               int32_t suffix_length) {
    freqs[ord] = freq;
    prefix_lengths[ord] = prefix_length;
    suffix_lengths[ord] = suffix_length;
    ++ord;
}

VoidResult Lucene90CompressingTermVectorsWriter::FieldData::add_position(
        int32_t position, int32_t start_offset, int32_t length, int32_t payload_length,
        std::vector<int32_t> &positions, std::vector<int32_t> &start_offsets, std::vector<int32_t> &lengths,
        std::vector<int32_t> &payload_lengths) {
    if (has_positions) {
        const size_t required = pos_start + total_positions;
        positions[required] = position;
    }
    if (has_offsets) {
        const size_t required = off_start + total_positions;
        start_offsets[required] = start_offset;
        lengths[required] = length;
    }
    if (has_payloads) {
        const size_t required = pay_start + total_positions;
        payload_lengths[required] = payload_length;
    }
    ++total_positions;
    return {};
}

Lucene90CompressingTermVectorsWriter::DocData::DocData(int32_t num_fields_in, size_t pos_start_in,
                                                       size_t off_start_in, size_t pay_start_in)
    : num_fields(num_fields_in), pos_start(pos_start_in), off_start(off_start_in), pay_start(pay_start_in) {
    fields.reserve(static_cast<size_t>(std::max(num_fields_in, 0)));
}

size_t Lucene90CompressingTermVectorsWriter::DocData::add_field(int32_t field_num, size_t num_terms, bool positions,
                                                                bool offsets, bool payloads) {
    size_t pos_start_field = pos_start;
    size_t off_start_field = off_start;
    size_t pay_start_field = pay_start;
    if (!fields.empty()) {
        const FieldData &last = fields.back();
        pos_start_field = last.pos_start + (last.has_positions ? last.total_positions : 0);
        off_start_field = last.off_start + (last.has_offsets ? last.total_positions : 0);
        pay_start_field = last.pay_start + (last.has_payloads ? last.total_positions : 0);
    }

    FieldData field;
    field.has_positions = positions;
    field.has_offsets = offsets;
    field.has_payloads = payloads;
    field.field_num = field_num;
    field.flags = (positions ? TV_POSITIONS : 0) | (offsets ? TV_OFFSETS : 0) | (payloads ? TV_PAYLOADS : 0);
    field.num_terms = num_terms;
    field.freqs.assign(num_terms, 0);
    field.prefix_lengths.assign(num_terms, 0);
    field.suffix_lengths.assign(num_terms, 0);
    field.pos_start = pos_start_field;
    field.off_start = off_start_field;
    field.pay_start = pay_start_field;
    field.total_positions = 0;
    field.ord = 0;

    const size_t index = fields.size();
    fields.push_back(std::move(field));
    return index;
}

// ---- construction ----

Lucene90CompressingTermVectorsWriter::Lucene90CompressingTermVectorsWriter(
        std::string format_name, CompressionMode compression_mode, int32_t chunk_size, int32_t max_docs_per_chunk,
        std::unique_ptr<IndexOutput> meta_stream, std::unique_ptr<IndexOutput> vectors_stream,
        std::unique_ptr<Lucene90FieldsIndexWriter> index_writer)
    : format_name_(std::move(format_name)),
      compression_mode_(compression_mode),
      compressor_(compression_mode.new_compressor()),
      chunk_size_(chunk_size),
      max_docs_per_chunk_(max_docs_per_chunk),
      meta_stream_(std::move(meta_stream)),
      vectors_stream_(std::move(vectors_stream)),
      index_writer_(std::move(index_writer)),
      term_suffixes_("term-suffixes"),
      payload_bytes_("term-payloads"),
      scratch_buffer_("term-vectors-scratch") {}

Lucene90CompressingTermVectorsWriter::~Lucene90CompressingTermVectorsWriter() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90CompressingTermVectorsWriter>> Lucene90CompressingTermVectorsWriter::create(
        Directory &directory, const SegmentInfo &segment_info, std::string_view segment_suffix,
        const IOContext &context, std::string_view format_name, const CompressionMode &compression_mode,
        int32_t chunk_size, int32_t max_docs_per_chunk, int32_t block_shift) {
    if (chunk_size <= 0) {
        return err(LuceneError::illegal_argument("chunk_size must be >= 1"));
    }
    if (max_docs_per_chunk <= 0) {
        return err(LuceneError::illegal_argument("max_docs_per_chunk must be >= 1"));
    }

    auto packed = BlockPackedWriter::create(PACKED_BLOCK_SIZE);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }

    const std::string meta_name = segment_file_name(segment_info.name, segment_suffix, VECTORS_META_EXTENSION);
    auto meta_stream = directory.create_output(meta_name, context);
    if (!meta_stream.has_value()) {
        return std::unexpected(std::move(meta_stream.error()));
    }
    if (auto header = CodecUtil::write_index_header(*meta_stream.value(), VECTORS_META_CODEC_NAME, VERSION_CURRENT,
                                                    segment_info.id, segment_suffix);
        !header.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        return std::unexpected(std::move(header.error()));
    }

    const std::string vectors_name = segment_file_name(segment_info.name, segment_suffix, VECTORS_EXTENSION);
    auto vectors_stream = directory.create_output(vectors_name, context);
    if (!vectors_stream.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        return std::unexpected(std::move(vectors_stream.error()));
    }
    if (auto header = CodecUtil::write_index_header(*vectors_stream.value(), format_name, VERSION_CURRENT,
                                                    segment_info.id, segment_suffix);
        !header.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(vectors_stream.value()->close());
        return std::unexpected(std::move(header.error()));
    }

    auto index_writer = Lucene90FieldsIndexWriter::create(directory, segment_info.name, segment_suffix,
                                                          VECTORS_INDEX_EXTENSION, VECTORS_INDEX_CODEC_NAME,
                                                          segment_info.id, block_shift, context);
    if (!index_writer.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(vectors_stream.value()->close());
        return std::unexpected(std::move(index_writer.error()));
    }

    // The packed-ints version and the chunk size go first: a reader needs both
    // before it can decode anything.
    if (auto written = meta_stream.value()->write_vint(PackedInts::VERSION_CURRENT); !written.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_writer.value()->close());
        return std::unexpected(std::move(written.error()));
    }
    if (auto written = meta_stream.value()->write_vint(chunk_size); !written.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_writer.value()->close());
        return std::unexpected(std::move(written.error()));
    }

    auto writer = std::unique_ptr<Lucene90CompressingTermVectorsWriter>(new Lucene90CompressingTermVectorsWriter(
            std::string(format_name), compression_mode, chunk_size, max_docs_per_chunk,
            std::move(meta_stream.value()), std::move(vectors_stream.value()), std::move(index_writer.value())));
    writer->writer_ = std::make_unique<BlockPackedWriter>(std::move(packed.value()));
    return writer;
}

// ---- chunk state ----

size_t Lucene90CompressingTermVectorsWriter::add_doc_data(int32_t num_vector_fields) {
    size_t pos_start = 0;
    size_t off_start = 0;
    size_t pay_start = 0;
    for (auto it = pending_docs_.rbegin(); it != pending_docs_.rend(); ++it) {
        if (!it->fields.empty()) {
            const FieldData &last = it->fields.back();
            pos_start = last.pos_start + (last.has_positions ? last.total_positions : 0);
            off_start = last.off_start + (last.has_offsets ? last.total_positions : 0);
            pay_start = last.pay_start + (last.has_payloads ? last.total_positions : 0);
            break;
        }
    }

    const size_t index = pending_docs_.size();
    pending_docs_.emplace_back(num_vector_fields, pos_start, off_start, pay_start);
    return index;
}

bool Lucene90CompressingTermVectorsWriter::trigger_flush() const noexcept {
    return term_suffixes_.size() >= static_cast<size_t>(chunk_size_) ||
           pending_docs_.size() >= static_cast<size_t>(max_docs_per_chunk_);
}

Result<Lucene90CompressingTermVectorsWriter::FieldData *> Lucene90CompressingTermVectorsWriter::current_field() {
    if (cur_doc_ >= pending_docs_.size()) {
        return err(LuceneError::illegal_state("No pending document for index " + std::to_string(cur_doc_)));
    }
    DocData &doc = pending_docs_[cur_doc_];
    if (cur_field_ >= doc.fields.size()) {
        return err(LuceneError::illegal_state("No field found at index " + std::to_string(cur_field_) +
                                              " for current doc " + std::to_string(cur_doc_)));
    }
    return &doc.fields[cur_field_];
}

// ---- flushing ----

VoidResult Lucene90CompressingTermVectorsWriter::flush(bool force) {
    if (trigger_flush() == force) {
        return err(LuceneError::illegal_state("flush called with the wrong force flag"));
    }
    if (pending_docs_.empty()) {
        return err(LuceneError::illegal_state("nothing to flush"));
    }
    const int32_t chunk_docs = static_cast<int32_t>(pending_docs_.size());
    ++num_chunks_;
    if (force) {
        // Incomplete: this flush was forced by `finish`.
        ++num_dirty_chunks_;
        num_dirty_docs_ += chunk_docs;
    }

    auto pointer = vectors_stream_->get_file_pointer();
    if (!pointer.has_value()) {
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = index_writer_->write_index(chunk_docs, pointer.value()); !written.has_value()) {
        return written;
    }

    const int32_t doc_base = num_docs_ - chunk_docs;
    if (auto written = vectors_stream_->write_vint(doc_base); !written.has_value()) {
        return written;
    }
    if (auto written = vectors_stream_->write_vint((chunk_docs << 1) | (force ? 1 : 0)); !written.has_value()) {
        return written;
    }

    auto total_fields = flush_num_fields(static_cast<size_t>(chunk_docs));
    if (!total_fields.has_value()) {
        return std::unexpected(std::move(total_fields.error()));
    }

    if (total_fields.value() > 0) {
        if (auto written = flush_field_nums(); !written.has_value()) {
            return written;
        }
        if (auto written = flush_fields(total_fields.value()); !written.has_value()) {
            return written;
        }
        if (auto written = flush_flags(total_fields.value()); !written.has_value()) {
            return written;
        }
        if (auto written = flush_num_terms(total_fields.value()); !written.has_value()) {
            return written;
        }
        if (auto written = flush_term_lengths(); !written.has_value()) {
            return written;
        }
        if (auto written = flush_term_freqs(); !written.has_value()) {
            return written;
        }
        if (auto written = flush_positions(); !written.has_value()) {
            return written;
        }
        if (auto written = flush_offsets(); !written.has_value()) {
            return written;
        }
        if (auto written = flush_payload_lengths(); !written.has_value()) {
            return written;
        }
        if (auto written = compressor_->compress(term_suffixes_.bytes(), *vectors_stream_); !written.has_value()) {
            return written;
        }
    }

    pending_docs_.clear();
    cur_doc_ = 0;
    cur_field_ = 0;
    term_suffixes_.reset();
    return {};
}

Result<int32_t> Lucene90CompressingTermVectorsWriter::flush_num_fields(size_t chunk_docs) {
    if (chunk_docs == 1) {
        const int32_t num_fields = pending_docs_.front().num_fields;
        if (auto written = vectors_stream_->write_vint(num_fields); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        return num_fields;
    }

    writer_->reset();
    int32_t total_fields = 0;
    for (const DocData &doc : pending_docs_) {
        if (auto written = writer_->add(doc.num_fields, *vectors_stream_); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        total_fields += doc.num_fields;
    }
    if (auto written = writer_->finish(*vectors_stream_); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return total_fields;
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_field_nums() {
    // 1. Collect the distinct field numbers, sorted.
    field_nums_.clear();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            const auto position = std::lower_bound(field_nums_.begin(), field_nums_.end(), field.field_num);
            if (position == field_nums_.end() || *position != field.field_num) {
                field_nums_.insert(position, field.field_num);
            }
        }
    }

    const size_t num_distinct_fields = field_nums_.size();
    if (num_distinct_fields == 0) {
        return err(LuceneError::illegal_state("a chunk must have at least one field"));
    }

    const int32_t max_field_num = field_nums_.back();
    auto bits = PackedInts::bits_required(max_field_num);
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    const int32_t bits_required = bits.value();

    const uint8_t token = static_cast<uint8_t>((std::min(num_distinct_fields - 1, size_t{0x07}) << 5) |
                                               static_cast<uint8_t>(bits_required));
    if (auto written = vectors_stream_->write_byte(token); !written.has_value()) {
        return written;
    }
    if (num_distinct_fields > 0x07) {
        if (auto written = vectors_stream_->write_vint(static_cast<int32_t>(num_distinct_fields - 1 - 0x07));
            !written.has_value()) {
            return written;
        }
    }

    std::vector<int64_t> numbers(field_nums_.begin(), field_nums_.end());
    return write_packed_stream(*vectors_stream_, std::span<const int64_t>(numbers), bits_required);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_fields(int32_t total_fields) {
    scratch_buffer_.reset();

    auto bits = DirectWriter::bits_required(static_cast<int64_t>(field_nums_.size()) - 1);
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    auto writer = DirectWriter::get_instance(scratch_buffer_, total_fields, bits.value());
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }

    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            const auto position = std::lower_bound(field_nums_.begin(), field_nums_.end(), field.field_num);
            if (position == field_nums_.end() || *position != field.field_num) {
                return err(LuceneError::illegal_state("Field number " + std::to_string(field.field_num) +
                                                      " not found in field_nums"));
            }
            if (auto added = writer.value().add(static_cast<int64_t>(position - field_nums_.begin()));
                !added.has_value()) {
                return added;
            }
        }
    }
    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }

    if (auto written = vectors_stream_->write_vlong(static_cast<int64_t>(scratch_buffer_.size()));
        !written.has_value()) {
        return written;
    }
    return scratch_buffer_.copy_to(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_flags(int32_t total_fields) {
    // Are the flags the same for every instance of a field?
    bool non_changing_flags = true;
    std::vector<int32_t> field_flags(field_nums_.size(), -1);

    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            const auto position = std::lower_bound(field_nums_.begin(), field_nums_.end(), field.field_num);
            if (position == field_nums_.end() || *position != field.field_num) {
                return err(LuceneError::illegal_state("field_num " + std::to_string(field.field_num) +
                                                      " not found in field_nums"));
            }
            const size_t index = static_cast<size_t>(position - field_nums_.begin());
            if (field_flags[index] == -1) {
                field_flags[index] = field.flags;
            } else if (field_flags[index] != field.flags) {
                non_changing_flags = false;
                break;
            }
        }
        if (!non_changing_flags) {
            break;
        }
    }

    scratch_buffer_.reset();

    if (non_changing_flags) {
        // One flag per distinct field number.
        if (auto written = vectors_stream_->write_vint(0); !written.has_value()) {
            return written;
        }
        auto writer = DirectWriter::get_instance(scratch_buffer_, static_cast<int64_t>(field_flags.size()),
                                                 flags_bits());
        if (!writer.has_value()) {
            return std::unexpected(std::move(writer.error()));
        }
        for (const int32_t flags : field_flags) {
            if (auto added = writer.value().add(flags); !added.has_value()) {
                return added;
            }
        }
        if (auto finished = writer.value().finish(); !finished.has_value()) {
            return finished;
        }
        if (auto written = vectors_stream_->write_vint(static_cast<int32_t>(scratch_buffer_.size()));
            !written.has_value()) {
            return written;
        }
        return scratch_buffer_.copy_to(*vectors_stream_);
    }

    // One flag for every field instance.
    if (auto written = vectors_stream_->write_vint(1); !written.has_value()) {
        return written;
    }
    auto writer = DirectWriter::get_instance(scratch_buffer_, total_fields, flags_bits());
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if (auto added = writer.value().add(field.flags); !added.has_value()) {
                return added;
            }
        }
    }
    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }
    if (auto written = vectors_stream_->write_vint(static_cast<int32_t>(scratch_buffer_.size()));
        !written.has_value()) {
        return written;
    }
    return scratch_buffer_.copy_to(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_num_terms(int32_t total_fields) {
    int32_t max_num_terms = 0;
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            max_num_terms |= static_cast<int32_t>(field.num_terms);
        }
    }

    auto bits = DirectWriter::bits_required(max_num_terms);
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    if (auto written = vectors_stream_->write_vint(bits.value()); !written.has_value()) {
        return written;
    }

    scratch_buffer_.reset();
    auto writer = DirectWriter::get_instance(scratch_buffer_, total_fields, bits.value());
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if (auto added = writer.value().add(static_cast<int64_t>(field.num_terms)); !added.has_value()) {
                return added;
            }
        }
    }
    if (auto finished = writer.value().finish(); !finished.has_value()) {
        return finished;
    }
    if (auto written = vectors_stream_->write_vint(static_cast<int32_t>(scratch_buffer_.size()));
        !written.has_value()) {
        return written;
    }
    return scratch_buffer_.copy_to(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_term_lengths() {
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            for (size_t i = 0; i < field.num_terms; ++i) {
                if (auto written = writer_->add(field.prefix_lengths[i], *vectors_stream_); !written.has_value()) {
                    return written;
                }
            }
        }
    }
    if (auto written = writer_->finish(*vectors_stream_); !written.has_value()) {
        return written;
    }

    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            for (size_t i = 0; i < field.num_terms; ++i) {
                if (auto written = writer_->add(field.suffix_lengths[i], *vectors_stream_); !written.has_value()) {
                    return written;
                }
            }
        }
    }
    return writer_->finish(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_term_freqs() {
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            for (size_t i = 0; i < field.num_terms; ++i) {
                if (auto written = writer_->add(static_cast<int64_t>(field.freqs[i]) - 1, *vectors_stream_);
                    !written.has_value()) {
                    return written;
                }
            }
        }
    }
    return writer_->finish(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_positions() {
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if (!field.has_positions) {
                continue;
            }
            size_t pos = 0;
            for (size_t i = 0; i < field.num_terms; ++i) {
                const int32_t freq = field.freqs[i];
                int32_t previous_position = 0;
                for (int32_t f = 0; f < freq; ++f) {
                    const int32_t position = positions_buf_[field.pos_start + pos];
                    if (auto written = writer_->add(static_cast<int64_t>(position) - previous_position,
                                                    *vectors_stream_);
                        !written.has_value()) {
                        return written;
                    }
                    previous_position = position;
                    ++pos;
                }
            }
        }
    }
    return writer_->finish(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_offsets() {
    bool has_offsets = false;
    std::vector<uint64_t> sum_pos(field_nums_.size(), 0);
    std::vector<uint64_t> sum_offsets(field_nums_.size(), 0);

    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            has_offsets = has_offsets || field.has_offsets;
            if (!field.has_offsets || !field.has_positions) {
                continue;
            }
            const auto position = std::lower_bound(field_nums_.begin(), field_nums_.end(), field.field_num);
            if (position == field_nums_.end() || *position != field.field_num) {
                return err(LuceneError::illegal_state("field_num " + std::to_string(field.field_num) +
                                                      " not found in field_nums"));
            }
            const size_t field_index = static_cast<size_t>(position - field_nums_.begin());
            size_t pos = 0;
            for (size_t i = 0; i < field.num_terms; ++i) {
                const size_t freq = static_cast<size_t>(field.freqs[i]);
                const size_t position_index = field.pos_start + pos + freq - 1;
                const size_t offset_index = field.off_start + pos + freq - 1;
                sum_pos[field_index] += static_cast<uint64_t>(positions_buf_[position_index]);
                sum_offsets[field_index] += static_cast<uint64_t>(start_offsets_buf_[offset_index]);
                pos += freq;
            }
        }
    }

    if (!has_offsets) {
        return {};
    }

    std::vector<float> chars_per_term(field_nums_.size(), 0.0F);
    for (size_t i = 0; i < field_nums_.size(); ++i) {
        chars_per_term[i] = (sum_pos[i] == 0 || sum_offsets[i] == 0)
                                    ? 0.0F
                                    : static_cast<float>(static_cast<double>(sum_offsets[i]) /
                                                         static_cast<double>(sum_pos[i]));
        if (auto written = vectors_stream_->write_int(BitUtil::float_to_int_bits(chars_per_term[i]));
            !written.has_value()) {
            return written;
        }
    }

    // Start offsets.
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if ((field.flags & TV_OFFSETS) == 0) {
                continue;
            }
            const auto position = std::lower_bound(field_nums_.begin(), field_nums_.end(), field.field_num);
            if (position == field_nums_.end() || *position != field.field_num) {
                return err(LuceneError::illegal_state("field_num " + std::to_string(field.field_num) + " not found"));
            }
            const float chars_per_term_field = chars_per_term[static_cast<size_t>(position - field_nums_.begin())];
            // Hoisted out of the per-position loop: the mode is a property of the
            // field, and this is the innermost loop of the term vector writer.
            const bool has_positions = field.has_positions;
            size_t pos = 0;
            for (size_t i = 0; i < field.num_terms; ++i) {
                const int32_t freq = field.freqs[i];
                int32_t previous_pos = 0;
                int32_t previous_off = 0;
                for (int32_t f = 0; f < freq; ++f) {
                    const int32_t current_position = has_positions ? positions_buf_[field.pos_start + pos] : 0;
                    const int32_t start_offset = start_offsets_buf_[field.off_start + pos];
                    const int32_t predicted =
                            static_cast<int32_t>(chars_per_term_field *
                                                 static_cast<float>(current_position - previous_pos));
                    if (auto written = writer_->add(static_cast<int64_t>(start_offset) - previous_off - predicted,
                                                    *vectors_stream_);
                        !written.has_value()) {
                        return written;
                    }
                    previous_pos = current_position;
                    previous_off = start_offset;
                    ++pos;
                }
            }
        }
    }
    if (auto written = writer_->finish(*vectors_stream_); !written.has_value()) {
        return written;
    }

    // Lengths.
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if ((field.flags & TV_OFFSETS) == 0) {
                continue;
            }
            size_t pos = 0;
            for (size_t i = 0; i < field.num_terms; ++i) {
                const int32_t prefix = field.prefix_lengths[i];
                const int32_t suffix = field.suffix_lengths[i];
                for (int32_t f = 0; f < field.freqs[i]; ++f) {
                    const int32_t length = lengths_buf_[field.off_start + pos];
                    if (auto written = writer_->add(static_cast<int64_t>(length) - prefix - suffix,
                                                    *vectors_stream_);
                        !written.has_value()) {
                        return written;
                    }
                    ++pos;
                }
            }
        }
    }
    return writer_->finish(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::flush_payload_lengths() {
    writer_->reset();
    for (const DocData &doc : pending_docs_) {
        for (const FieldData &field : doc.fields) {
            if (!field.has_payloads) {
                continue;
            }
            for (size_t i = 0; i < field.total_positions; ++i) {
                if (auto written = writer_->add(payload_lengths_buf_[field.pay_start + i], *vectors_stream_);
                    !written.has_value()) {
                    return written;
                }
            }
        }
    }
    return writer_->finish(*vectors_stream_);
}

// ---- TermVectorsWriter ----

VoidResult Lucene90CompressingTermVectorsWriter::start_document(int32_t num_vector_fields) {
    cur_doc_ = add_doc_data(num_vector_fields);
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::finish_document() {
    // The payloads of a document follow its term suffixes.
    if (auto written = payload_bytes_.copy_to(term_suffixes_); !written.has_value()) {
        return written;
    }
    payload_bytes_.reset();
    ++num_docs_;

    if (trigger_flush()) {
        return flush(false);
    }
    cur_doc_ = 0;
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::start_field(const FieldInfo &field_info, int32_t num_terms,
                                                             bool positions, bool offsets, bool payloads) {
    if (num_terms < 0) {
        return err(LuceneError::illegal_argument("num_terms must be non-negative"));
    }
    if (cur_doc_ >= pending_docs_.size()) {
        return err(LuceneError::illegal_state("start_field called without a document"));
    }
    const size_t num_terms_size = static_cast<size_t>(num_terms);
    cur_field_ = pending_docs_[cur_doc_].add_field(field_info.number, num_terms_size, positions, offsets, payloads);

    // Reserve the arrays the positions of this field will be written to.
    const DocData &doc = pending_docs_[cur_doc_];
    const FieldData &field = doc.fields.back();
    if (positions) {
        ensure_size(positions_buf_, field.pos_start + num_terms_size);
    }
    if (offsets) {
        ensure_size(start_offsets_buf_, field.off_start + num_terms_size);
        ensure_size(lengths_buf_, field.off_start + num_terms_size);
    }
    if (payloads) {
        ensure_size(payload_lengths_buf_, field.pay_start + num_terms_size);
    }
    last_term_.set_length(0);
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::finish_field() {
    cur_field_ = 0;
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::start_term(std::span<const uint8_t> term, int32_t freq) {
    if (freq < 1) {
        return err(LuceneError::illegal_argument("freq must be >= 1"));
    }
    size_t prefix = 0;
    if (last_term_.length() != 0) {
        auto difference = StringHelper::bytes_difference(last_term_.as_slice(), term);
        if (!difference.has_value()) {
            return std::unexpected(std::move(difference.error()));
        }
        prefix = difference.value();
    }
    const size_t suffix_length = term.size() - prefix;

    auto field = current_field();
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    field.value()->add_term(freq, static_cast<int32_t>(prefix), static_cast<int32_t>(suffix_length));

    if (auto written = term_suffixes_.write_bytes(term, prefix, suffix_length); !written.has_value()) {
        return written;
    }

    // Remember the term for the next prefix computation.
    last_term_.bytes().resize(std::max(last_term_.bytes().size(), term.size()));
    last_term_.set_offset(0);
    last_term_.set_length(term.size());
    std::copy(term.begin(), term.end(), last_term_.bytes().begin());
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::add_position(int32_t position, int32_t start_offset,
                                                             int32_t end_offset,
                                                             std::optional<std::span<const uint8_t>> payload) {
    auto field = current_field();
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    FieldData &data = *field.value();
    const int32_t length = end_offset - start_offset;
    const int32_t payload_length = payload.has_value() ? static_cast<int32_t>(payload->size()) : 0;

    if (data.has_positions) {
        ensure_size(positions_buf_, data.pos_start + data.total_positions + 1);
    }
    if (data.has_offsets) {
        ensure_size(start_offsets_buf_, data.off_start + data.total_positions + 1);
        ensure_size(lengths_buf_, data.off_start + data.total_positions + 1);
    }
    if (data.has_payloads) {
        ensure_size(payload_lengths_buf_, data.pay_start + data.total_positions + 1);
    }
    if (auto added = data.add_position(position, start_offset, length, payload_length, positions_buf_,
                                       start_offsets_buf_, lengths_buf_, payload_lengths_buf_);
        !added.has_value()) {
        return added;
    }

    if (data.has_payloads && payload.has_value()) {
        return payload_bytes_.write_bytes(*payload, 0, payload->size());
    }
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::add_prox(size_t num_prox, DataInput *positions, DataInput *offsets) {
    auto field = current_field();
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    FieldData &data = *field.value();

    if (data.has_positions) {
        if (positions == nullptr) {
            return err(LuceneError::illegal_state("positions were requested but not provided"));
        }
        const size_t pos_start = data.pos_start + data.total_positions;
        ensure_size(positions_buf_, pos_start + num_prox);
        if (data.has_payloads) {
            const size_t pay_start = data.pay_start + data.total_positions;
            ensure_size(payload_lengths_buf_, pay_start + num_prox);
            int32_t position = 0;
            for (size_t i = 0; i < num_prox; ++i) {
                auto code = positions->read_vint();
                if (!code.has_value()) {
                    return std::unexpected(std::move(code.error()));
                }
                if ((code.value() & 1) != 0) {
                    // This position has a payload.
                    auto payload_length = positions->read_vint();
                    if (!payload_length.has_value()) {
                        return std::unexpected(std::move(payload_length.error()));
                    }
                    if (payload_length.value() < 0) {
                        return err(LuceneError::corrupt_index("negative payload length"));
                    }
                    payload_lengths_buf_[pay_start + i] = payload_length.value();
                    if (auto copied = payload_bytes_.copy_bytes(*positions,
                                                                static_cast<size_t>(payload_length.value()));
                        !copied.has_value()) {
                        return copied;
                    }
                } else {
                    payload_lengths_buf_[pay_start + i] = 0;
                }
                const int32_t delta = static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
                position += delta;
                positions_buf_[pos_start + i] = position;
            }
        } else {
            int32_t position = 0;
            for (size_t i = 0; i < num_prox; ++i) {
                auto code = positions->read_vint();
                if (!code.has_value()) {
                    return std::unexpected(std::move(code.error()));
                }
                position += static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
                positions_buf_[pos_start + i] = position;
            }
        }
    }

    if (data.has_offsets) {
        if (offsets == nullptr) {
            return err(LuceneError::illegal_state("offsets were requested but not provided"));
        }
        const size_t off_start = data.off_start + data.total_positions;
        ensure_size(start_offsets_buf_, off_start + num_prox);
        ensure_size(lengths_buf_, off_start + num_prox);
        int32_t last_offset = 0;
        for (size_t i = 0; i < num_prox; ++i) {
            auto start_delta = offsets->read_vint();
            if (!start_delta.has_value()) {
                return std::unexpected(std::move(start_delta.error()));
            }
            auto length = offsets->read_vint();
            if (!length.has_value()) {
                return std::unexpected(std::move(length.error()));
            }
            const int32_t start_offset = last_offset + start_delta.value();
            const int32_t end_offset = start_offset + length.value();
            last_offset = end_offset;
            start_offsets_buf_[off_start + i] = start_offset;
            lengths_buf_[off_start + i] = end_offset - start_offset;
        }
    }

    data.total_positions += num_prox;
    return {};
}

VoidResult Lucene90CompressingTermVectorsWriter::finish(int32_t num_docs) {
    if (!pending_docs_.empty()) {
        if (auto written = flush(true); !written.has_value()) {
            return written;
        }
    }

    if (num_docs != num_docs_) {
        return err(LuceneError::illegal_state("Wrote " + std::to_string(num_docs_) +
                                              " docs, finish called with numDocs=" + std::to_string(num_docs)));
    }

    auto pointer = vectors_stream_->get_file_pointer();
    if (!pointer.has_value()) {
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = index_writer_->finish(num_docs, pointer.value(), *meta_stream_); !written.has_value()) {
        return written;
    }

    if (auto written = meta_stream_->write_vlong(num_chunks_); !written.has_value()) {
        return written;
    }
    if (auto written = meta_stream_->write_vlong(num_dirty_chunks_); !written.has_value()) {
        return written;
    }
    if (auto written = meta_stream_->write_vlong(num_dirty_docs_); !written.has_value()) {
        return written;
    }
    if (auto written = CodecUtil::write_footer(*meta_stream_); !written.has_value()) {
        return written;
    }
    return CodecUtil::write_footer(*vectors_stream_);
}

VoidResult Lucene90CompressingTermVectorsWriter::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    VoidResult result;
    if (meta_stream_ != nullptr) {
        result = meta_stream_->close();
    }
    if (vectors_stream_ != nullptr) {
        if (auto closed = vectors_stream_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
    }
    if (index_writer_ != nullptr) {
        if (auto closed = index_writer_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
    }
    return result;
}

} // namespace lucene
