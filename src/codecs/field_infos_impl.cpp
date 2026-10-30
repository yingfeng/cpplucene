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

module lucene_core:field_infos.impl;

import :field_infos;

import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :index_input;
import :index_output;
import :index_types;
import :io_context;
import :segment_info;
import :types;

import std;

namespace lucene {

namespace {

constexpr uint8_t kStoreTermVector = 0x1;
constexpr uint8_t kOmitNorms = 0x2;
constexpr uint8_t kStorePayloads = 0x4;
constexpr uint8_t kSoftDeletes = 0x8;
constexpr uint8_t kParent = 0x10;

constexpr int32_t kMaxIndexOptions = static_cast<int32_t>(IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
constexpr int32_t kMaxDocValuesByte = 5;
constexpr int32_t kMaxVectorEncoding = static_cast<int32_t>(VectorEncoding::Float32);
constexpr int32_t kMaxVectorSimilarity = static_cast<int32_t>(VectorSimilarityFunction::MaximumInnerProduct);
constexpr int32_t kMaxSkipIndexType = static_cast<int32_t>(DocValuesSkipIndexType::Range);

/// Doc values types as they are numbered **on disk**.
///
/// Lucene94 numbers `SortedSet` before `SortedNumeric`, the opposite of this
/// port's enum, so the two cannot be cast into each other.
uint8_t doc_values_byte(DocValuesType type) {
    switch (type) {
        case DocValuesType::None:
            return 0;
        case DocValuesType::Numeric:
            return 1;
        case DocValuesType::Binary:
            return 2;
        case DocValuesType::Sorted:
            return 3;
        case DocValuesType::SortedSet:
            return 4;
        case DocValuesType::SortedNumeric:
            return 5;
    }
    return 0;
}

Result<DocValuesType> doc_values_type_from_byte(int32_t byte) {
    switch (byte) {
        case 0:
            return DocValuesType::None;
        case 1:
            return DocValuesType::Numeric;
        case 2:
            return DocValuesType::Binary;
        case 3:
            return DocValuesType::Sorted;
        case 4:
            return DocValuesType::SortedSet;
        case 5:
            return DocValuesType::SortedNumeric;
        default:
            return err(LuceneError::corrupt_index("invalid docvalues byte: " + std::to_string(byte)));
    }
}

/// Reads one byte and range-checks it, so a corrupt file cannot produce an
/// out-of-range enum value.
Result<int32_t> read_checked_byte(DataInput &input, int32_t max_value, std::string_view what) {
    auto byte = input.read_byte();
    if (!byte.has_value()) {
        return err(byte.error());
    }
    const int32_t value = static_cast<int32_t>(byte.value());
    if (value > max_value) {
        return err(LuceneError::corrupt_index("invalid " + std::string(what) + ": " + std::to_string(value) +
                                              " (resource=" + std::string(input.to_string()) + ")"));
    }
    return value;
}

Result<int32_t> read_vint_checked(DataInput &input, std::string_view what) {
    auto value = input.read_vint();
    if (!value.has_value()) {
        return err(value.error());
    }
    if (value.value() < 0) {
        return err(LuceneError::corrupt_index("invalid " + std::string(what) + ": " + std::to_string(value.value()) +
                                              " (resource=" + std::string(input.to_string()) + ")"));
    }
    return value;
}

} // namespace

Result<FieldInfos> FieldInfos::of(std::vector<FieldInfo> fields) {
    std::sort(fields.begin(), fields.end(),
              [](const FieldInfo &left, const FieldInfo &right) { return left.number < right.number; });

    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].number != static_cast<int32_t>(i)) {
            return err(LuceneError::illegal_argument("field numbers must be dense and start at 0: field " +
                                                     fields[i].name + " has number " +
                                                     std::to_string(fields[i].number) + " at position " +
                                                     std::to_string(i)));
        }
        if (fields[i].name.empty()) {
            return err(LuceneError::illegal_argument("a field name must not be empty"));
        }
        if (i > 0 && fields[i - 1].name == fields[i].name) {
            return err(LuceneError::illegal_argument("duplicate field name: " + fields[i].name));
        }
        if (fields[i].point_dimension_count < 0 || fields[i].point_index_dimension_count < 0 ||
            fields[i].point_num_bytes < 0 || fields[i].vector_dimension < 0) {
            return err(LuceneError::illegal_argument("field dimensions must not be negative: " + fields[i].name));
        }
        if (fields[i].point_index_dimension_count > fields[i].point_dimension_count) {
            return err(LuceneError::illegal_argument("the point index dimension count must not exceed the point "
                                                     "dimension count: field " + fields[i].name));
        }
        if (fields[i].point_dimension_count > 0 && fields[i].point_num_bytes == 0) {
            return err(LuceneError::illegal_argument("a point field needs a non-zero point byte size: field " +
                                                     fields[i].name));
        }
    }

    FieldInfos infos;
    infos.fields_ = std::move(fields);
    return infos;
}

const FieldInfo *FieldInfos::by_name(std::string_view name) const {
    for (const auto &field : fields_) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

const FieldInfo *FieldInfos::by_number(int32_t number) const {
    if (number < 0 || static_cast<size_t>(number) >= fields_.size()) {
        return nullptr;
    }
    const auto &field = fields_[static_cast<size_t>(number)];
    return field.number == number ? &field : nullptr;
}

std::set<std::string> FieldInfos::indexed_fields() const {
    std::set<std::string> names;
    for (const auto &field : fields()) {
        if (field.is_indexed()) {
            names.insert(field.name);
        }
    }
    return names;
}

namespace {

template <class Predicate>
bool any_field(const std::vector<FieldInfo> &fields, Predicate predicate) {
    for (const auto &field : fields) {
        if (predicate(field)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool FieldInfos::has_freqs() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_freqs(); });
}

bool FieldInfos::has_positions() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_positions(); });
}

bool FieldInfos::has_payloads() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_payloads(); });
}

bool FieldInfos::has_offsets() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_offsets(); });
}

bool FieldInfos::has_norms() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_norms(); });
}

bool FieldInfos::has_term_vectors() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_term_vectors(); });
}

bool FieldInfos::has_postings() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.is_indexed(); });
}

bool FieldInfos::has_doc_values() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_doc_values(); });
}

bool FieldInfos::has_point_values() const {
    return any_field(fields(), [](const FieldInfo &field) { return field.has_point_values(); });
}

std::string Lucene94FieldInfosFormat::file_name(std::string_view segment, std::string_view suffix) {
    return segment_file_name(segment, suffix, FNM_EXTENSION);
}

VoidResult Lucene94FieldInfosFormat::write_field_infos(DataOutput &out, const FieldInfos &infos) {
    if (infos.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return err(LuceneError::illegal_argument("too many fields"));
    }
    if (auto written = out.write_vint(static_cast<int32_t>(infos.size())); !written.has_value()) {
        return written;
    }

    for (const auto &field : infos.fields()) {
        if (auto written = out.write_string(field.name); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_vint(field.number); !written.has_value()) {
            return written;
        }

        uint8_t bits = 0;
        if (field.store_term_vector) {
            bits |= kStoreTermVector;
        }
        if (field.omit_norms) {
            bits |= kOmitNorms;
        }
        if (field.store_payloads) {
            bits |= kStorePayloads;
        }
        if (field.soft_deletes) {
            bits |= kSoftDeletes;
        }
        if (field.parent) {
            bits |= kParent;
        }
        if (auto written = out.write_byte(bits); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_byte(static_cast<uint8_t>(field.index_options)); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_byte(doc_values_byte(field.doc_values_type)); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_byte(static_cast<uint8_t>(field.doc_values_skip_index)); !written.has_value()) {
            return written;
        }
        // The doc values generation uses the plain (little-endian) `writeLong`,
        // like the reference implementation; `write_be_long` is only for the
        // codec and footer magic numbers.
        if (auto written = out.write_long(field.doc_values_gen); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_map_of_strings(field.attributes); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_vint(field.point_dimension_count); !written.has_value()) {
            return written;
        }
        if (field.point_dimension_count != 0) {
            // The index dimension count and byte size are only stored when the
            // field has points at all; a reader must not consume them otherwise.
            if (auto written = out.write_vint(field.point_index_dimension_count); !written.has_value()) {
                return written;
            }
            if (auto written = out.write_vint(field.point_num_bytes); !written.has_value()) {
                return written;
            }
        }
        if (auto written = out.write_vint(field.vector_dimension); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_byte(static_cast<uint8_t>(field.vector_encoding)); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_byte(static_cast<uint8_t>(field.vector_similarity)); !written.has_value()) {
            return written;
        }
    }
    return {};
}

Result<FieldInfos> Lucene94FieldInfosFormat::parse_field_infos(DataInput &input) {
    auto count = read_vint_checked(input, "field count");
    if (!count.has_value()) {
        return err(count.error());
    }

    std::vector<FieldInfo> fields;
    fields.reserve(static_cast<size_t>(count.value()));
    for (int32_t i = 0; i < count.value(); ++i) {
        FieldInfo field;
        field.number = i;

        auto name = input.read_string();
        if (!name.has_value()) {
            return err(name.error());
        }
        field.name = std::move(name.value());

        auto field_number = read_vint_checked(input, "field number");
        if (!field_number.has_value()) {
            return err(field_number.error());
        }
        field.number = field_number.value();

        auto bits = input.read_byte();
        if (!bits.has_value()) {
            return err(bits.error());
        }
        field.store_term_vector = (bits.value() & kStoreTermVector) != 0;
        field.omit_norms = (bits.value() & kOmitNorms) != 0;
        field.store_payloads = (bits.value() & kStorePayloads) != 0;
        field.soft_deletes = (bits.value() & kSoftDeletes) != 0;
        field.parent = (bits.value() & kParent) != 0;
        if ((bits.value() & 0xC0) != 0) {
            return err(LuceneError::corrupt_index("unused field bits are set in FieldInfo: " +
                                                  std::to_string(bits.value())));
        }

        auto index_options = read_checked_byte(input, kMaxIndexOptions, "IndexOptions");
        if (!index_options.has_value()) {
            return err(index_options.error());
        }
        field.index_options = static_cast<IndexOptions>(index_options.value());

        auto doc_values_type = read_checked_byte(input, kMaxDocValuesByte, "DocValuesType");
        if (!doc_values_type.has_value()) {
            return err(doc_values_type.error());
        }
        auto mapped_doc_values = doc_values_type_from_byte(doc_values_type.value());
        if (!mapped_doc_values.has_value()) {
            return err(mapped_doc_values.error());
        }
        field.doc_values_type = mapped_doc_values.value();

        auto skip_index = read_checked_byte(input, kMaxSkipIndexType, "DocValuesSkipIndexType");
        if (!skip_index.has_value()) {
            return err(skip_index.error());
        }
        field.doc_values_skip_index = static_cast<DocValuesSkipIndexType>(skip_index.value());

        auto doc_values_gen = input.read_long();
        if (!doc_values_gen.has_value()) {
            return err(doc_values_gen.error());
        }
        field.doc_values_gen = doc_values_gen.value();

        auto attributes = input.read_map_of_strings();
        if (!attributes.has_value()) {
            return err(attributes.error());
        }
        field.attributes = std::move(attributes.value());

        auto point_dimension_count = read_vint_checked(input, "point dimension count");
        if (!point_dimension_count.has_value()) {
            return err(point_dimension_count.error());
        }
        field.point_dimension_count = point_dimension_count.value();

        if (field.point_dimension_count != 0) {
            auto point_index_dimension_count = read_vint_checked(input, "point index dimension count");
            if (!point_index_dimension_count.has_value()) {
                return err(point_index_dimension_count.error());
            }
            field.point_index_dimension_count = point_index_dimension_count.value();

            auto point_num_bytes = read_vint_checked(input, "point num bytes");
            if (!point_num_bytes.has_value()) {
                return err(point_num_bytes.error());
            }
            field.point_num_bytes = point_num_bytes.value();
        }

        auto vector_dimension = read_vint_checked(input, "vector dimension");
        if (!vector_dimension.has_value()) {
            return err(vector_dimension.error());
        }
        field.vector_dimension = vector_dimension.value();

        auto vector_encoding = read_checked_byte(input, kMaxVectorEncoding, "VectorEncoding");
        if (!vector_encoding.has_value()) {
            return err(vector_encoding.error());
        }
        field.vector_encoding = static_cast<VectorEncoding>(vector_encoding.value());

        auto vector_similarity = read_checked_byte(input, kMaxVectorSimilarity, "VectorSimilarityFunction");
        if (!vector_similarity.has_value()) {
            return err(vector_similarity.error());
        }
        field.vector_similarity = static_cast<VectorSimilarityFunction>(vector_similarity.value());

        fields.push_back(std::move(field));
    }
    return FieldInfos::of(std::move(fields));
}

VoidResult Lucene94FieldInfosFormat::write(Directory &dir, std::string_view segment, std::string_view suffix,
                                           std::span<const uint8_t> id, const FieldInfos &infos,
                                           const IOContext &context) {
    const std::string name = file_name(segment, suffix);
    auto output = dir.create_output(name, context);
    if (!output.has_value()) {
        return err(output.error());
    }
    IndexOutput &out = *output.value();

    auto result = [&]() -> VoidResult {
        if (auto written = CodecUtil::write_index_header(out, CODEC_NAME, VERSION_CURRENT, id, suffix);
            !written.has_value()) {
            return written;
        }
        if (auto written = write_field_infos(out, infos); !written.has_value()) {
            return written;
        }
        return CodecUtil::write_footer(out);
    }();

    const auto closed = out.close();
    if (!result.has_value()) {
        return result;
    }
    return closed;
}

Result<FieldInfos> Lucene94FieldInfosFormat::read(const Directory &dir, std::string_view segment,
                                                 std::string_view suffix, std::span<const uint8_t> id,
                                                 const IOContext &context) {
    const std::string name = file_name(segment, suffix);
    auto input = dir.open_input(name, context);
    if (!input.has_value()) {
        return err(input.error());
    }
    IndexInput &in = *input.value();

    auto result = [&]() -> Result<FieldInfos> {
        if (auto checked = CodecUtil::check_index_header(in, CODEC_NAME, VERSION_CURRENT, VERSION_CURRENT, id, suffix);
            !checked.has_value()) {
            return err(checked.error());
        }
        auto parsed = parse_field_infos(in);
        if (!parsed.has_value()) {
            return err(parsed.error());
        }
        if (auto footer = CodecUtil::check_footer(in); !footer.has_value()) {
            return err(footer.error());
        }
        return parsed;
    }();

    const auto closed = in.close();
    if (!result.has_value()) {
        return result;
    }
    if (!closed.has_value()) {
        return err(closed.error());
    }
    return result;
}

} // namespace lucene
