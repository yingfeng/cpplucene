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

module lucene_core:lucene90_compressing_stored_fields_reader.impl;

import :lucene90_compressing_stored_fields_reader;

import :codec_util;
import :compression;
import :data_input;
import :directory;
import :document;
import :error;
import :indexable_field;
import :field_infos;
import :index_input;
import :io_context;
import :lucene90_compressing_stored_fields_format;
import :lucene90_fields_index_reader;
import :segment_info;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_ints;
import :stored_fields_reader;
import :types;

import std;

namespace lucene {

namespace {

/// Number of recently prefetched chunks to remember; a small number is enough
/// because index sorting and clustering put similar documents next to each other.
constexpr size_t PREFETCH_CACHE_SIZE = 1 << 4;
constexpr size_t PREFETCH_CACHE_MASK = PREFETCH_CACHE_SIZE - 1;

/// Collects every stored field of a document into a [`Document`].
class DocumentVisitor final : public StoredFieldVisitor {
public:
    [[nodiscard]] Status needs_field(const FieldInfo &field_info) override {
        static_cast<void>(field_info);
        return Status::Yes;
    }

    [[nodiscard]] VoidResult binary_field(const FieldInfo &field_info, std::span<const uint8_t> value) override {
        return add(StoredField::from_bytes(field_info.name, std::vector<uint8_t>(value.begin(), value.end())));
    }

    [[nodiscard]] VoidResult string_field(const FieldInfo &field_info, std::string_view value) override {
        return add(StoredField::from_string(field_info.name, std::string(value)));
    }

    [[nodiscard]] VoidResult int_field(const FieldInfo &field_info, int32_t value) override {
        return add(StoredField::from_i32(field_info.name, value));
    }

    [[nodiscard]] VoidResult long_field(const FieldInfo &field_info, int64_t value) override {
        return add(StoredField::from_i64(field_info.name, value));
    }

    [[nodiscard]] VoidResult float_field(const FieldInfo &field_info, float value) override {
        return add(StoredField::from_f32(field_info.name, value));
    }

    [[nodiscard]] VoidResult double_field(const FieldInfo &field_info, double value) override {
        return add(StoredField::from_f64(field_info.name, value));
    }

    [[nodiscard]] Document take() { return std::move(document_); }

private:
    VoidResult add(Field field) {
        document_.add(std::make_unique<Field>(std::move(field)));
        return {};
    }

    Document document_;
};

} // namespace

/// The chunk that is currently loaded, plus the arrays of its header.
struct Lucene90CompressingStoredFieldsReader::BlockState {
    size_t doc_base{0};
    int32_t chunk_docs{0};
    /// Whether the chunk was written as several compressed blocks.
    bool sliced{false};
    /// `chunk_docs + 1` cumulative offsets into `bytes`.
    std::vector<size_t> offsets;
    std::vector<int32_t> num_stored_fields;
    size_t start_pointer{0};
    /// The decompressed chunk.
    std::vector<uint8_t> bytes;

    [[nodiscard]] bool contains(int32_t doc_id) const noexcept {
        return doc_id >= static_cast<int32_t>(doc_base) &&
               doc_id < static_cast<int32_t>(doc_base) + chunk_docs;
    }
};

Lucene90CompressingStoredFieldsReader::Lucene90CompressingStoredFieldsReader(
        int32_t version, std::shared_ptr<const FieldInfos> field_infos,
        std::unique_ptr<Lucene90FieldsIndexReader> index_reader, std::unique_ptr<IndexInput> fields_stream,
        int32_t max_pointer, int32_t chunk_size, CompressionMode compression_mode, int32_t num_docs,
        int64_t num_chunks, int64_t num_dirty_chunks, int64_t num_dirty_docs)
    : version_(version),
      field_infos_(std::move(field_infos)),
      index_reader_(std::move(index_reader)),
      fields_stream_(std::move(fields_stream)),
      max_pointer_(max_pointer),
      chunk_size_(chunk_size),
      compression_mode_(compression_mode),
      decompressor_(compression_mode.new_decompressor()),
      num_docs_(num_docs),
      num_chunks_(num_chunks),
      num_dirty_chunks_(num_dirty_chunks),
      num_dirty_docs_(num_dirty_docs),
      state_(std::make_unique<BlockState>()) {}

Lucene90CompressingStoredFieldsReader::~Lucene90CompressingStoredFieldsReader() = default;

Result<std::unique_ptr<Lucene90CompressingStoredFieldsReader>> Lucene90CompressingStoredFieldsReader::create(
        const Directory &directory, const SegmentInfo &segment_info,
        std::shared_ptr<const FieldInfos> field_infos, const IOContext &context, std::string_view format_name,
        const CompressionMode &compression_mode) {
    const int32_t num_docs = segment_info.max_doc;

    auto fields_stream = directory.open_input(segment_file_name(segment_info.name, "",
                                                                STORED_FIELDS_FIELDS_EXTENSION),
                                              context);
    if (!fields_stream.has_value()) {
        return std::unexpected(std::move(fields_stream.error()));
    }
    auto version = CodecUtil::check_index_header(*fields_stream.value(), format_name, STORED_FIELDS_VERSION_START,
                                                 STORED_FIELDS_VERSION_CURRENT, segment_info.id, "");
    if (!version.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        return std::unexpected(std::move(version.error()));
    }
    // The data file is too big to checksum on open; only its footer structure is
    // validated, which catches truncation.
    auto checksum = CodecUtil::retrieve_checksum(*fields_stream.value());
    if (!checksum.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        return std::unexpected(std::move(checksum.error()));
    }

    auto meta_in = directory.open_input(segment_file_name(segment_info.name, "", STORED_FIELDS_META_EXTENSION),
                                        context);
    if (!meta_in.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        return std::unexpected(std::move(meta_in.error()));
    }
    auto meta_header = CodecUtil::check_index_header(*meta_in.value(), STORED_FIELDS_META_CODEC_NAME,
                                                     STORED_FIELDS_META_VERSION_START, version.value(),
                                                     segment_info.id, "");
    if (!meta_header.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(meta_header.error()));
    }
    auto chunk_size = meta_in.value()->read_vint();
    if (!chunk_size.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(chunk_size.error()));
    }

    auto index_reader = Lucene90FieldsIndexReader::create(directory, segment_info.name, "",
                                                          STORED_FIELDS_INDEX_EXTENSION,
                                                          STORED_FIELDS_INDEX_CODEC_NAME, segment_info.id,
                                                          *meta_in.value(), context);
    if (!index_reader.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(index_reader.error()));
    }
    const int32_t max_pointer = static_cast<int32_t>(index_reader.value()->get_max_pointer());

    auto num_chunks = meta_in.value()->read_vlong();
    auto num_dirty_chunks = meta_in.value()->read_vlong();
    auto num_dirty_docs = meta_in.value()->read_vlong();
    if (!num_chunks.has_value() || !num_dirty_chunks.has_value() || !num_dirty_docs.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("the stored fields metadata is truncated"));
    }
    if (num_chunks.value() < num_dirty_chunks.value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have more dirty chunks than chunks"));
    }
    if ((num_dirty_chunks.value() == 0) != (num_dirty_docs.value() == 0)) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have dirty chunks without dirty docs or vice-versa"));
    }
    if (num_dirty_docs.value() < num_dirty_chunks.value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have more dirty chunks than documents within them"));
    }

    // The metadata file is small, so its whole checksum is verified here rather
    // than only the structure of the footer.
    auto footer = CodecUtil::checksum_entire_file(*meta_in.value());
    if (!footer.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(footer.error()));
    }
    if (auto closed = meta_in.value()->close(); !closed.has_value()) {
        static_cast<void>(fields_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        return std::unexpected(std::move(closed.error()));
    }

    std::unique_ptr<Lucene90CompressingStoredFieldsReader> reader(
            new Lucene90CompressingStoredFieldsReader(
                    version.value(), std::move(field_infos), std::move(index_reader.value()),
                    std::move(fields_stream.value()), max_pointer, chunk_size.value(), compression_mode,
                    num_docs, num_chunks.value(), num_dirty_chunks.value(), num_dirty_docs.value()));
    return std::move(reader);
}

VoidResult Lucene90CompressingStoredFieldsReader::load_chunk(int32_t doc_id) {
    auto doc_base = fields_stream_->read_vint();
    auto token = fields_stream_->read_vint();
    if (!doc_base.has_value() || !token.has_value()) {
        state_->chunk_docs = 0;
        return err(LuceneError::corrupt_index("a stored fields chunk header is truncated"));
    }
    state_->doc_base = static_cast<size_t>(doc_base.value());
    state_->chunk_docs = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 2);
    state_->sliced = (token.value() & 1) != 0;

    if (!state_->contains(doc_id) ||
        static_cast<int64_t>(state_->doc_base) + state_->chunk_docs > static_cast<int64_t>(num_docs_)) {
        state_->chunk_docs = 0;
        return err(LuceneError::corrupt_index("corrupted: docID=" + std::to_string(doc_id) + ", docBase=" +
                                              std::to_string(state_->doc_base) + ", chunkDocs=" +
                                              std::to_string(state_->chunk_docs) + ", numDocs=" +
                                              std::to_string(num_docs_)));
    }

    const auto chunk_docs = static_cast<size_t>(state_->chunk_docs);
    state_->num_stored_fields.assign(chunk_docs, 0);
    state_->offsets.assign(chunk_docs + 1, 0);

    if (state_->chunk_docs == 1) {
        auto num_fields = fields_stream_->read_vint();
        auto length = fields_stream_->read_vint();
        if (!num_fields.has_value() || !length.has_value() || length.value() < 0) {
            state_->chunk_docs = 0;
            return err(LuceneError::corrupt_index("a single-document chunk header is truncated"));
        }
        state_->num_stored_fields[0] = num_fields.value();
        state_->offsets[1] = static_cast<size_t>(length.value());
    } else {
        // Both arrays are stored through StoredFieldsInts; the lengths are
        // cumulative, so they become offsets after a prefix sum.
        std::vector<int64_t> counts(chunk_docs, 0);
        if (auto read = StoredFieldsInts::read_ints(*fields_stream_, chunk_docs, counts, 0); !read.has_value()) {
            state_->chunk_docs = 0;
            return read;
        }
        std::vector<int64_t> lengths(chunk_docs, 0);
        if (auto read = StoredFieldsInts::read_ints(*fields_stream_, chunk_docs, lengths, 0); !read.has_value()) {
            state_->chunk_docs = 0;
            return read;
        }
        for (size_t i = 0; i < chunk_docs; ++i) {
            state_->num_stored_fields[i] = static_cast<int32_t>(counts[i]);
            state_->offsets[i + 1] = state_->offsets[i] + static_cast<size_t>(lengths[i]);
        }
        // Only an empty document has a serialized length of zero.
        for (size_t i = 0; i < chunk_docs; ++i) {
            const bool empty = state_->offsets[i + 1] == state_->offsets[i];
            if (empty != (state_->num_stored_fields[i] == 0)) {
                state_->chunk_docs = 0;
                return err(LuceneError::corrupt_index("length=" + std::to_string(state_->offsets[i + 1] -
                                                                                 state_->offsets[i]) +
                                                      ", numStoredFields=" +
                                                      std::to_string(state_->num_stored_fields[i])));
            }
        }
    }

    auto pointer = fields_stream_->get_file_pointer();
    if (!pointer.has_value()) {
        state_->chunk_docs = 0;
        return std::unexpected(std::move(pointer.error()));
    }
    state_->start_pointer = pointer.value();

    const size_t total_length = state_->offsets[chunk_docs];
    state_->bytes.assign(total_length, 0);
    std::vector<uint8_t> buffer;
    if (state_->sliced) {
        // Every block was compressed on its own.
        size_t decompressed = 0;
        while (decompressed < total_length) {
            const size_t to_decompress =
                    std::min(total_length - decompressed, static_cast<size_t>(chunk_size_));
            if (auto result = decompressor_->decompress(*fields_stream_, static_cast<int32_t>(to_decompress), 0,
                                                        static_cast<int32_t>(to_decompress), buffer);
                !result.has_value()) {
                state_->chunk_docs = 0;
                return result;
            }
            std::copy_n(buffer.begin(), to_decompress, state_->bytes.begin() +
                                                              static_cast<std::ptrdiff_t>(decompressed));
            decompressed += to_decompress;
        }
    } else if (auto result = decompressor_->decompress(*fields_stream_, static_cast<int32_t>(total_length), 0,
                                                       static_cast<int32_t>(total_length), buffer);
               !result.has_value()) {
        state_->chunk_docs = 0;
        return result;
    } else {
        std::copy_n(buffer.begin(), total_length, state_->bytes.begin());
    }
    return {};
}

VoidResult Lucene90CompressingStoredFieldsReader::read_field(DataInput &input, StoredFieldVisitor &visitor,
                                                             const FieldInfo &field_info, int32_t bits) {
    switch (bits & static_cast<int32_t>(SF_TYPE_MASK)) {
        case SF_BYTE_ARR: {
            auto length = input.read_vint();
            if (!length.has_value() || length.value() < 0) {
                return err(LuceneError::corrupt_index("a stored binary value has no length"));
            }
            auto bytes = input.get_bytes(static_cast<size_t>(length.value()));
            if (!bytes.has_value()) {
                return std::unexpected(std::move(bytes.error()));
            }
            return visitor.binary_field(field_info, std::span<const uint8_t>(bytes.value()));
        }
        case SF_STRING: {
            auto value = input.read_string();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return visitor.string_field(field_info, value.value());
        }
        case SF_NUMERIC_INT: {
            auto value = input.read_zint();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return visitor.int_field(field_info, value.value());
        }
        case SF_NUMERIC_FLOAT: {
            auto value = read_zfloat(input);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return visitor.float_field(field_info, value.value());
        }
        case SF_NUMERIC_LONG: {
            auto value = read_tlong(input);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return visitor.long_field(field_info, value.value());
        }
        case SF_NUMERIC_DOUBLE: {
            auto value = read_zdouble(input);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return visitor.double_field(field_info, value.value());
        }
        default:
            return err(LuceneError::illegal_state("unknown stored field type flag"));
    }
}

VoidResult Lucene90CompressingStoredFieldsReader::skip_field(DataInput &input, int32_t bits) {
    switch (bits & static_cast<int32_t>(SF_TYPE_MASK)) {
        case SF_BYTE_ARR:
        case SF_STRING: {
            auto length = input.read_vint();
            if (!length.has_value()) {
                return std::unexpected(std::move(length.error()));
            }
            return input.skip_bytes(length.value());
        }
        case SF_NUMERIC_INT: {
            auto value = input.read_zint();
            return value.has_value() ? VoidResult{} : std::unexpected(std::move(value.error()));
        }
        case SF_NUMERIC_FLOAT: {
            auto value = read_zfloat(input);
            return value.has_value() ? VoidResult{} : std::unexpected(std::move(value.error()));
        }
        case SF_NUMERIC_LONG: {
            auto value = read_tlong(input);
            return value.has_value() ? VoidResult{} : std::unexpected(std::move(value.error()));
        }
        case SF_NUMERIC_DOUBLE: {
            auto value = read_zdouble(input);
            return value.has_value() ? VoidResult{} : std::unexpected(std::move(value.error()));
        }
        default:
            return err(LuceneError::illegal_state("unknown stored field type flag"));
    }
}

VoidResult Lucene90CompressingStoredFieldsReader::prefetch(int32_t doc_id) {
    auto block_id = index_reader_->get_block_id(doc_id);
    if (!block_id.has_value()) {
        return std::unexpected(std::move(block_id.error()));
    }
    auto start = index_reader_->get_block_start_pointer(block_id.value());
    if (!start.has_value()) {
        return std::unexpected(std::move(start.error()));
    }
    auto length = index_reader_->get_block_length(block_id.value());
    if (!length.has_value()) {
        return std::unexpected(std::move(length.error()));
    }
    return fields_stream_->prefetch(start.value(), length.value());
}

VoidResult Lucene90CompressingStoredFieldsReader::document_with_visitor(int32_t doc_id,
                                                                       StoredFieldVisitor &visitor) {
    if (!state_->contains(doc_id)) {
        auto pointer = index_reader_->get_start_pointer(doc_id);
        if (!pointer.has_value()) {
            return std::unexpected(std::move(pointer.error()));
        }
        if (auto sought = fields_stream_->seek(pointer.value()); !sought.has_value()) {
            return sought;
        }
        if (auto loaded = load_chunk(doc_id); !loaded.has_value()) {
            return loaded;
        }
    }

    const size_t index = static_cast<size_t>(doc_id) - state_->doc_base;
    const size_t offset = state_->offsets[index];
    const size_t length = state_->offsets[index + 1] - offset;
    const int32_t num_stored_fields = state_->num_stored_fields[index];
    if (length == 0) {
        return {};
    }

    ByteArrayDataInput input = ByteArrayDataInput::with_range(state_->bytes, offset, length);
    for (int32_t field_index = 0; field_index < num_stored_fields; ++field_index) {
        auto info_and_bits = input.read_vlong();
        if (!info_and_bits.has_value()) {
            return std::unexpected(std::move(info_and_bits.error()));
        }
        const int32_t field_number = static_cast<int32_t>(static_cast<uint64_t>(info_and_bits.value()) >>
                                                          SF_TYPE_BITS);
        const int32_t bits = static_cast<int32_t>(info_and_bits.value() & SF_TYPE_MASK);
        const FieldInfo *field_info = field_infos_->by_number(field_number);
        if (field_info == nullptr) {
            return err(LuceneError::illegal_state("no field info for stored field number " +
                                                  std::to_string(field_number)));
        }

        switch (visitor.needs_field(*field_info)) {
            case Status::Yes:
                if (auto read = read_field(input, visitor, *field_info, bits); !read.has_value()) {
                    return read;
                }
                break;
            case Status::No:
                // The value of the last field does not have to be skipped: the
                // document ends here anyway.
                if (field_index == num_stored_fields - 1) {
                    return {};
                }
                if (auto skipped = skip_field(input, bits); !skipped.has_value()) {
                    return skipped;
                }
                break;
            case Status::Stop:
                return {};
        }
    }
    return {};
}

Result<Document> Lucene90CompressingStoredFieldsReader::document(int32_t doc_id) {
    DocumentVisitor visitor;
    if (auto loaded = document_with_visitor(doc_id, visitor); !loaded.has_value()) {
        return std::unexpected(std::move(loaded.error()));
    }
    return visitor.take();
}

VoidResult Lucene90CompressingStoredFieldsReader::check_integrity() const {
    auto index = index_reader_->check_integrity();
    if (!index.has_value()) {
        return index;
    }
    if (fields_stream_ == nullptr) {
        return err(LuceneError::already_closed("this StoredFieldsReader is closed"));
    }
    auto checksum = CodecUtil::checksum_entire_file(*fields_stream_);
    if (!checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

Result<std::unique_ptr<StoredFieldsReader>> Lucene90CompressingStoredFieldsReader::get_merge_instance() const {
    auto instance = clone();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    return instance;
}

Result<std::unique_ptr<StoredFieldsReader>> Lucene90CompressingStoredFieldsReader::clone() const {
    if (fields_stream_ == nullptr) {
        return err(LuceneError::already_closed("this StoredFieldsReader is closed"));
    }
    auto index = index_reader_->clone_reader();
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto stream = fields_stream_->clone();
    if (!stream.has_value()) {
        return std::unexpected(std::move(stream.error()));
    }
    std::unique_ptr<StoredFieldsReader> reader(new Lucene90CompressingStoredFieldsReader(
            version_, field_infos_, std::move(index.value()), std::move(stream.value()), max_pointer_, chunk_size_,
            compression_mode_, num_docs_, num_chunks_, num_dirty_chunks_, num_dirty_docs_));
    return reader;
}

VoidResult Lucene90CompressingStoredFieldsReader::close() const {
    if (fields_stream_ == nullptr) {
        return {};
    }
    auto index = index_reader_->close();
    auto stream = fields_stream_->close();
    fields_stream_.reset();
    if (!index.has_value()) {
        return index;
    }
    return stream;
}

} // namespace lucene
