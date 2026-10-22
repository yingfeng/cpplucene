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

module lucene_core:lucene90_compressing_stored_fields_writer.impl;

import :lucene90_compressing_stored_fields_writer;

import :byte_array_output;
import :codec_util;
import :compression;
import :data_output;
import :directory;
import :error;
import :fields_index;
import :index_output;
import :io_context;
import :lucene90_compressing_stored_fields_format;
import :lucene90_fields_index_writer;
import :segment_info;
import :stored_fields_ints;
import :stored_fields_writer;
import :types;

import std;

namespace lucene {

Lucene90CompressingStoredFieldsWriter::Lucene90CompressingStoredFieldsWriter(
        Directory &directory, std::string format_name, CompressionMode compression_mode, int32_t chunk_size,
        int32_t max_docs_per_chunk, std::unique_ptr<IndexOutput> meta_stream,
        std::unique_ptr<IndexOutput> fields_stream, std::unique_ptr<Lucene90FieldsIndexWriter> index_writer)
    : directory_(directory),
      format_name_(std::move(format_name)),
      compression_mode_(compression_mode),
      compressor_(compression_mode.new_compressor()),
      chunk_size_(chunk_size),
      max_docs_per_chunk_(max_docs_per_chunk),
      meta_stream_(std::move(meta_stream)),
      fields_stream_(std::move(fields_stream)),
      index_writer_(std::move(index_writer)),
      buffered_docs_("stored-fields-chunk") {}

Lucene90CompressingStoredFieldsWriter::~Lucene90CompressingStoredFieldsWriter() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90CompressingStoredFieldsWriter>> Lucene90CompressingStoredFieldsWriter::create(
        Directory &directory, const SegmentInfo &segment_info, const IOContext &context,
        std::string_view format_name, const CompressionMode &compression_mode, int32_t chunk_size,
        int32_t max_docs_per_chunk, int32_t block_shift) {
    const std::string meta_name = segment_file_name(segment_info.name, "", STORED_FIELDS_META_EXTENSION);
    auto meta_stream = directory.create_output(meta_name, context);
    if (!meta_stream.has_value()) {
        return std::unexpected(std::move(meta_stream.error()));
    }
    auto meta_header = CodecUtil::write_index_header(*meta_stream.value(), STORED_FIELDS_META_CODEC_NAME,
                                                     STORED_FIELDS_VERSION_CURRENT, segment_info.id, "");
    if (!meta_header.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        return std::unexpected(std::move(meta_header.error()));
    }

    const std::string fields_name = segment_file_name(segment_info.name, "", STORED_FIELDS_FIELDS_EXTENSION);
    auto fields_stream = directory.create_output(fields_name, context);
    if (!fields_stream.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        return std::unexpected(std::move(fields_stream.error()));
    }
    auto fields_header = CodecUtil::write_index_header(*fields_stream.value(), format_name,
                                                       STORED_FIELDS_VERSION_CURRENT, segment_info.id, "");
    if (!fields_header.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(fields_stream.value()->close());
        return std::unexpected(std::move(fields_header.error()));
    }

    auto index_writer = Lucene90FieldsIndexWriter::create(directory, segment_info.name, "",
                                                          STORED_FIELDS_INDEX_EXTENSION,
                                                          STORED_FIELDS_INDEX_CODEC_NAME, segment_info.id,
                                                          block_shift, context);
    if (!index_writer.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(fields_stream.value()->close());
        return std::unexpected(std::move(index_writer.error()));
    }

    // The chunk size goes first: a reader needs it before it can decompress.
    if (auto written = meta_stream.value()->write_vint(chunk_size); !written.has_value()) {
        static_cast<void>(meta_stream.value()->close());
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_writer.value()->close());
        return std::unexpected(std::move(written.error()));
    }

    std::unique_ptr<Lucene90CompressingStoredFieldsWriter> writer(
            new Lucene90CompressingStoredFieldsWriter(
                    directory, std::string(format_name), compression_mode, chunk_size, max_docs_per_chunk,
                    std::move(meta_stream.value()), std::move(fields_stream.value()),
                    std::move(index_writer.value())));
    return writer;
}

VoidResult Lucene90CompressingStoredFieldsWriter::save_ints(const std::vector<int32_t> &values, size_t count,
                                                            DataOutput &out) {
    if (count == 1) {
        return out.write_vint(values[0]);
    }
    return StoredFieldsInts::write_ints(values, 0, count, out);
}

bool Lucene90CompressingStoredFieldsWriter::trigger_flush() const noexcept {
    return static_cast<int32_t>(buffered_docs_.size()) >= chunk_size_ ||
           static_cast<int32_t>(num_buffered_docs_) >= max_docs_per_chunk_;
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_header(bool sliced, bool dirty_chunk) {
    const int32_t sliced_bit = sliced ? 1 : 0;
    const int32_t dirty_bit = dirty_chunk ? 2 : 0;

    if (auto written = fields_stream_->write_vint(static_cast<int32_t>(doc_base_)); !written.has_value()) {
        return written;
    }
    if (auto written = fields_stream_->write_vint(
                (static_cast<int32_t>(num_buffered_docs_) << 2) | dirty_bit | sliced_bit);
        !written.has_value()) {
        return written;
    }
    if (auto written = save_ints(num_stored_fields_, num_buffered_docs_, *fields_stream_); !written.has_value()) {
        return written;
    }
    return save_ints(end_offsets_, num_buffered_docs_, *fields_stream_);
}

VoidResult Lucene90CompressingStoredFieldsWriter::flush(bool force) {
    if (trigger_flush() == force) {
        return err(LuceneError::illegal_state("flush called with the wrong force flag"));
    }

    ++num_chunks_;
    if (force) {
        ++num_dirty_chunks_;
        num_dirty_docs_ += static_cast<int64_t>(num_buffered_docs_);
    }

    auto pointer = fields_stream_->get_file_pointer();
    if (!pointer.has_value()) {
        return std::unexpected(std::move(pointer.error()));
    }
    if (auto written = index_writer_->write_index(static_cast<int32_t>(num_buffered_docs_), pointer.value());
        !written.has_value()) {
        return written;
    }

    // The lengths are stored cumulatively; the header wants the deltas.
    for (size_t i = num_buffered_docs_; i > 1; --i) {
        end_offsets_[i - 1] -= end_offsets_[i - 2];
    }

    const bool sliced = buffered_docs_.size() >= 2 * static_cast<size_t>(chunk_size_);
    if (auto written = write_header(sliced, force); !written.has_value()) {
        return written;
    }

    const auto bytes = buffered_docs_.bytes();
    if (sliced) {
        // Every `chunk_size` bytes form one compressed block; the reader
        // decompresses them one after the other.
        size_t compressed = 0;
        while (compressed < bytes.size()) {
            const size_t len = std::min(static_cast<size_t>(chunk_size_), bytes.size() - compressed);
            if (auto written = compressor_->compress(bytes.subspan(compressed, len), *fields_stream_);
                !written.has_value()) {
                return written;
            }
            compressed += len;
        }
    } else if (auto written = compressor_->compress(bytes, *fields_stream_); !written.has_value()) {
        return written;
    }

    doc_base_ += num_buffered_docs_;
    num_buffered_docs_ = 0;
    num_stored_fields_.clear();
    end_offsets_.clear();
    buffered_docs_.reset();
    return {};
}

VoidResult Lucene90CompressingStoredFieldsWriter::start_document() { return {}; }

VoidResult Lucene90CompressingStoredFieldsWriter::finish_document() {
    const auto end_offset = static_cast<int32_t>(buffered_docs_.size());
    num_stored_fields_.push_back(num_stored_fields_in_doc_);
    num_stored_fields_in_doc_ = 0;
    end_offsets_.push_back(end_offset);
    ++num_buffered_docs_;

    if (trigger_flush()) {
        return flush(false);
    }
    return {};
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_i32(const FieldInfo &field_info, int32_t value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_NUMERIC_INT;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    return buffered_docs_.write_zint(value);
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_i64(const FieldInfo &field_info, int64_t value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_NUMERIC_LONG;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    return write_tlong(buffered_docs_, value);
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_f32(const FieldInfo &field_info, float value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_NUMERIC_FLOAT;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    return write_zfloat(buffered_docs_, value);
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_f64(const FieldInfo &field_info, double value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_NUMERIC_DOUBLE;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    return write_zdouble(buffered_docs_, value);
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_bytes(const FieldInfo &field_info,
                                                                   std::span<const uint8_t> value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_BYTE_ARR;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    if (auto written = buffered_docs_.write_vint(static_cast<int32_t>(value.size())); !written.has_value()) {
        return written;
    }
    return buffered_docs_.write_bytes(value, 0, value.size());
}

VoidResult Lucene90CompressingStoredFieldsWriter::write_field_str(const FieldInfo &field_info,
                                                                 std::string_view value) {
    ++num_stored_fields_in_doc_;
    const int64_t info_and_bits = (static_cast<int64_t>(field_info.number) << SF_TYPE_BITS) | SF_STRING;
    if (auto written = buffered_docs_.write_vlong(info_and_bits); !written.has_value()) {
        return written;
    }
    return buffered_docs_.write_string(value);
}

VoidResult Lucene90CompressingStoredFieldsWriter::finish(int32_t num_docs) {
    if (num_buffered_docs_ > 0) {
        if (auto flushed = flush(true); !flushed.has_value()) {
            return flushed;
        }
    }
    if (doc_base_ != static_cast<size_t>(num_docs)) {
        return err(LuceneError::illegal_state("wrote " + std::to_string(doc_base_) + " docs, but finish was "
                                              "called with num_docs=" + std::to_string(num_docs)));
    }

    auto pointer = fields_stream_->get_file_pointer();
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
    return CodecUtil::write_footer(*fields_stream_);
}

VoidResult Lucene90CompressingStoredFieldsWriter::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    // Every stream is closed even if an earlier one fails, so that no file
    // descriptor leaks; the first error is reported.
    VoidResult result{};
    if (meta_stream_ != nullptr) {
        result = meta_stream_->close();
    }
    if (fields_stream_ != nullptr) {
        auto closed = fields_stream_->close();
        if (result.has_value()) {
            result = closed;
        }
    }
    if (index_writer_ != nullptr) {
        auto closed = index_writer_->close();
        if (result.has_value()) {
            result = closed;
        }
    }
    if (compressor_ != nullptr) {
        auto closed = compressor_->close();
        if (result.has_value()) {
            result = closed;
        }
    }
    return result;
}

} // namespace lucene
