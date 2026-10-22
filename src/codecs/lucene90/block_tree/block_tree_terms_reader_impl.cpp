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

module lucene_core:block_tree_terms_reader.impl;

import :block_tree_terms_reader;

import :block_tree_segment_terms_enum;
import :block_tree_terms_constants;
import :bytes_ref;
import :check_sum_index_input;
import :codec_util;
import :data_input;
import :debug;
import :directory;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :index_file_names;
import :index_input;
import :io_context;
import :postings_reader_base;
import :segment_info;
import :segment_read_state;
import :terms;
import :types;

import std;

namespace lucene {

namespace {

/// `read_bytes_ref`: a vint length followed by that many bytes.
Result<BytesRef> read_bytes_ref(DataInput &input) {
    auto num_bytes = input.read_vint();
    if (!num_bytes) {
        return std::unexpected(std::move(num_bytes.error()));
    }
    if (num_bytes.value() < 0) {
        return err(LuceneError::corrupt_index(std::format("invalid bytes length: {} (resource={})",
                                                          num_bytes.value(), input.to_string())));
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(num_bytes.value()), 0);
    if (auto read = input.read_bytes(buffer); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return BytesRef(std::move(buffer));
}

} // namespace

TermsReader::TermsReader(std::unique_ptr<IndexInput> terms_in, std::unique_ptr<PostingsReaderBase> postings_reader,
                         std::string segment, int32_t version)
    : terms_in_(std::move(terms_in)),
      postings_reader_(std::move(postings_reader)),
      segment_(std::move(segment)),
      version_(version) {}

Result<std::unique_ptr<IndexInput>> TermsReader::clone_terms_input() const {
    if (terms_in_ == nullptr) {
        return err(LuceneError::illegal_state("terms input is closed"));
    }
    return terms_in_->clone();
}

VoidResult TermsReader::close() {
    if (terms_in_ != nullptr) {
        if (auto closed = terms_in_->close(); !closed) {
            return closed;
        }
        terms_in_.reset();
    }
    if (postings_reader_ != nullptr) {
        if (auto closed = postings_reader_->close(); !closed) {
            return closed;
        }
    }
    return {};
}

Result<std::unique_ptr<Lucene90BlockTreeTermsReader>> Lucene90BlockTreeTermsReader::create(
        std::unique_ptr<PostingsReaderBase> postings_reader, SegmentReadState &state,
        const SegmentInfo &segment_info) {
    if (postings_reader == nullptr) {
        return err(LuceneError::illegal_argument("postings reader is missing"));
    }
    const std::string &segment = segment_info.name;

    // ---- .tim ----
    const std::string terms_name = segment_file_name(segment, state.segment_suffix, TERMS_EXTENSION);
    auto terms_in_result = state.directory.open_input(terms_name, state.context);
    if (!terms_in_result) {
        return std::unexpected(std::move(terms_in_result.error()));
    }
    std::unique_ptr<IndexInput> terms_in = std::move(terms_in_result.value());

    // Only the current format is read (see the project's "current version only"
    // rule), so the accepted version range is a single value.
    auto version = CodecUtil::check_index_header(*terms_in, TERMS_CODEC_NAME, BLOCK_TREE_VERSION_CURRENT,
                                                BLOCK_TREE_VERSION_CURRENT, segment_info.id, state.segment_suffix);
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }

    // ---- .tip ----
    const std::string index_name = segment_file_name(segment, state.segment_suffix, TERMS_INDEX_EXTENSION);
    auto index_in_result = state.directory.open_input(index_name, state.context);
    if (!index_in_result) {
        return std::unexpected(std::move(index_in_result.error()));
    }
    std::shared_ptr<IndexInput> index_in(std::move(index_in_result.value()));
    if (auto checked = CodecUtil::check_index_header(*index_in, TERMS_INDEX_CODEC_NAME, version.value(),
                                                     version.value(), segment_info.id, state.segment_suffix);
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }

    // ---- .tmd ----
    const std::string meta_name = segment_file_name(segment, state.segment_suffix, TERMS_META_EXTENSION);
    auto meta_in_result = state.directory.open_checksum_input(meta_name);
    if (!meta_in_result) {
        return std::unexpected(std::move(meta_in_result.error()));
    }
    std::unique_ptr<CheckSumIndexInput> meta_in = std::move(meta_in_result.value());

    if (auto checked = CodecUtil::check_index_header(*meta_in, TERMS_META_CODEC_NAME, version.value(),
                                                     version.value(), segment_info.id, state.segment_suffix);
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }

    if (auto initialized = postings_reader->init(*meta_in, state, segment_info); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }

    auto num_fields = meta_in->read_vint();
    if (!num_fields) {
        return std::unexpected(std::move(num_fields.error()));
    }
    if (num_fields.value() < 0) {
        return err(LuceneError::corrupt_index(std::format("invalid numFields: {}", num_fields.value())));
    }

    std::map<int32_t, std::unique_ptr<FieldReader>> field_map;
    for (int32_t i = 0; i < num_fields.value(); ++i) {
        auto field = meta_in->read_vint();
        if (!field) {
            return std::unexpected(std::move(field.error()));
        }
        auto num_terms = meta_in->read_vlong();
        if (!num_terms) {
            return std::unexpected(std::move(num_terms.error()));
        }
        if (num_terms.value() <= 0) {
            return err(LuceneError::corrupt_index(
                    std::format("Illegal numTerms for field number: {}", field.value())));
        }

        auto root_code = read_bytes_ref(*meta_in);
        if (!root_code) {
            return std::unexpected(std::move(root_code.error()));
        }

        const FieldInfo *field_info = state.field_infos->by_number(field.value());
        if (field_info == nullptr) {
            return err(LuceneError::corrupt_index(std::format("invalid field number: {}", field.value())));
        }

        auto sum_total_term_freq = meta_in->read_vlong();
        if (!sum_total_term_freq) {
            return std::unexpected(std::move(sum_total_term_freq.error()));
        }
        // When frequencies are omitted only one value is written.
        int64_t sum_doc_freq = sum_total_term_freq.value();
        if (field_info->index_options != IndexOptions::Docs) {
            auto read = meta_in->read_vlong();
            if (!read) {
                return std::unexpected(std::move(read.error()));
            }
            sum_doc_freq = read.value();
        }

        auto doc_count = meta_in->read_vint();
        if (!doc_count) {
            return std::unexpected(std::move(doc_count.error()));
        }
        auto min_term = read_bytes_ref(*meta_in);
        if (!min_term) {
            return std::unexpected(std::move(min_term.error()));
        }
        auto max_term = read_bytes_ref(*meta_in);
        if (!max_term) {
            return std::unexpected(std::move(max_term.error()));
        }
        if (num_terms.value() == 1) {
            debug_check(max_term.value() == min_term.value());
        }

        if (doc_count.value() < 0 || doc_count.value() > segment_info.max_doc) {
            return err(LuceneError::corrupt_index(
                    std::format("invalid docCount: {} maxDoc: {}", doc_count.value(), segment_info.max_doc)));
        }
        if (sum_doc_freq < static_cast<int64_t>(doc_count.value())) {
            return err(LuceneError::corrupt_index(
                    std::format("invalid sumDocFreq: {} docCount: {}", sum_doc_freq, doc_count.value())));
        }
        if (sum_total_term_freq.value() < sum_doc_freq) {
            return err(LuceneError::corrupt_index(std::format(
                    "invalid sumTotalTermFreq: {} sumDocFreq: {}", sum_total_term_freq.value(), sum_doc_freq)));
        }

        auto index_start_fp = meta_in->read_vlong();
        if (!index_start_fp) {
            return std::unexpected(std::move(index_start_fp.error()));
        }

        auto field_reader = FieldReader::create(
                std::make_shared<FieldInfo>(*field_info), num_terms.value(), std::move(root_code.value()),
                sum_total_term_freq.value(), sum_doc_freq, doc_count.value(), index_start_fp.value(), *meta_in,
                std::move(min_term.value()), std::move(max_term.value()));
        if (!field_reader) {
            return std::unexpected(std::move(field_reader.error()));
        }

        if (field_map.contains(field_info->number)) {
            return err(LuceneError::corrupt_index(std::format("duplicate field: {}", field_info->name)));
        }
        field_map.emplace(field_info->number, std::move(field_reader.value()));
    }

    auto index_length = meta_in->read_long();
    if (!index_length) {
        return std::unexpected(std::move(index_length.error()));
    }
    auto terms_length = meta_in->read_long();
    if (!terms_length) {
        return std::unexpected(std::move(terms_length.error()));
    }
    if (auto footer = CodecUtil::check_footer(*meta_in); !footer) {
        return std::unexpected(std::move(footer.error()));
    }
    if (auto closed = meta_in->close(); !closed) {
        return std::unexpected(std::move(closed.error()));
    }

    // The meta file checksum is verified, so the recorded lengths are trusted.
    if (auto retrieved = CodecUtil::retrieve_checksum_with_expected(*index_in, static_cast<size_t>(index_length.value()));
        !retrieved) {
        return std::unexpected(std::move(retrieved.error()));
    }
    if (auto retrieved = CodecUtil::retrieve_checksum_with_expected(*terms_in, static_cast<size_t>(terms_length.value()));
        !retrieved) {
        return std::unexpected(std::move(retrieved.error()));
    }

    auto reader = std::unique_ptr<Lucene90BlockTreeTermsReader>(new Lucene90BlockTreeTermsReader());
    reader->terms_reader_ = std::make_shared<TermsReader>(std::move(terms_in), std::move(postings_reader),
                                                          segment, version.value());
    reader->index_in_ = index_in;
    reader->field_infos_ = state.field_infos;

    for (auto &[number, field_reader] : field_map) {
        (void)number;
        field_reader->set_parent(reader->terms_reader_.get());
        // The FST of every field is read from the shared `.tip` input.
        if (auto initialized = FieldReader::init_field_reader(*index_in, *field_reader); !initialized) {
            return std::unexpected(std::move(initialized.error()));
        }
    }

    // `sort_field_names`: the field list is sorted by name.
    std::vector<std::string> field_names;
    field_names.reserve(field_map.size());
    for (const auto &[number, field_reader] : field_map) {
        const FieldInfo *field_info = state.field_infos->by_number(number);
        if (field_info == nullptr) {
            return err(LuceneError::illegal_state(
                    std::format("Missing field info for field number {}", number)));
        }
        field_names.push_back(field_info->name);
    }
    std::sort(field_names.begin(), field_names.end());

    reader->field_map_ = std::move(field_map);
    reader->field_list_ = std::move(field_names);
    return reader;
}

Result<std::unique_ptr<Terms>> Lucene90BlockTreeTermsReader::terms(std::string_view field) const {
    const FieldInfo *field_info = field_infos_->by_name(field);
    if (field_info == nullptr) {
        return static_cast<std::unique_ptr<Terms>>(nullptr);
    }
    const auto found = field_map_.find(field_info->number);
    if (found == field_map_.end()) {
        return static_cast<std::unique_ptr<Terms>>(nullptr);
    }
    auto cloned = found->second->clone_for_enum();
    if (!cloned.has_value()) {
        return std::unexpected(std::move(cloned.error()));
    }
    std::unique_ptr<Terms> result = std::move(cloned.value());
    return result;
}

Result<std::unique_ptr<FieldNameIterator>> Lucene90BlockTreeTermsReader::iterator() const {
    /// Walks the sorted field-name list of the reader.
    class ListIterator final : public FieldNameIterator {
    public:
        explicit ListIterator(const std::vector<std::string> &names) : names_(names) {}

        [[nodiscard]] Result<const std::string *> next() override {
            if (index_ >= names_.size()) {
                return static_cast<const std::string *>(nullptr);
            }
            return &names_[index_++];
        }

    private:
        const std::vector<std::string> &names_;
        size_t index_{0};
    };

    return std::unique_ptr<FieldNameIterator>(std::make_unique<ListIterator>(field_list_));
}

Result<int32_t> Lucene90BlockTreeTermsReader::size() const {
    return static_cast<int32_t>(field_map_.size());
}

VoidResult Lucene90BlockTreeTermsReader::check_integrity() const {
    if (index_in_ == nullptr) {
        return err(LuceneError::illegal_state("terms index input is closed"));
    }
    if (auto checked = CodecUtil::checksum_entire_file(*index_in_); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = CodecUtil::checksum_entire_file(terms_reader_->terms_in()); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    return terms_reader_->postings_reader().check_integrity();
}

VoidResult Lucene90BlockTreeTermsReader::close() const {
    VoidResult result;
    if (index_in_ != nullptr) {
        if (auto closed = index_in_->close(); !closed) {
            result = std::move(closed);
        }
        index_in_.reset();
    }
    if (terms_reader_ != nullptr) {
        if (auto closed = terms_reader_->close(); !closed && result) {
            result = std::move(closed);
        }
    }
    // Drop the field readers so that references to the terms index are
    // releasable even when the caller keeps this reader.
    field_map_.clear();
    return result;
}

std::string Lucene90BlockTreeTermsReader::to_string() const {
    return std::format("Lucene90BlockTreeTermsReader(fields={},delegate={})", field_map_.size(),
                       terms_reader_->postings_reader().to_string());
}

} // namespace lucene
