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

module lucene_core:lucene90_fields_index_reader.impl;

import :lucene90_fields_index_reader;

import :codec_util;
import :core_helper;
import :data_input;
import :directory;
import :direct_monotonic_reader;
import :error;
import :fields_index;
import :index_input;
import :io_context;
import :random_access_input;
import :segment_info;
import :types;

import std;

namespace lucene {

namespace {

constexpr int32_t VERSION_START = 0;
/// Only the current version is written and read, by design.
constexpr int32_t VERSION_CURRENT = VERSION_START;

} // namespace

Lucene90FieldsIndexReader::Lucene90FieldsIndexReader(int32_t max_doc, int32_t block_shift, int32_t num_chunks,
                                                    DirectMonotonicMeta docs_meta,
                                                    DirectMonotonicMeta start_pointers_meta,
                                                    std::unique_ptr<IndexInput> index_input,
                                                    size_t docs_start_pointer, size_t docs_end_pointer,
                                                    size_t start_pointers_start_pointer,
                                                    size_t start_pointers_end_pointer, size_t max_pointer)
    : max_doc_(max_doc),
      block_shift_(block_shift),
      num_chunks_(num_chunks),
      docs_meta_(std::move(docs_meta)),
      start_pointers_meta_(std::move(start_pointers_meta)),
      index_input_(std::move(index_input)),
      docs_start_pointer_(docs_start_pointer),
      docs_end_pointer_(docs_end_pointer),
      start_pointers_start_pointer_(start_pointers_start_pointer),
      start_pointers_end_pointer_(start_pointers_end_pointer),
      max_pointer_(max_pointer) {}

Lucene90FieldsIndexReader::~Lucene90FieldsIndexReader() = default;

VoidResult Lucene90FieldsIndexReader::open_readers() {
    auto docs_slice = index_input_->random_access_slice(docs_start_pointer_, docs_end_pointer_ - docs_start_pointer_);
    if (!docs_slice.has_value()) {
        return std::unexpected(std::move(docs_slice.error()));
    }
    docs_slice_ = std::move(docs_slice.value());

    auto pointers_slice = index_input_->random_access_slice(start_pointers_start_pointer_,
                                                            start_pointers_end_pointer_ -
                                                                    start_pointers_start_pointer_);
    if (!pointers_slice.has_value()) {
        return std::unexpected(std::move(pointers_slice.error()));
    }
    start_pointers_slice_ = std::move(pointers_slice.value());

    docs_.emplace(docs_meta_, *docs_slice_);
    start_pointers_.emplace(start_pointers_meta_, *start_pointers_slice_);
    return {};
}

Result<std::unique_ptr<Lucene90FieldsIndexReader>> Lucene90FieldsIndexReader::create(
        const Directory &dir, std::string_view name, std::string_view suffix, std::string_view extension,
        std::string_view codec_name, std::span<const uint8_t> id, DataInput &meta_in, const IOContext &context) {
    auto max_doc = meta_in.read_int();
    auto block_shift = meta_in.read_int();
    auto num_chunks = meta_in.read_int();
    auto docs_start_pointer = meta_in.read_long();
    if (!max_doc.has_value() || !block_shift.has_value() || !num_chunks.has_value() ||
        !docs_start_pointer.has_value()) {
        return err(LuceneError::corrupt_index("a fields index metadata entry is truncated"));
    }
    if (docs_start_pointer.value() < 0) {
        return err(LuceneError::corrupt_index("a fields index pointer is negative"));
    }

    auto docs_meta = DirectMonotonicMeta::load(meta_in, num_chunks.value(), block_shift.value());
    if (!docs_meta.has_value()) {
        return std::unexpected(std::move(docs_meta.error()));
    }
    auto docs_end_pointer = meta_in.read_long();
    if (!docs_end_pointer.has_value() || docs_end_pointer.value() < 0) {
        return err(LuceneError::corrupt_index("a fields index pointer is invalid"));
    }
    auto start_pointers_meta = DirectMonotonicMeta::load(meta_in, num_chunks.value(), block_shift.value());
    if (!start_pointers_meta.has_value()) {
        return std::unexpected(std::move(start_pointers_meta.error()));
    }
    auto start_pointers_end_pointer = meta_in.read_long();
    auto max_pointer = meta_in.read_long();
    if (!start_pointers_end_pointer.has_value() || !max_pointer.has_value() ||
        start_pointers_end_pointer.value() < 0 || max_pointer.value() < 0) {
        return err(LuceneError::corrupt_index("a fields index pointer is invalid"));
    }

    auto index_input = dir.open_input(segment_file_name(name, suffix, extension), context);
    if (!index_input.has_value()) {
        return std::unexpected(std::move(index_input.error()));
    }
    auto header = CodecUtil::check_index_header(*index_input.value(), std::string(codec_name) + "Idx",
                                                VERSION_START, VERSION_CURRENT, id, suffix);
    if (!header.has_value()) {
        static_cast<void>(index_input.value()->close());
        return std::unexpected(std::move(header.error()));
    }
    auto checksum = CodecUtil::retrieve_checksum(*index_input.value());
    if (!checksum.has_value()) {
        static_cast<void>(index_input.value()->close());
        return std::unexpected(std::move(checksum.error()));
    }

    std::unique_ptr<Lucene90FieldsIndexReader> reader(new Lucene90FieldsIndexReader(
            max_doc.value(), block_shift.value(), num_chunks.value(), std::move(docs_meta.value()),
            std::move(start_pointers_meta.value()), std::move(index_input.value()),
            static_cast<size_t>(docs_start_pointer.value()), static_cast<size_t>(docs_end_pointer.value()),
            static_cast<size_t>(docs_end_pointer.value()), static_cast<size_t>(start_pointers_end_pointer.value()),
            static_cast<size_t>(max_pointer.value())));
    if (auto opened = reader->open_readers(); !opened.has_value()) {
        static_cast<void>(reader->close());
        return std::unexpected(std::move(opened.error()));
    }
    return reader;
}

Result<int64_t> Lucene90FieldsIndexReader::get_block_id(int32_t doc_id) const {
    if (auto checked = CoreHelper::check_index(static_cast<size_t>(doc_id), static_cast<size_t>(max_doc_));
        !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    auto block = docs_->binary_search(0, num_chunks_, static_cast<int64_t>(doc_id));
    if (!block.has_value()) {
        return std::unexpected(std::move(block.error()));
    }
    // A negative result encodes the insertion point as `-2 - insertionPoint`.
    return block.value() < 0 ? -(2 + block.value()) : block.value();
}

Result<size_t> Lucene90FieldsIndexReader::get_block_start_pointer(int64_t block_id) const {
    auto pointer = start_pointers_->get(static_cast<size_t>(block_id));
    if (!pointer.has_value()) {
        return std::unexpected(std::move(pointer.error()));
    }
    return static_cast<size_t>(pointer.value());
}

Result<size_t> Lucene90FieldsIndexReader::get_block_length(int64_t block_id) const {
    size_t end_pointer = max_pointer_;
    if (block_id != static_cast<int64_t>(num_chunks_) - 1) {
        auto next = start_pointers_->get(static_cast<size_t>(block_id + 1));
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        end_pointer = static_cast<size_t>(next.value());
    }
    auto start = get_block_start_pointer(block_id);
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }
    if (end_pointer < start.value()) {
        return err(LuceneError::corrupt_index("a fields index chunk length is negative"));
    }
    return end_pointer - start.value();
}

VoidResult Lucene90FieldsIndexReader::check_integrity() const {
    auto checksum = CodecUtil::checksum_entire_file(*index_input_);
    if (!checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

VoidResult Lucene90FieldsIndexReader::close() const {
    if (index_input_ == nullptr) {
        return {};
    }
    auto closed = index_input_->close();
    index_input_.reset();
    return closed;
}

Result<std::unique_ptr<Lucene90FieldsIndexReader>> Lucene90FieldsIndexReader::clone_reader() const {
    auto input = index_input_->clone();
    if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
    }
    std::unique_ptr<Lucene90FieldsIndexReader> reader(new Lucene90FieldsIndexReader(
            max_doc_, block_shift_, num_chunks_, docs_meta_, start_pointers_meta_, std::move(input.value()),
            docs_start_pointer_, docs_end_pointer_, start_pointers_start_pointer_, start_pointers_end_pointer_,
            max_pointer_));
    if (auto opened = reader->open_readers(); !opened.has_value()) {
        static_cast<void>(reader->close());
        return std::unexpected(std::move(opened.error()));
    }
    return reader;
}

Result<std::unique_ptr<FieldsIndex>> Lucene90FieldsIndexReader::clone() const {
    auto reader = clone_reader();
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<FieldsIndex>(std::move(reader.value()));
}

} // namespace lucene
