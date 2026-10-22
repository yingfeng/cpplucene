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

module lucene_core:lucene101_postings_reader.impl;

import :bit_util;
import :block_term_state;
import :codec_util;
import :data_input;
import :debug;
import :doc_id_set_iterator;
import :error;
import :field_infos;
import :for_delta_util;
import :for_util;
import :impact;
import :impacts_enum;
import :index_input;
import :index_types;
import :lucene101_postings_format;
import :lucene101_postings_reader;
import :pfor_util;
import :posting_decoding_util;
import :postings_enum;
import :postings_reader_base;
import :postings_util;
import :segment_info;
import :segment_read_state;
import :types;

import std;

namespace lucene {

namespace {

/// `wrapping_add` for the signed arithmetic the reference implementation relies on.
int32_t wrapping_add32(int32_t left, int32_t right) {
    return static_cast<int32_t>(static_cast<uint32_t>(left) + static_cast<uint32_t>(right));
}

int64_t wrapping_add64(int64_t left, int64_t right) {
    return static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
}

} // namespace

void prefix_sum(std::span<int32_t> buffer, size_t count, int32_t base) {
    debug_check(count <= buffer.size());
    buffer[0] = wrapping_add32(buffer[0], base);
    for (size_t i = 1; i < count; ++i) {
        buffer[i] = wrapping_add32(buffer[i], buffer[i - 1]);
    }
}

int32_t sum_over_range(std::span<const int32_t> arr, size_t start, size_t end) {
    int32_t total = 0;
    for (size_t i = start; i < end; ++i) {
        total = wrapping_add32(total, arr[i]);
    }
    return total;
}

Result<int32_t> read_vint15(DataInput &input) {
    auto short_value = input.read_short();
    if (!short_value) {
        return std::unexpected(std::move(short_value.error()));
    }
    const int32_t s = short_value.value();
    if (s >= 0) {
        return s;
    }
    auto higher = input.read_vint();
    if (!higher) {
        return std::unexpected(std::move(higher.error()));
    }
    return (s & 0x7FFF) | (higher.value() << 15);
}

Result<int64_t> read_vlong15(DataInput &input) {
    auto short_value = input.read_short();
    if (!short_value) {
        return std::unexpected(std::move(short_value.error()));
    }
    const int64_t s = short_value.value();
    if (s >= 0) {
        return s;
    }
    auto higher = input.read_vlong();
    if (!higher) {
        return std::unexpected(std::move(higher.error()));
    }
    return (s & 0x7FFF) | (higher.value() << 15);
}

VoidResult read_impacts(ByteArrayDataInput &in, MutableImpactList &reuse) {
    int32_t freq = 0;
    int64_t norm = 0;
    reuse.impacts.clear();

    while (in.get_position() < in.length()) {
        auto freq_delta_result = in.read_vint();
        if (!freq_delta_result) {
            return std::unexpected(std::move(freq_delta_result.error()));
        }
        const int32_t freq_delta = freq_delta_result.value();
        freq = wrapping_add32(freq, wrapping_add32(1, static_cast<int32_t>(static_cast<uint32_t>(freq_delta) >> 1)));
        if ((freq_delta & 1) != 0) {
            auto norm_delta = in.read_zlong();
            if (!norm_delta) {
                return std::unexpected(std::move(norm_delta.error()));
            }
            norm = wrapping_add64(wrapping_add64(norm, 1), norm_delta.value());
        } else {
            norm = wrapping_add64(norm, 1);
        }
        // The reference implementation keeps a fixed decode bound so that corrupt input cannot make
        // the list grow without bound; `reserve()` in `decode` provides it.
        reuse.impacts.push_back(Impact{freq, norm});
    }
    reuse.length = reuse.impacts.size();
    return {};
}

VoidResult prefetch_postings(IndexInput &doc_in, const IntBlockTermState &state) {
    debug_check(state.base.doc_freq > 1);
    auto pointer = doc_in.get_file_pointer();
    if (!pointer) {
        return std::unexpected(std::move(pointer.error()));
    }
    if (static_cast<int64_t>(pointer.value()) != state.doc_start_fp) {
        // Don't prefetch if the input is already positioned at the right offset,
        // which suggests that the caller is streaming the entire inverted index
        // (e.g. for merging): let the read-ahead logic do its work instead.
        if (auto prefetched = doc_in.prefetch(static_cast<size_t>(state.doc_start_fp), 1); !prefetched) {
            return prefetched;
        }
    }
    // Positions and offsets are less likely to be needed, so they are not
    // prefetched.
    return {};
}

ImpactsImpl::ImpactsImpl(bool index_has_freq, int32_t level0_last_doc_id, int32_t level1_last_doc_id,
                         std::optional<std::span<const uint8_t>> level0_serialized_impacts,
                         std::optional<std::span<const uint8_t>> level1_serialized_impacts,
                         size_t max_num_impacts_at_level0, size_t max_num_impacts_at_level1)
    : index_has_freq_(index_has_freq),
      level0_last_doc_id_(level0_last_doc_id),
      level1_last_doc_id_(level1_last_doc_id),
      level0_serialized_impacts_(level0_serialized_impacts),
      level1_serialized_impacts_(level1_serialized_impacts),
      max_num_impacts_at_level0_(max_num_impacts_at_level0),
      max_num_impacts_at_level1_(max_num_impacts_at_level1) {}

Result<MutableImpactList> ImpactsImpl::decode(std::span<const uint8_t> serialized, size_t level_impacts_len) {
    ByteArrayDataInput scratch{serialized};
    // Each impact consumes at least one byte.
    MutableImpactList level_impacts;
    level_impacts.impacts.reserve(std::min(level_impacts_len, serialized.size()));
    if (auto read = read_impacts(scratch, level_impacts); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return level_impacts;
}

size_t ImpactsImpl::num_levels() const {
    if (!index_has_freq_ || level1_last_doc_id_ == NO_MORE_DOCS) {
        return 1;
    }
    return 2;
}

int32_t ImpactsImpl::get_doc_id_upto(size_t level) const {
    if (!index_has_freq_) {
        return NO_MORE_DOCS;
    }
    if (level == 0) {
        return level0_last_doc_id_;
    }
    if (level == 1) {
        return level1_last_doc_id_;
    }
    return NO_MORE_DOCS;
}

Result<std::vector<Impact>> ImpactsImpl::get_impacts(size_t level) const {
    if (index_has_freq_) {
        if (level == 0 && level0_last_doc_id_ != NO_MORE_DOCS) {
            if (!level0_serialized_impacts_.has_value()) {
                return err(LuceneError::illegal_state("level 0 impacts are missing"));
            }
            auto decoded = decode(level0_serialized_impacts_.value(), max_num_impacts_at_level0_);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            decoded.value().impacts.resize(decoded.value().length);
            return std::move(decoded.value().impacts);
        }
        if (level == 1) {
            if (!level1_serialized_impacts_.has_value()) {
                return err(LuceneError::illegal_state("level 1 impacts are missing"));
            }
            auto decoded = decode(level1_serialized_impacts_.value(), max_num_impacts_at_level1_);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            decoded.value().impacts.resize(decoded.value().length);
            return std::move(decoded.value().impacts);
        }
    }
    return std::vector<Impact>{default_impact()};
}

namespace {

Result<size_t> as_file_length(int64_t value, std::string_view what) {
    if (value < 0) {
        return err(LuceneError::corrupt_index(std::format("negative {}: {}", what, value)));
    }
    return static_cast<size_t>(value);
}

} // namespace

VoidResult Lucene101PostingsReader::close_quietly() noexcept {
    for (std::unique_ptr<IndexInput> *input : {&doc_in_, &pos_in_, &pay_in_}) {
        if (*input != nullptr) {
            static_cast<void>((*input)->close());
            input->reset();
        }
    }
    return {};
}

Result<std::unique_ptr<Lucene101PostingsReader>>
Lucene101PostingsReader::create(const SegmentReadState &state, const SegmentInfo &segment_info) {
    std::unique_ptr<Lucene101PostingsReader> reader{new Lucene101PostingsReader()};
    const FieldInfos *field_infos = state.field_infos.get();

    auto fail = [&reader](LuceneError error) -> Result<std::unique_ptr<Lucene101PostingsReader>> {
        static_cast<void>(reader->close_quietly());
        return err(std::move(error));
    };

    const std::string meta_name = Lucene101PostingsFormat::file_name(
            segment_info.name, state.segment_suffix, Lucene101PostingsFormat::META_EXTENSION);
    const std::span<const uint8_t> id(segment_info.id);

    auto meta = state.directory.open_checksum_input(meta_name);
    if (!meta) {
        return fail(std::move(meta.error()));
    }
    auto &meta_in = *meta.value();

    auto header = CodecUtil::check_index_header(meta_in, Lucene101PostingsFormat::META_CODEC,
                                                Lucene101PostingsFormat::VERSION_START,
                                                Lucene101PostingsFormat::VERSION_CURRENT, id, state.segment_suffix);
    if (!header) {
        return fail(std::move(header.error()));
    }
    const int32_t version = header.value();

    auto level0_max = meta_in.read_int();
    if (!level0_max) {
        return fail(std::move(level0_max.error()));
    }
    reader->max_num_impacts_at_level0_ = level0_max.value();

    auto level0_bytes = meta_in.read_int();
    if (!level0_bytes) {
        return fail(std::move(level0_bytes.error()));
    }
    reader->max_impact_num_bytes_at_level0_ = level0_bytes.value();

    auto level1_max = meta_in.read_int();
    if (!level1_max) {
        return fail(std::move(level1_max.error()));
    }
    reader->max_num_impacts_at_level1_ = level1_max.value();

    auto level1_bytes = meta_in.read_int();
    if (!level1_bytes) {
        return fail(std::move(level1_bytes.error()));
    }
    reader->max_impact_num_bytes_at_level1_ = level1_bytes.value();

    auto doc_length = meta_in.read_long();
    if (!doc_length) {
        return fail(std::move(doc_length.error()));
    }
    const int64_t expected_doc_file_length = doc_length.value();
    int64_t expected_pos_file_length = -1;
    int64_t expected_pay_file_length = -1;

    const bool index_has_positions = field_infos != nullptr && field_infos->has_positions();
    if (index_has_positions) {
        auto pos_length = meta_in.read_long();
        if (!pos_length) {
            return fail(std::move(pos_length.error()));
        }
        expected_pos_file_length = pos_length.value();
        if (field_infos->has_payloads() || field_infos->has_offsets()) {
            auto pay_length = meta_in.read_long();
            if (!pay_length) {
                return fail(std::move(pay_length.error()));
            }
            expected_pay_file_length = pay_length.value();
        }
    }

    // Cheap structural check: FOOTER_MAGIC + algorithmID.  The data files are
    // only validated against their CRC in `check_integrity`.
    if (auto footer = CodecUtil::check_footer(meta_in); !footer) {
        return fail(std::move(footer.error()));
    }
    if (auto closed = meta_in.close(); !closed) {
        return fail(std::move(closed.error()));
    }

    const std::string doc_name = Lucene101PostingsFormat::file_name(
            segment_info.name, state.segment_suffix, Lucene101PostingsFormat::DOC_EXTENSION);
    // Postings are read forward only, so the reference implementation's `ReadAdvice::Normal` is what
    // the port gets by default.
    auto doc = state.directory.open_input(doc_name, state.context);
    if (!doc) {
        return fail(std::move(doc.error()));
    }
    reader->doc_in_ = std::move(doc.value());

    auto doc_header = CodecUtil::check_index_header(*reader->doc_in_, Lucene101PostingsFormat::DOC_CODEC, version,
                                                    version, id, state.segment_suffix);
    if (!doc_header) {
        return fail(std::move(doc_header.error()));
    }
    auto doc_expected = as_file_length(expected_doc_file_length, "doc file length");
    if (!doc_expected) {
        return fail(std::move(doc_expected.error()));
    }
    if (auto checksum = CodecUtil::retrieve_checksum_with_expected(*reader->doc_in_, doc_expected.value());
        !checksum) {
        return fail(std::move(checksum.error()));
    }

    if (index_has_positions) {
        const std::string pos_name = Lucene101PostingsFormat::file_name(
                segment_info.name, state.segment_suffix, Lucene101PostingsFormat::POS_EXTENSION);
        auto pos = state.directory.open_input(pos_name, state.context);
        if (!pos) {
            return fail(std::move(pos.error()));
        }
        reader->pos_in_ = std::move(pos.value());
        auto pos_header = CodecUtil::check_index_header(*reader->pos_in_, Lucene101PostingsFormat::POS_CODEC,
                                                        version, version, id, state.segment_suffix);
        if (!pos_header) {
            return fail(std::move(pos_header.error()));
        }
        auto pos_expected = as_file_length(expected_pos_file_length, "pos file length");
        if (!pos_expected) {
            return fail(std::move(pos_expected.error()));
        }
        if (auto checksum = CodecUtil::retrieve_checksum_with_expected(*reader->pos_in_, pos_expected.value());
            !checksum) {
            return fail(std::move(checksum.error()));
        }

        if (expected_pay_file_length != -1) {
            const std::string pay_name = Lucene101PostingsFormat::file_name(
                    segment_info.name, state.segment_suffix, Lucene101PostingsFormat::PAY_EXTENSION);
            auto pay = state.directory.open_input(pay_name, state.context);
            if (!pay) {
                return fail(std::move(pay.error()));
            }
            reader->pay_in_ = std::move(pay.value());
            auto pay_header = CodecUtil::check_index_header(*reader->pay_in_, Lucene101PostingsFormat::PAY_CODEC,
                                                            version, version, id, state.segment_suffix);
            if (!pay_header) {
                return fail(std::move(pay_header.error()));
            }
            auto pay_expected = as_file_length(expected_pay_file_length, "pay file length");
            if (!pay_expected) {
                return fail(std::move(pay_expected.error()));
            }
            if (auto checksum = CodecUtil::retrieve_checksum_with_expected(*reader->pay_in_, pay_expected.value());
                !checksum) {
                return fail(std::move(checksum.error()));
            }
        }
    }

    return reader;
}

VoidResult Lucene101PostingsReader::init(IndexInput &terms_in, const SegmentReadState &state,
                                         const SegmentInfo &segment_info) {
    // Make sure we are talking to the matching postings writer.
    const std::span<const uint8_t> id(segment_info.id);
    auto header = CodecUtil::check_index_header(terms_in, Lucene101PostingsFormat::TERMS_CODEC,
                                                Lucene101PostingsFormat::VERSION_START,
                                                Lucene101PostingsFormat::VERSION_CURRENT, id, state.segment_suffix);
    if (!header) {
        return std::unexpected(std::move(header.error()));
    }
    auto index_block_size = terms_in.read_vint();
    if (!index_block_size) {
        return std::unexpected(std::move(index_block_size.error()));
    }
    if (static_cast<size_t>(index_block_size.value()) != ForUtil::BLOCK_SIZE) {
        return err(LuceneError::illegal_state(
                std::format("index-time BLOCK_SIZE ({}) != read-time BLOCK_SIZE ({})", index_block_size.value(),
                            ForUtil::BLOCK_SIZE)));
    }
    return {};
}

Result<TermStateEnum> Lucene101PostingsReader::new_term_state() const {
    return TermStateEnum{IntBlockTermState{}};
}

VoidResult Lucene101PostingsReader::decode_term(DataInput &input, const FieldInfo &field_info,
                                                TermStateEnum &state, bool absolute) const {
    auto *term_state = std::get_if<IntBlockTermState>(&state.value_mut());
    if (term_state == nullptr) {
        return err(LuceneError::illegal_state("term_state should be IntBlockTermState"));
    }
    if (absolute) {
        term_state->doc_start_fp = 0;
        term_state->pos_start_fp = 0;
        term_state->pay_start_fp = 0;
    }

    auto l_result = input.read_vlong();
    if (!l_result) {
        return std::unexpected(std::move(l_result.error()));
    }
    const int64_t l = l_result.value();
    if ((l & 1) == 0) {
        term_state->doc_start_fp += static_cast<int64_t>(static_cast<uint64_t>(l) >> 1);
        if (term_state->base.doc_freq == 1) {
            auto singleton = input.read_vint();
            if (!singleton) {
                return std::unexpected(std::move(singleton.error()));
            }
            term_state->singleton_doc_id = singleton.value();
        } else {
            term_state->singleton_doc_id = -1;
        }
    } else {
        debug_check(!absolute);
        debug_check(term_state->singleton_doc_id != -1);
        const auto delta = BitUtil::zig_zag_decode_i64(static_cast<uint64_t>(l) >> 1);
        term_state->singleton_doc_id += static_cast<int32_t>(delta);
    }

    if (field_info.index_options >= IndexOptions::DocsAndFreqsAndPositions) {
        auto pos_start = input.read_vlong();
        if (!pos_start) {
            return std::unexpected(std::move(pos_start.error()));
        }
        term_state->pos_start_fp += pos_start.value();

        if (field_info.index_options >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets ||
            field_info.has_payloads()) {
            auto pay_start = input.read_vlong();
            if (!pay_start) {
                return std::unexpected(std::move(pay_start.error()));
            }
            term_state->pay_start_fp += pay_start.value();
        }
        if (term_state->base.total_term_freq > static_cast<int64_t>(ForUtil::BLOCK_SIZE)) {
            auto last_pos_block = input.read_vlong();
            if (!last_pos_block) {
                return std::unexpected(std::move(last_pos_block.error()));
            }
            term_state->last_pos_block_offset = last_pos_block.value();
        } else {
            term_state->last_pos_block_offset = -1;
        }
    }
    return {};
}

Result<std::unique_ptr<PostingsEnum>>
Lucene101PostingsReader::postings(const FieldInfo &field_info, const TermStateEnum &state,
                                  std::unique_ptr<PostingsEnum> reuse, int32_t flags) const {
    auto *self = const_cast<Lucene101PostingsReader *>(this);
    if (reuse != nullptr) {
        auto *existing = dynamic_cast<BlockPostingsEnum *>(reuse.get());
        if (existing != nullptr && existing->can_reuse(doc_in_.get(), field_info, flags, false)) {
            if (auto reset = existing->reset(state, *self); !reset) {
                return std::unexpected(std::move(reset.error()));
            }
            return reuse;
        }
    }
    auto created = BlockPostingsEnum::create(field_info, flags, false, *self);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    if (auto reset = created.value()->reset(state, *self); !reset) {
        return std::unexpected(std::move(reset.error()));
    }
    return std::unique_ptr<PostingsEnum>(std::move(created.value()));
}

Result<std::unique_ptr<ImpactsEnum>> Lucene101PostingsReader::impacts(const FieldInfo &field_info,
                                                                     const TermStateEnum &state, int32_t flags) const {
    auto *self = const_cast<Lucene101PostingsReader *>(this);
    auto created = BlockPostingsEnum::create(field_info, flags, true, *self);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }
    if (auto reset = created.value()->reset(state, *self); !reset) {
        return std::unexpected(std::move(reset.error()));
    }
    return std::unique_ptr<ImpactsEnum>(std::move(created.value()));
}

VoidResult Lucene101PostingsReader::check_integrity() const {
    if (doc_in_ != nullptr) {
        if (auto checked = CodecUtil::checksum_entire_file(*doc_in_); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }
    if (pos_in_ != nullptr) {
        if (auto checked = CodecUtil::checksum_entire_file(*pos_in_); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }
    if (pay_in_ != nullptr) {
        if (auto checked = CodecUtil::checksum_entire_file(*pay_in_); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
    }
    return {};
}

VoidResult Lucene101PostingsReader::close() {
    VoidResult status{};
    for (std::unique_ptr<IndexInput> *input : {&doc_in_, &pos_in_, &pay_in_}) {
        if (*input != nullptr) {
            auto closed = (*input)->close();
            if (status.has_value() && !closed.has_value()) {
                status = std::move(closed);
            }
            input->reset();
        }
    }
    return status;
}

std::string Lucene101PostingsReader::to_string() const {
    return std::format("Lucene101PostingsReader(positions={},payloads={})", pos_in_ != nullptr,
                       pay_in_ != nullptr);
}

namespace {

constexpr size_t BLOCK_SIZE = Lucene101PostingsFormat::BLOCK_SIZE;

/// `VECTOR_UTIL.find_next_geq` of the reference implementation, scalar.
size_t find_next_geq(std::span<const int32_t> buffer, int32_t target, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (buffer[i] >= target) {
            return i;
        }
    }
    return to;
}

} // namespace

PostingDecodingUtil *BlockPostingsEnum::doc_util() {
    return doc_in_util_.has_value() ? &doc_in_util_.value() : nullptr;
}

Result<std::unique_ptr<BlockPostingsEnum>>
BlockPostingsEnum::create(const FieldInfo &field_info, int32_t flags, bool needs_impacts,
                          Lucene101PostingsReader &reader) {
    std::unique_ptr<BlockPostingsEnum> enumeration{new BlockPostingsEnum()};
    const IndexOptions options = field_info.index_options;
    enumeration->options_ = options;
    enumeration->index_has_freq_ = options >= IndexOptions::DocsAndFreqs;
    enumeration->index_has_pos_ = options >= IndexOptions::DocsAndFreqsAndPositions;
    enumeration->index_has_offsets_ = options >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    enumeration->index_has_payloads_ = field_info.has_payloads();
    enumeration->index_has_offsets_or_payloads_ =
            enumeration->index_has_offsets_ || enumeration->index_has_payloads_;

    enumeration->flags_ = flags;
    enumeration->needs_freq_ = enumeration->index_has_freq_ && feature_requested(flags, FREQS);
    enumeration->needs_pos_ = enumeration->index_has_pos_ && feature_requested(flags, POSITIONS);
    enumeration->needs_offsets_ = enumeration->index_has_offsets_ && feature_requested(flags, OFFSETS);
    enumeration->needs_payloads_ = enumeration->index_has_payloads_ && feature_requested(flags, PAYLOADS);
    enumeration->needs_offsets_or_payloads_ = enumeration->needs_offsets_ || enumeration->needs_payloads_;
    enumeration->needs_impacts_ = needs_impacts;
    enumeration->needs_docs_and_freqs_only_ = !enumeration->needs_pos_ && !needs_impacts;

    enumeration->doc_input_ = reader.doc_input();
    enumeration->max_num_impacts_at_level0_ = reader.max_num_impacts_at_level0();
    enumeration->max_num_impacts_at_level1_ = reader.max_num_impacts_at_level1();

    enumeration->doc_buffer_.assign(BLOCK_SIZE, 0);
    enumeration->freq_buffer_.assign(BLOCK_SIZE, enumeration->needs_freq_ ? 0 : 1);

    if (enumeration->needs_freq_ && needs_impacts) {
        if (reader.max_impact_num_bytes_at_level0() < 0 || reader.max_impact_num_bytes_at_level1() < 0) {
            return err(LuceneError::illegal_state("max impact byte counts must not be negative"));
        }
        enumeration->level0_serialized_impacts_.reserve(
                static_cast<size_t>(reader.max_impact_num_bytes_at_level0()));
        enumeration->level1_serialized_impacts_.reserve(
                static_cast<size_t>(reader.max_impact_num_bytes_at_level1()));
    }

    if (enumeration->needs_pos_) {
        if (reader.pos_input() == nullptr) {
            return err(LuceneError::illegal_state("positions input is missing"));
        }
        auto cloned = reader.pos_input()->clone();
        if (!cloned) {
            return err(cloned.error());
        }
        enumeration->pos_in_clone_ = std::move(cloned.value());
        enumeration->pos_in_util_.emplace(*enumeration->pos_in_clone_);
        enumeration->pos_delta_buffer_.assign(BLOCK_SIZE, 0);
    }

    if (enumeration->needs_offsets_or_payloads_) {
        if (reader.pay_input() == nullptr) {
            return err(LuceneError::illegal_state("payload input is missing"));
        }
        auto cloned = reader.pay_input()->clone();
        if (!cloned) {
            return err(cloned.error());
        }
        enumeration->pay_in_clone_ = std::move(cloned.value());
        enumeration->pay_in_util_.emplace(*enumeration->pay_in_clone_);
    }

    if (enumeration->needs_offsets_) {
        enumeration->offset_start_delta_buffer_.assign(BLOCK_SIZE, 0);
        enumeration->offset_length_buffer_.assign(BLOCK_SIZE, 0);
        enumeration->start_offset_ = 0;
        enumeration->end_offset_ = 0;
    } else {
        enumeration->start_offset_ = -1;
        enumeration->end_offset_ = -1;
    }

    if (enumeration->index_has_payloads_) {
        enumeration->payload_length_buffer_.assign(BLOCK_SIZE, 0);
        enumeration->payload_bytes_.assign(128, 0);
    }

    return enumeration;
}

bool BlockPostingsEnum::can_reuse(const IndexInput *doc_input, const FieldInfo &field_info, int32_t flags,
                                  bool needs_impacts) const {
    return doc_input_ == doc_input && options_ == field_info.index_options &&
           index_has_payloads_ == field_info.has_payloads() && flags_ == flags &&
           needs_impacts_ == needs_impacts;
}

VoidResult BlockPostingsEnum::reset(const TermStateEnum &term_state, Lucene101PostingsReader &reader) {
    const auto *state = std::get_if<IntBlockTermState>(&term_state.value());
    if (state == nullptr) {
        return err(LuceneError::illegal_state("term_state should be IntBlockTermState"));
    }
    doc_freq_ = state->base.doc_freq;
    singleton_doc_id_ = state->singleton_doc_id;
    if (doc_freq_ > 1) {
        if (!doc_in_util_.has_value()) {
            auto cloned = reader.doc_input()->clone();
            if (!cloned) {
                return std::unexpected(std::move(cloned.error()));
            }
            doc_in_clone_ = std::move(cloned.value());
            doc_in_util_.emplace(*doc_in_clone_);
        }
        if (auto prefetched = prefetch_postings(*doc_in_clone_, *state); !prefetched) {
            return prefetched;
        }
    }

    if (!for_delta_util_.has_value() && static_cast<size_t>(doc_freq_) >= BLOCK_SIZE) {
        for_delta_util_.emplace();
    }
    total_term_freq_ = index_has_freq_ ? state->base.total_term_freq : static_cast<int64_t>(state->base.doc_freq);
    if (needs_freq_ && !pfor_util_.has_value() && static_cast<size_t>(total_term_freq_) >= BLOCK_SIZE) {
        pfor_util_.emplace();
    }

    const int64_t pos_term_start_fp = state->pos_start_fp;
    const int64_t pay_term_start_fp = state->pay_start_fp;
    if (pos_in_util_.has_value()) {
        if (auto seeked = pos_in_clone_->seek(static_cast<size_t>(pos_term_start_fp)); !seeked) {
            return seeked;
        }
        if (pay_in_util_.has_value()) {
            if (auto seeked = pay_in_clone_->seek(static_cast<size_t>(pay_term_start_fp)); !seeked) {
                return seeked;
            }
        }
    }
    level1_pos_end_fp_ = pos_term_start_fp;
    level1_pay_end_fp_ = pay_term_start_fp;
    level0_pos_end_fp_ = pos_term_start_fp;
    level0_pay_end_fp_ = pay_term_start_fp;
    pos_pending_count_ = 0;
    payload_byte_upto_ = 0;
    if (static_cast<size_t>(state->base.total_term_freq) <= BLOCK_SIZE) {
        last_pos_block_fp_ = state->base.total_term_freq == static_cast<int64_t>(BLOCK_SIZE)
                                     ? -1
                                     : state->pos_start_fp;
    } else {
        last_pos_block_fp_ = state->pos_start_fp + state->last_pos_block_offset;
    }

    level1_block_pos_upto_ = 0;
    level1_block_pay_upto_ = 0;
    level0_block_pos_upto_ = 0;
    level0_block_pay_upto_ = 0;
    pos_buffer_upto_ = BLOCK_SIZE;

    doc_ = -1;
    prev_doc_id_ = -1;
    doc_count_left_ = doc_freq_;
    has_freq_fp_ = false;
    level0_last_doc_id_ = -1;
    if (doc_freq_ < Lucene101PostingsFormat::LEVEL1_NUM_DOCS) {
        level1_last_doc_id_ = NO_MORE_DOCS;
        if (doc_freq_ > 1) {
            if (doc_util() == nullptr) {
                return err(LuceneError::illegal_state("documents input is missing"));
            }
            if (auto seeked = doc_in_clone_->seek(static_cast<size_t>(state->doc_start_fp)); !seeked) {
                return seeked;
            }
        }
    } else {
        level1_last_doc_id_ = -1;
        level1_doc_end_fp_ = state->doc_start_fp;
    }
    level1_doc_count_upto_ = 0;
    doc_buffer_size_ = BLOCK_SIZE;
    doc_buffer_upto_ = BLOCK_SIZE;
    pos_doc_buffer_upto_ = BLOCK_SIZE;
    needs_refilling_ = false;
    return {};
}

VoidResult BlockPostingsEnum::refill_full_block() {
    if (!for_delta_util_.has_value()) {
        return err(LuceneError::illegal_state("FOR decoder is missing"));
    }
    if (doc_util() == nullptr) {
        return err(LuceneError::illegal_state("documents input is missing"));
    }
    if (auto decoded = for_delta_util_->decode_and_prefix_sum(*doc_util(), prev_doc_id_,
                                                              std::span<int32_t>(doc_buffer_));
        !decoded) {
        return decoded;
    }
    if (index_has_freq_) {
        auto &doc_in = *doc_in_clone_;
        if (needs_freq_) {
            auto pointer = doc_in.get_file_pointer();
            if (!pointer) {
                return std::unexpected(std::move(pointer.error()));
            }
            freq_fp_ = pointer.value();
            has_freq_fp_ = true;
        }
        if (auto skipped = PForUtil::skip(doc_in); !skipped) {
            return skipped;
        }
    }

    doc_count_left_ -= static_cast<int32_t>(BLOCK_SIZE);
    prev_doc_id_ = doc_buffer_[BLOCK_SIZE - 1];
    doc_buffer_upto_ = 0;
    pos_doc_buffer_upto_ = 0;
    return {};
}

VoidResult BlockPostingsEnum::refill_remainder() {
    const size_t doc_count = static_cast<size_t>(doc_count_left_);
    debug_check(doc_count_left_ >= 0 && doc_count < BLOCK_SIZE);

    if (doc_freq_ == 1) {
        doc_buffer_[0] = singleton_doc_id_;
        freq_buffer_[0] = static_cast<int32_t>(total_term_freq_);
        doc_buffer_[1] = NO_MORE_DOCS;
        debug_check(!has_freq_fp_);
        doc_count_left_ = 0;
        doc_buffer_size_ = 1;
    } else {
        if (doc_util() == nullptr) {
            return err(LuceneError::illegal_state("documents input is missing"));
        }
        if (auto read = PostingsUtil::read_vint_block(doc_util()->input(), std::span<int32_t>(doc_buffer_),
                                                     std::span<int32_t>(freq_buffer_), doc_count,
                                                     index_has_freq_, needs_freq_);
            !read) {
            return read;
        }
        prefix_sum(std::span<int32_t>(doc_buffer_), doc_count, prev_doc_id_);
        doc_buffer_[doc_count] = NO_MORE_DOCS;
        has_freq_fp_ = false;
        doc_buffer_size_ = doc_count;
        doc_count_left_ = 0;
    }
    prev_doc_id_ = doc_buffer_[BLOCK_SIZE - 1];
    doc_buffer_upto_ = 0;
    pos_doc_buffer_upto_ = 0;
    debug_check(doc_buffer_[doc_buffer_size_] == NO_MORE_DOCS);
    return {};
}

VoidResult BlockPostingsEnum::refill_docs() {
    debug_check(doc_count_left_ >= 0);
    if (static_cast<size_t>(doc_count_left_) >= BLOCK_SIZE) {
        return refill_full_block();
    }
    return refill_remainder();
}

VoidResult BlockPostingsEnum::skip_level1_to(int32_t target) {
    if (doc_in_clone_ == nullptr) {
        return err(LuceneError::illegal_state("documents input is missing"));
    }
    auto &doc_in = *doc_in_clone_;
    while (true) {
        prev_doc_id_ = level1_last_doc_id_;
        level0_last_doc_id_ = level1_last_doc_id_;
        if (auto seeked = doc_in.seek(static_cast<size_t>(level1_doc_end_fp_)); !seeked) {
            return seeked;
        }
        level0_pos_end_fp_ = level1_pos_end_fp_;
        level0_block_pos_upto_ = level1_block_pos_upto_;
        level0_pay_end_fp_ = level1_pay_end_fp_;
        level0_block_pay_upto_ = level1_block_pay_upto_;

        doc_count_left_ = doc_freq_ - level1_doc_count_upto_;
        level1_doc_count_upto_ += Lucene101PostingsFormat::LEVEL1_NUM_DOCS;

        if (doc_count_left_ < Lucene101PostingsFormat::LEVEL1_NUM_DOCS) {
            level1_last_doc_id_ = NO_MORE_DOCS;
            break;
        }

        auto doc_delta = doc_in.read_vint();
        if (!doc_delta) {
            return std::unexpected(std::move(doc_delta.error()));
        }
        level1_last_doc_id_ += doc_delta.value();
        auto end_delta = doc_in.read_vlong();
        if (!end_delta) {
            return std::unexpected(std::move(end_delta.error()));
        }
        auto pointer = doc_in.get_file_pointer();
        if (!pointer) {
            return std::unexpected(std::move(pointer.error()));
        }
        level1_doc_end_fp_ = end_delta.value() + static_cast<int64_t>(pointer.value());

        if (index_has_freq_) {
            auto skip1_length = doc_in.read_short();
            if (!skip1_length) {
                return std::unexpected(std::move(skip1_length.error()));
            }
            auto after_length = doc_in.get_file_pointer();
            if (!after_length) {
                return std::unexpected(std::move(after_length.error()));
            }
            const int64_t skip1_end_fp = static_cast<int64_t>(skip1_length.value()) +
                                         static_cast<int64_t>(after_length.value());
            auto num_impact_bytes = doc_in.read_short();
            if (!num_impact_bytes) {
                return std::unexpected(std::move(num_impact_bytes.error()));
            }
            if (num_impact_bytes.value() < 0) {
                return err(LuceneError::corrupt_index("negative impact length"));
            }
            const auto impact_bytes = static_cast<size_t>(num_impact_bytes.value());

            if (needs_impacts_ && level1_last_doc_id_ >= target) {
                level1_serialized_impacts_.resize(impact_bytes);
                const std::span<uint8_t> impacts(level1_serialized_impacts_);
                if (auto read = doc_in.read_bytes(impacts); !read) {
                    return read;
                }
            } else {
                if (auto skipped = doc_in.skip_bytes(static_cast<int64_t>(impact_bytes)); !skipped) {
                    return skipped;
                }
            }

            if (index_has_pos_) {
                auto pos_end_delta = doc_in.read_vlong();
                if (!pos_end_delta) {
                    return std::unexpected(std::move(pos_end_delta.error()));
                }
                level1_pos_end_fp_ += pos_end_delta.value();
                auto block_pos_upto = doc_in.read_byte();
                if (!block_pos_upto) {
                    return std::unexpected(std::move(block_pos_upto.error()));
                }
                level1_block_pos_upto_ = static_cast<int32_t>(block_pos_upto.value());
                if (index_has_offsets_or_payloads_) {
                    auto pay_end_delta = doc_in.read_vlong();
                    if (!pay_end_delta) {
                        return std::unexpected(std::move(pay_end_delta.error()));
                    }
                    level1_pay_end_fp_ += pay_end_delta.value();
                    auto block_pay_upto = doc_in.read_vint();
                    if (!block_pay_upto) {
                        return std::unexpected(std::move(block_pay_upto.error()));
                    }
                    level1_block_pay_upto_ = block_pay_upto.value();
                }
                auto final_pointer = doc_in.get_file_pointer();
                if (!final_pointer) {
                    return std::unexpected(std::move(final_pointer.error()));
                }
                debug_check(static_cast<int64_t>(final_pointer.value()) == skip1_end_fp);
            }
        }

        if (level1_last_doc_id_ >= target) {
            break;
        }
    }
    return {};
}

VoidResult BlockPostingsEnum::seek_pos_data(int64_t pos_fp, int32_t pos_upto, int64_t pay_fp, int32_t pay_upto) {
    // If the target position is behind the current one, the positions of the
    // first docs of the next block are already decoded: accumulate their
    // frequencies into `pos_pending_count_` instead of decoding them twice.
    auto &pos_in = *pos_in_clone_;
    auto pos_pointer = pos_in.get_file_pointer();
    if (!pos_pointer) {
        return std::unexpected(std::move(pos_pointer.error()));
    }
    if (pos_fp >= static_cast<int64_t>(pos_pointer.value())) {
        if (auto seeked = pos_in.seek(static_cast<size_t>(pos_fp)); !seeked) {
            return seeked;
        }
        pos_pending_count_ = pos_upto;
        if (pay_in_util_.has_value()) {
            auto &pay_in = *pay_in_clone_;
            auto pay_pointer = pay_in.get_file_pointer();
            if (!pay_pointer) {
                return std::unexpected(std::move(pay_pointer.error()));
            }
            debug_check(level0_pay_end_fp_ >= static_cast<int64_t>(pay_pointer.value()));
            if (auto seeked = pay_in.seek(static_cast<size_t>(pay_fp)); !seeked) {
                return seeked;
            }
            payload_byte_upto_ = static_cast<size_t>(pay_upto);
        }
        pos_buffer_upto_ = BLOCK_SIZE;
    } else {
        pos_pending_count_ +=
                sum_over_range(std::span<const int32_t>(freq_buffer_), pos_doc_buffer_upto_, BLOCK_SIZE);
    }
    return {};
}

VoidResult BlockPostingsEnum::skip_level0_to(int32_t target) {
    int64_t pos_fp = 0;
    int32_t pos_upto = 0;
    int64_t pay_fp = 0;
    int32_t pay_upto = 0;
    // The reference implementation dereferences the documents input lazily, inside the block loop:
    // a term with a single document never touches it (the singleton document
    // id lives in the term state), so the input may legitimately be null here.
    while (true) {
        prev_doc_id_ = level0_last_doc_id_;

        pos_fp = level0_pos_end_fp_;
        pos_upto = level0_block_pos_upto_;
        pay_fp = level0_pay_end_fp_;
        pay_upto = level0_block_pay_upto_;

        if (static_cast<size_t>(doc_count_left_) >= BLOCK_SIZE) {
            if (doc_in_clone_ == nullptr) {
                return err(LuceneError::illegal_state("documents input is missing"));
            }
            auto &doc_in = *doc_in_clone_;
            auto num_skip_bytes = doc_in.read_vlong();
            if (!num_skip_bytes) {
                return std::unexpected(std::move(num_skip_bytes.error()));
            }
            if (num_skip_bytes.value() < 0) {
                return err(LuceneError::corrupt_index("negative skip length"));
            }
            auto skip0_pointer = doc_in.get_file_pointer();
            if (!skip0_pointer) {
                return std::unexpected(std::move(skip0_pointer.error()));
            }
            const size_t skip0_end = skip0_pointer.value() + static_cast<size_t>(num_skip_bytes.value());
            auto doc_delta = read_vint15(doc_in);
            if (!doc_delta) {
                return std::unexpected(std::move(doc_delta.error()));
            }
            level0_last_doc_id_ += doc_delta.value();
            const bool found = target <= level0_last_doc_id_;
            auto block_length = read_vlong15(doc_in);
            if (!block_length) {
                return std::unexpected(std::move(block_length.error()));
            }
            auto pointer = doc_in.get_file_pointer();
            if (!pointer) {
                return std::unexpected(std::move(pointer.error()));
            }
            level0_doc_end_fp_ = static_cast<int64_t>(pointer.value()) + block_length.value();

            if (index_has_freq_) {
                if (!found && !needs_pos_) {
                    if (auto seeked = doc_in.seek(skip0_end); !seeked) {
                        return seeked;
                    }
                } else {
                    auto num_impact_bytes = doc_in.read_vint();
                    if (!num_impact_bytes) {
                        return std::unexpected(std::move(num_impact_bytes.error()));
                    }
                    if (num_impact_bytes.value() < 0) {
                        return err(LuceneError::corrupt_index("negative impact length"));
                    }
                    const auto impact_bytes = static_cast<size_t>(num_impact_bytes.value());
                    if (needs_impacts_ && found) {
                        level0_serialized_impacts_.resize(impact_bytes);
                        const std::span<uint8_t> impacts(level0_serialized_impacts_);
                        if (auto read = doc_in.read_bytes(impacts); !read) {
                            return read;
                        }
                    } else {
                        if (auto skipped = doc_in.skip_bytes(static_cast<int64_t>(impact_bytes)); !skipped) {
                            return skipped;
                        }
                    }
                    if (needs_pos_) {
                        auto pos_end_delta = doc_in.read_vlong();
                        if (!pos_end_delta) {
                            return std::unexpected(std::move(pos_end_delta.error()));
                        }
                        level0_pos_end_fp_ += pos_end_delta.value();
                        auto block_pos_upto = doc_in.read_byte();
                        if (!block_pos_upto) {
                            return std::unexpected(std::move(block_pos_upto.error()));
                        }
                        level0_block_pos_upto_ = static_cast<int32_t>(block_pos_upto.value());
                        if (index_has_offsets_or_payloads_) {
                            auto pay_end_delta = doc_in.read_vlong();
                            if (!pay_end_delta) {
                                return std::unexpected(std::move(pay_end_delta.error()));
                            }
                            level0_pay_end_fp_ += pay_end_delta.value();
                            auto block_pay_upto = doc_in.read_vint();
                            if (!block_pay_upto) {
                                return std::unexpected(std::move(block_pay_upto.error()));
                            }
                            level0_block_pay_upto_ = block_pay_upto.value();
                        }
                    } else {
                        if (auto seeked = doc_in.seek(skip0_end); !seeked) {
                            return seeked;
                        }
                    }
                }
            }
            if (found) {
                break;
            }
            if (auto seeked = doc_in.seek(static_cast<size_t>(level0_doc_end_fp_)); !seeked) {
                return seeked;
            }
            doc_count_left_ -= static_cast<int32_t>(BLOCK_SIZE);
        } else {
            level0_last_doc_id_ = NO_MORE_DOCS;
            break;
        }
    }
    if (pos_in_util_.has_value()) {
        return seek_pos_data(pos_fp, pos_upto, pay_fp, pay_upto);
    }
    return {};
}

VoidResult BlockPostingsEnum::do_advance_shallow(int32_t target) {
    if (target > level1_last_doc_id_) {
        // Advance the skip data on level 1 first.
        if (auto skipped = skip_level1_to(target); !skipped) {
            return skipped;
        }
    } else if (needs_refilling_) {
        auto &doc_in = *doc_in_clone_;
        if (auto seeked = doc_in.seek(static_cast<size_t>(level0_doc_end_fp_)); !seeked) {
            return seeked;
        }
        doc_count_left_ -= static_cast<int32_t>(BLOCK_SIZE);
    }
    return skip_level0_to(target);
}

VoidResult BlockPostingsEnum::skip_positions(int32_t freq) {
    size_t to_skip = static_cast<size_t>(pos_pending_count_ - freq);
    const size_t left_in_block = BLOCK_SIZE - pos_buffer_upto_;

    if (to_skip < left_in_block) {
        const size_t end = pos_buffer_upto_ + to_skip;
        if (needs_payloads_) {
            payload_byte_upto_ += static_cast<size_t>(sum_over_range(std::span<const int32_t>(payload_length_buffer_),
                                                                     pos_buffer_upto_, end));
        }
        pos_buffer_upto_ = end;
        return {};
    }

    to_skip -= left_in_block;
    {
        auto &pos_in = *pos_in_clone_;
        while (to_skip >= BLOCK_SIZE) {
            auto pointer = pos_in.get_file_pointer();
            if (!pointer) {
                return std::unexpected(std::move(pointer.error()));
            }
            debug_check(static_cast<int64_t>(pointer.value()) != last_pos_block_fp_);
            if (auto skipped = PForUtil::skip(pos_in); !skipped) {
                return skipped;
            }

            if (pay_in_util_.has_value()) {
                auto &pay_in = *pay_in_clone_;
                if (index_has_payloads_) {
                    if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                        return skipped;
                    }
                    auto num_bytes = pay_in.read_vint();
                    if (!num_bytes) {
                        return std::unexpected(std::move(num_bytes.error()));
                    }
                    if (num_bytes.value() < 0) {
                        return err(LuceneError::corrupt_index("negative payload length"));
                    }
                    auto pay_pointer = pay_in.get_file_pointer();
                    if (!pay_pointer) {
                        return std::unexpected(std::move(pay_pointer.error()));
                    }
                    if (auto seeked = pay_in.seek(pay_pointer.value() +
                                                  static_cast<size_t>(num_bytes.value()));
                        !seeked) {
                        return seeked;
                    }
                }

                if (index_has_offsets_) {
                    if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                        return skipped;
                    }
                    if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                        return skipped;
                    }
                }
            }

            to_skip -= BLOCK_SIZE;
        }
    }
    if (auto refilled = refill_positions(); !refilled) {
        return refilled;
    }

    if (needs_payloads_) {
        payload_byte_upto_ =
                static_cast<size_t>(sum_over_range(std::span<const int32_t>(payload_length_buffer_), 0, to_skip));
    }

    pos_buffer_upto_ = to_skip;
    return {};
}

VoidResult BlockPostingsEnum::refill_last_position_block() {
    const size_t count = static_cast<size_t>(total_term_freq_ % static_cast<int64_t>(BLOCK_SIZE));
    int32_t payload_length = 0;
    int32_t offset_length = 0;
    payload_byte_upto_ = 0;

    auto &pos_in = *pos_in_clone_;
    for (size_t i = 0; i < count; ++i) {
        auto code_result = pos_in.read_vint();
        if (!code_result) {
            return std::unexpected(std::move(code_result.error()));
        }
        const int32_t code = code_result.value();
        if (index_has_payloads_) {
            if ((code & 1) != 0) {
                auto length_result = pos_in.read_vint();
                if (!length_result) {
                    return std::unexpected(std::move(length_result.error()));
                }
                payload_length = length_result.value();
            }
            if (!payload_length_buffer_.empty()) {
                payload_length_buffer_[i] = payload_length;
                pos_delta_buffer_[i] = static_cast<int32_t>(static_cast<uint32_t>(code) >> 1);

                if (payload_length != 0) {
                    if (payload_length < 0) {
                        return err(LuceneError::corrupt_index("negative payload length"));
                    }
                    const size_t length = static_cast<size_t>(payload_length);
                    const size_t need = payload_byte_upto_ + length;
                    if (payload_position_.has_value()) {
                        const auto [offset, previous_length] = payload_position_.value();
                        payload_value_.assign(payload_bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                              payload_bytes_.begin() +
                                                      static_cast<std::ptrdiff_t>(offset + previous_length));
                        payload_position_.reset();
                    }
                    if (need > payload_bytes_.size()) {
                        payload_bytes_.resize(need, 0);
                    }
                    const std::span<uint8_t> target(payload_bytes_);
                    if (auto read = pos_in.read_bytes(target.subspan(payload_byte_upto_, length)); !read) {
                        return read;
                    }
                    payload_byte_upto_ += length;
                }
            } else {
                if (auto skipped = pos_in.skip_bytes(static_cast<int64_t>(payload_length)); !skipped) {
                    return skipped;
                }
            }
        } else {
            pos_delta_buffer_[i] = code;
        }

        if (index_has_offsets_) {
            auto delta_code_result = pos_in.read_vint();
            if (!delta_code_result) {
                return std::unexpected(std::move(delta_code_result.error()));
            }
            const int32_t delta_code = delta_code_result.value();
            if ((delta_code & 1) != 0) {
                auto length_result = pos_in.read_vint();
                if (!length_result) {
                    return std::unexpected(std::move(length_result.error()));
                }
                offset_length = length_result.value();
            }

            if (!offset_start_delta_buffer_.empty()) {
                offset_start_delta_buffer_[i] = static_cast<int32_t>(static_cast<uint32_t>(delta_code) >> 1);
                offset_length_buffer_[i] = offset_length;
            }
        }
    }

    payload_byte_upto_ = 0;
    return {};
}

VoidResult BlockPostingsEnum::refill_offsets_or_payloads() {
    if (index_has_payloads_) {
        if (needs_payloads_) {
            if (!pfor_util_.has_value()) {
                return err(LuceneError::illegal_state("PFor decoder is missing"));
            }
            if (!pay_in_util_.has_value()) {
                return err(LuceneError::illegal_state("payload input is missing"));
            }
            if (auto decoded = pfor_util_->decode(*pay_in_util_, std::span<int32_t>(payload_length_buffer_));
                !decoded) {
                return decoded;
            }

            auto &pay_in = *pay_in_clone_;
            auto num_bytes = pay_in.read_vint();
            if (!num_bytes) {
                return std::unexpected(std::move(num_bytes.error()));
            }
            if (num_bytes.value() < 0) {
                return err(LuceneError::corrupt_index("negative payload block length"));
            }
            const size_t block_bytes = static_cast<size_t>(num_bytes.value());
            if (payload_position_.has_value()) {
                const auto [offset, previous_length] = payload_position_.value();
                payload_value_.assign(payload_bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                      payload_bytes_.begin() +
                                              static_cast<std::ptrdiff_t>(offset + previous_length));
                payload_position_.reset();
            }
            if (block_bytes > payload_bytes_.size()) {
                payload_bytes_.resize(block_bytes, 0);
            }
            const std::span<uint8_t> target(payload_bytes_);
            if (auto read = pay_in.read_bytes(target.first(block_bytes)); !read) {
                return read;
            }
        } else if (pay_in_util_.has_value()) {
            // This works because the first length is always written when a vint
            // block is written.
            auto &pay_in = *pay_in_clone_;
            if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                return skipped;
            }
            auto num_bytes = pay_in.read_vint();
            if (!num_bytes) {
                return std::unexpected(std::move(num_bytes.error()));
            }
            if (num_bytes.value() < 0) {
                return err(LuceneError::corrupt_index("negative payload block length"));
            }
            auto pointer = pay_in.get_file_pointer();
            if (!pointer) {
                return std::unexpected(std::move(pointer.error()));
            }
            if (auto seeked = pay_in.seek(pointer.value() + static_cast<size_t>(num_bytes.value())); !seeked) {
                return seeked;
            }
        }
        payload_byte_upto_ = 0;
    }

    if (index_has_offsets_) {
        if (needs_offsets_) {
            if (!pfor_util_.has_value()) {
                return err(LuceneError::illegal_state("PFor decoder is missing"));
            }
            if (!pay_in_util_.has_value()) {
                return err(LuceneError::illegal_state("payload input is missing"));
            }
            if (auto decoded = pfor_util_->decode(*pay_in_util_, std::span<int32_t>(offset_start_delta_buffer_));
                !decoded) {
                return decoded;
            }
            if (auto decoded = pfor_util_->decode(*pay_in_util_, std::span<int32_t>(offset_length_buffer_));
                !decoded) {
                return decoded;
            }
        } else if (pay_in_util_.has_value()) {
            auto &pay_in = *pay_in_clone_;
            if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                return skipped;
            }
            if (auto skipped = PForUtil::skip(pay_in); !skipped) {
                return skipped;
            }
        }
    }
    return {};
}

VoidResult BlockPostingsEnum::refill_positions() {
    auto pointer = pos_in_clone_->get_file_pointer();
    if (!pointer) {
        return std::unexpected(std::move(pointer.error()));
    }
    if (static_cast<int64_t>(pointer.value()) == last_pos_block_fp_) {
        return refill_last_position_block();
    }

    if (!pfor_util_.has_value()) {
        return err(LuceneError::illegal_state("PFor decoder is missing"));
    }
    if (!pos_in_util_.has_value()) {
        return err(LuceneError::illegal_state("positions input is missing"));
    }
    if (auto decoded = pfor_util_->decode(*pos_in_util_, std::span<int32_t>(pos_delta_buffer_)); !decoded) {
        return decoded;
    }

    if (index_has_offsets_or_payloads_) {
        return refill_offsets_or_payloads();
    }
    return {};
}

VoidResult BlockPostingsEnum::accumulate_pending_positions() {
    auto freq_result = freq();
    if (!freq_result) {
        return std::unexpected(std::move(freq_result.error()));
    }
    const int32_t freq = freq_result.value();

    const int32_t pending = pos_doc_buffer_upto_ + 1 == doc_buffer_upto_
                                    ? freq
                                    : sum_over_range(std::span<const int32_t>(freq_buffer_), pos_doc_buffer_upto_,
                                                     doc_buffer_upto_);
    pos_pending_count_ += pending;
    pos_doc_buffer_upto_ = doc_buffer_upto_;

    debug_check(pos_pending_count_ > 0);

    if (pos_pending_count_ > freq) {
        if (auto skipped = skip_positions(freq); !skipped) {
            return skipped;
        }
        pos_pending_count_ = freq;
    }
    return {};
}

VoidResult BlockPostingsEnum::accumulate_payload_and_offsets() {
    if (needs_payloads_) {
        const int32_t length = payload_length_buffer_[pos_buffer_upto_];
        if (length < 0) {
            return err(LuceneError::corrupt_index("negative payload length"));
        }
        payload_length_ = static_cast<size_t>(length);
        // Keep the range check before committing the current position.
        debug_check(payload_byte_upto_ + payload_length_ <= payload_bytes_.size());
        payload_position_ = std::pair<size_t, size_t>{payload_byte_upto_, payload_length_};
        payload_byte_upto_ += payload_length_;
    }

    if (needs_offsets_) {
        const size_t position = pos_buffer_upto_;
        start_offset_ = last_start_offset_ + offset_start_delta_buffer_[position];
        end_offset_ = start_offset_ + offset_length_buffer_[position];
        last_start_offset_ = start_offset_;
    }
    return {};
}

VoidResult BlockPostingsEnum::do_move_to_next_level0_block() {
    debug_check(doc_buffer_upto_ == BLOCK_SIZE);
    if (pos_in_util_.has_value()) {
        auto &pos_in = *pos_in_clone_;
        auto pos_pointer = pos_in.get_file_pointer();
        if (!pos_pointer) {
            return std::unexpected(std::move(pos_pointer.error()));
        }
        if (level0_pos_end_fp_ >= static_cast<int64_t>(pos_pointer.value())) {
            if (auto seeked = pos_in.seek(static_cast<size_t>(level0_pos_end_fp_)); !seeked) {
                return seeked;
            }
            pos_pending_count_ = level0_block_pos_upto_;
            if (pay_in_util_.has_value()) {
                auto &pay_in = *pay_in_clone_;
                auto pay_pointer = pay_in.get_file_pointer();
                if (!pay_pointer) {
                    return std::unexpected(std::move(pay_pointer.error()));
                }
                debug_check(level0_pay_end_fp_ >= static_cast<int64_t>(pay_pointer.value()));
                if (auto seeked = pay_in.seek(static_cast<size_t>(level0_pay_end_fp_)); !seeked) {
                    return seeked;
                }
                payload_byte_upto_ = static_cast<size_t>(level0_block_pay_upto_);
            }
            pos_buffer_upto_ = BLOCK_SIZE;
        } else {
            debug_check(!has_freq_fp_);
            pos_pending_count_ +=
                    sum_over_range(std::span<const int32_t>(freq_buffer_), pos_doc_buffer_upto_, BLOCK_SIZE);
        }
    }

    if (static_cast<size_t>(doc_count_left_) >= BLOCK_SIZE) {
        auto &doc_in = *doc_in_clone_;
        // The level 0 skip length; the general path re-parses its content.
        auto skip_bytes = doc_in.read_vlong();
        if (!skip_bytes) {
            return std::unexpected(std::move(skip_bytes.error()));
        }
        auto doc_delta = read_vint15(doc_in);
        if (!doc_delta) {
            return std::unexpected(std::move(doc_delta.error()));
        }
        level0_last_doc_id_ += doc_delta.value();

        auto block_length = read_vlong15(doc_in);
        if (!block_length) {
            return std::unexpected(std::move(block_length.error()));
        }
        auto pointer = doc_in.get_file_pointer();
        if (!pointer) {
            return std::unexpected(std::move(pointer.error()));
        }
        level0_doc_end_fp_ = static_cast<int64_t>(pointer.value()) + block_length.value();

        if (index_has_freq_) {
            auto num_impact_bytes = doc_in.read_vint();
            if (!num_impact_bytes) {
                return std::unexpected(std::move(num_impact_bytes.error()));
            }
            if (num_impact_bytes.value() < 0) {
                return err(LuceneError::corrupt_index("negative impact length"));
            }
            const auto impact_bytes = static_cast<size_t>(num_impact_bytes.value());
            if (needs_impacts_) {
                level0_serialized_impacts_.resize(impact_bytes);
                const std::span<uint8_t> target(level0_serialized_impacts_);
                if (auto read = doc_in.read_bytes(target); !read) {
                    return read;
                }
            } else {
                if (auto skipped = doc_in.skip_bytes(static_cast<int64_t>(impact_bytes)); !skipped) {
                    return skipped;
                }
            }
            if (index_has_pos_) {
                auto pos_end_delta = doc_in.read_vlong();
                if (!pos_end_delta) {
                    return std::unexpected(std::move(pos_end_delta.error()));
                }
                level0_pos_end_fp_ += pos_end_delta.value();
                auto block_pos_upto = doc_in.read_byte();
                if (!block_pos_upto) {
                    return std::unexpected(std::move(block_pos_upto.error()));
                }
                level0_block_pos_upto_ = static_cast<int32_t>(block_pos_upto.value());
                if (index_has_offsets_or_payloads_) {
                    auto pay_end_delta = doc_in.read_vlong();
                    if (!pay_end_delta) {
                        return std::unexpected(std::move(pay_end_delta.error()));
                    }
                    level0_pay_end_fp_ += pay_end_delta.value();
                    auto block_pay_upto = doc_in.read_vint();
                    if (!block_pay_upto) {
                        return std::unexpected(std::move(block_pay_upto.error()));
                    }
                    level0_block_pay_upto_ = block_pay_upto.value();
                }
            }
        }
        return refill_full_block();
    }

    level0_last_doc_id_ = NO_MORE_DOCS;
    return refill_remainder();
}

VoidResult BlockPostingsEnum::move_to_next_level0_block() {
    if (doc_ == level1_last_doc_id_) {
        // Advance the level 1 skip data.
        if (auto skipped = skip_level1_to(doc_ + 1); !skipped) {
            return skipped;
        }
    }
    // Now advance the level 0 skip data.
    prev_doc_id_ = level0_last_doc_id_;

    if (needs_docs_and_freqs_only_ && static_cast<size_t>(doc_count_left_) >= BLOCK_SIZE) {
        // Optimize the common path for exhaustive evaluation.
        auto &doc_in = *doc_in_clone_;
        auto level0_num_bytes = doc_in.read_vlong();
        if (!level0_num_bytes) {
            return std::unexpected(std::move(level0_num_bytes.error()));
        }
        if (auto skipped = doc_in.skip_bytes(level0_num_bytes.value()); !skipped) {
            return skipped;
        }
        if (auto refilled = refill_full_block(); !refilled) {
            return refilled;
        }
        level0_last_doc_id_ = doc_buffer_[BLOCK_SIZE - 1];
        return {};
    }
    return do_move_to_next_level0_block();
}

Result<int32_t> BlockPostingsEnum::next_doc() {
    if (doc_util() == nullptr && doc_freq_ > 1) {
        return err(LuceneError::illegal_state("documents input is missing"));
    }
    int32_t doc = 0;
    if (doc_buffer_upto_ < doc_buffer_.size() && doc_buffer_upto_ < doc_buffer_size_) {
        doc = doc_buffer_[doc_buffer_upto_];
    } else {
        if (doc_buffer_upto_ == BLOCK_SIZE) {
            if (auto moved = move_to_next_level0_block(); !moved) {
                return std::unexpected(std::move(moved.error()));
            }
        }
        doc = doc_buffer_[doc_buffer_upto_];
    }
    doc_ = doc;
    doc_buffer_upto_ += 1;
    return doc;
}

Result<int32_t> BlockPostingsEnum::advance(int32_t target) {
    if (target > level0_last_doc_id_ || needs_refilling_) {
        if (target > level0_last_doc_id_) {
            if (auto advanced = do_advance_shallow(target); !advanced) {
                return std::unexpected(std::move(advanced.error()));
            }
        }
        if (auto refilled = refill_docs(); !refilled) {
            return std::unexpected(std::move(refilled.error()));
        }
        needs_refilling_ = false;
    }
    const size_t next = find_next_geq(std::span<const int32_t>(doc_buffer_), target, doc_buffer_upto_,
                                      doc_buffer_size_);
    doc_ = doc_buffer_[next];
    doc_buffer_upto_ = next + 1;
    return doc_;
}

Result<int32_t> BlockPostingsEnum::freq() {
    if (!needs_freq_) {
        return 1;
    }
    if (has_freq_fp_) {
        if (!doc_in_util_.has_value()) {
            return err(LuceneError::illegal_state("documents input is missing"));
        }
        if (auto seeked = doc_in_clone_->seek(freq_fp_); !seeked) {
            return std::unexpected(std::move(seeked.error()));
        }
        if (!pfor_util_.has_value()) {
            return err(LuceneError::illegal_state("PFor decoder is missing"));
        }
        if (auto decoded = pfor_util_->decode(*doc_util(), std::span<int32_t>(freq_buffer_)); !decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        has_freq_fp_ = false;
    }
    return freq_buffer_[doc_buffer_upto_ - 1];
}

Result<int32_t> BlockPostingsEnum::next_position() {
    if (!needs_pos_) {
        return -1;
    }

    debug_check(pos_doc_buffer_upto_ <= doc_buffer_upto_);

    if (pos_doc_buffer_upto_ != doc_buffer_upto_) {
        if (auto accumulated = accumulate_pending_positions(); !accumulated) {
            return std::unexpected(std::move(accumulated.error()));
        }
        position_ = 0;
        last_start_offset_ = 0;
    }

    if (pos_buffer_upto_ == BLOCK_SIZE) {
        if (auto refilled = refill_positions(); !refilled) {
            return std::unexpected(std::move(refilled.error()));
        }
        pos_buffer_upto_ = 0;
    }

    position_ += pos_delta_buffer_[pos_buffer_upto_];

    if (needs_offsets_or_payloads_) {
        if (auto accumulated = accumulate_payload_and_offsets(); !accumulated) {
            return std::unexpected(std::move(accumulated.error()));
        }
    }

    pos_buffer_upto_ += 1;
    pos_pending_count_ -= 1;

    return position_;
}

Result<int32_t> BlockPostingsEnum::start_offset() const {
    if (!needs_offsets_) {
        return -1;
    }
    return start_offset_;
}

Result<int32_t> BlockPostingsEnum::end_offset() const {
    if (!needs_offsets_) {
        return -1;
    }
    return end_offset_;
}

Result<std::optional<std::span<const uint8_t>>> BlockPostingsEnum::get_payload() const {
    if (!needs_payloads_ || payload_length_ == 0) {
        return std::optional<std::span<const uint8_t>>{};
    }
    if (payload_position_.has_value()) {
        const auto [offset, length] = payload_position_.value();
        return std::optional<std::span<const uint8_t>>(
                std::span<const uint8_t>(payload_bytes_).subspan(offset, length));
    }
    return std::optional<std::span<const uint8_t>>(std::span<const uint8_t>(payload_value_));
}

VoidResult BlockPostingsEnum::advance_shallow(int32_t target) {
    if (target > level0_last_doc_id_) {
        // Advance the level 0 skip data.
        if (auto advanced = do_advance_shallow(target); !advanced) {
            return advanced;
        }

        // If we are on the last doc ID of a block and the target is the doc ID
        // just beyond this block, decode the block. This may not be necessary,
        // but it avoids having to check whether we are on an undecoded block in
        // `next_doc`.
        if (doc_buffer_upto_ == BLOCK_SIZE && target == doc_ + 1) {
            if (auto refilled = refill_docs(); !refilled) {
                return refilled;
            }
            needs_refilling_ = false;
        } else {
            needs_refilling_ = true;
        }
    }
    return {};
}

Result<std::unique_ptr<Impacts>> BlockPostingsEnum::get_impacts() const {
    debug_check(needs_impacts_);

    // The reference implementation keeps `None` when the serialized impacts were not allocated.
    std::optional<std::span<const uint8_t>> level0_serialized;
    std::optional<std::span<const uint8_t>> level1_serialized;
    if (needs_freq_ && needs_impacts_) {
        level0_serialized = std::span<const uint8_t>(level0_serialized_impacts_);
        level1_serialized = std::span<const uint8_t>(level1_serialized_impacts_);
    }

    return std::unique_ptr<Impacts>(std::make_unique<ImpactsImpl>(
            index_has_freq_, level0_last_doc_id_, level1_last_doc_id_, level0_serialized, level1_serialized,
            static_cast<size_t>(max_num_impacts_at_level0_),
            static_cast<size_t>(max_num_impacts_at_level1_)));
}

} // namespace lucene
