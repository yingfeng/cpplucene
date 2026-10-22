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

export module lucene_core:lucene101_postings_format;

import :types;
import std;

import :error;
import :fields_consumer;
import :fields_producer;
import :for_util;
import :postings_format;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Lucene 10.1 postings format: packed integer blocks for fast decode.
///
/// Port of `Lucene101PostingsFormat`
/// (`core/codecs/lucene101/lucene101_postings_format.rs`).
///
/// Packed blocks encode integers with the same bit width; the block size
/// ([`BLOCK_SIZE`]) is fixed at 128 and skip data is interleaved on two levels:
/// level 0 skip data sits between packed blocks, level 1 skip data between
/// groups of [`LEVEL1_FACTOR`] blocks.
///
/// Files:
///  * `.tim` term dictionary, `.tip` term index, `.tmd` terms metadata
///  * `.psm` postings metadata, `.doc` frequencies and skip data
///  * `.pos` positions, `.pay` payloads and offsets
///
/// **Deviation:** the `PostingsFormat` binding is implemented in
/// `lucene101_postings_format_impl.cpp`, not here: the postings reader and
/// writer both import this module for its constants, so importing them back from
/// the interface would create a module cycle.
export class Lucene101PostingsFormat final : public PostingsFormat {
public:
    /// Extension of the postings metadata file.
    static constexpr std::string_view META_EXTENSION = "psm";
    /// Extension of the frequencies and skip data file.
    static constexpr std::string_view DOC_EXTENSION = "doc";
    /// Extension of the positions file.
    static constexpr std::string_view POS_EXTENSION = "pos";
    /// Extension of the payloads and offsets file.
    static constexpr std::string_view PAY_EXTENSION = "pay";

    /// Number of integers in a packed block (`ForUtil::BLOCK_SIZE`).
    static constexpr size_t BLOCK_SIZE = ForUtil::BLOCK_SIZE;
    /// `doc_id & BLOCK_MASK` is the in-block index.
    static constexpr size_t BLOCK_MASK = BLOCK_SIZE - 1;

    /// Number of blocks between two level 1 skip entries.
    static constexpr int32_t LEVEL1_FACTOR = 32;
    /// Number of documents covered by one level 1 skip entry.
    static constexpr int32_t LEVEL1_NUM_DOCS = LEVEL1_FACTOR * static_cast<int32_t>(BLOCK_SIZE);
    /// `doc_id & LEVEL1_MASK` is the in-group index.
    static constexpr int32_t LEVEL1_MASK = LEVEL1_NUM_DOCS - 1;

    static constexpr std::string_view TERMS_CODEC = "Lucene90PostingsWriterTerms";
    static constexpr std::string_view META_CODEC = "Lucene101PostingsWriterMeta";
    static constexpr std::string_view DOC_CODEC = "Lucene101PostingsWriterDoc";
    static constexpr std::string_view POS_CODEC = "Lucene101PostingsWriterPos";
    static constexpr std::string_view PAY_CODEC = "Lucene101PostingsWriterPay";

    /// Only the current version is written and read; older headers are rejected
    /// rather than decoded.
    static constexpr int32_t VERSION_START = 0;
    static constexpr int32_t VERSION_CURRENT = VERSION_START;

    /// Smallest number of terms in a block (Lucene's
    /// `BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE`).
    static constexpr int32_t DEFAULT_MIN_TERM_BLOCK_SIZE = 25;
    /// Largest number of terms in a block (Lucene's
    /// `BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE`).
    static constexpr int32_t DEFAULT_MAX_TERM_BLOCK_SIZE = 48;

    /// The format name recorded in the codec metadata.
    static constexpr std::string_view FORMAT_NAME = "Lucene101";

    /// Creates the format with Lucene's default block sizes.
    Lucene101PostingsFormat() = default;

    /// Creates the format with explicit block sizes.
    explicit Lucene101PostingsFormat(int32_t min_term_block_size, int32_t max_term_block_size)
            : min_term_block_size_(min_term_block_size), max_term_block_size_(max_term_block_size) {}

    [[nodiscard]] std::string_view name() const override { return FORMAT_NAME; }

    /// Writes the terms dictionary and the postings of a new segment.
    [[nodiscard]] Result<std::unique_ptr<FieldsConsumer>> fields_consumer(
            const SegmentWriteState &state, const SegmentInfo &segment_info) const override;

    /// Reads the terms dictionary and the postings of an existing segment.
    [[nodiscard]] Result<std::unique_ptr<FieldsProducer>> fields_producer(
            const SegmentReadState &state, const SegmentInfo &segment_info) const override;

    [[nodiscard]] int32_t min_term_block_size() const noexcept { return min_term_block_size_; }
    [[nodiscard]] int32_t max_term_block_size() const noexcept { return max_term_block_size_; }

    /// `<segment>_<suffix>.psm` and friends, for `extension` without the dot.
    [[nodiscard]] static std::string file_name(std::string_view segment, std::string_view suffix,
                                               std::string_view extension) {
        return segment_file_name(segment, suffix, extension);
    }

private:
    int32_t min_term_block_size_{DEFAULT_MIN_TERM_BLOCK_SIZE};
    int32_t max_term_block_size_{DEFAULT_MAX_TERM_BLOCK_SIZE};
};

} // namespace lucene
