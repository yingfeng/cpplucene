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

export module lucene_core:lucene90_doc_values_format;

import :types;
import std;

import :doc_values_consumer;
import :doc_values_format;
import :doc_values_producer;
import :error;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Lucene 9.0 doc values format: the per-document values of a field are stored
/// densely, indexed by the ordinal of the document among those that have a value.
///
/// Port of `Lucene90DocValuesFormat`
/// (`codecs/lucene90/lucene90_doc_values_format.rs`).  The doc-id set of the
/// documents that have a value is an [`IndexedDISI`]: when every document has a
/// value it is implicit, otherwise it is encoded in blocks of 65536 doc ids as
/// SPARSE, DENSE or ALL.
///
/// The five value types are encoded as:
///
///  * **numeric** — delta compressed with bit packing, or a lookup table when
///    there are at most 256 distinct values, or GCD compressed, or monotonic
///    compressed for increasing offsets, or constant.  Values may be split into
///    blocks of 16384 with a jump table appended;
///  * **binary** — fixed width, variable width (end addresses as monotonic
///    numerics) or prefix compressed in chunks of 16;
///  * **sorted** — a prefix-compressed term dictionary plus numeric ordinals;
///  * **sorted set** — like sorted, plus an ordinal list and a per-document
///    offset into it; a single-valued field is stored exactly like sorted;
///  * **sorted numeric** — a value list plus a per-document offset; a
///    single-valued field is stored exactly like numeric.
///
/// Files: `.dvd` holds the values, `.dvm` the metadata.
export class Lucene90DocValuesFormat final : public DocValuesFormat {
public:
    /// The name the codec metadata uses to select this format.
    static constexpr std::string_view NAME = "Lucene90";

    static constexpr std::string_view DATA_CODEC = "Lucene90DocValuesData";
    static constexpr std::string_view DATA_EXTENSION = "dvd";
    static constexpr std::string_view META_CODEC = "Lucene90DocValuesMetadata";
    static constexpr std::string_view META_EXTENSION = "dvm";

    /// First version this format ever wrote.
    static constexpr int32_t VERSION_START = 0;
    /// Version this port writes and reads (only the current one, by design).
    static constexpr int32_t VERSION_CURRENT = VERSION_START;

    /// Type byte of a numeric field.
    static constexpr uint8_t NUMERIC = 0;
    /// Type byte of a binary field.
    static constexpr uint8_t BINARY = 1;
    /// Type byte of a sorted field.
    static constexpr uint8_t SORTED = 2;
    /// Type byte of a sorted set field.
    static constexpr uint8_t SORTED_SET = 3;
    /// Type byte of a sorted numeric field.
    static constexpr uint8_t SORTED_NUMERIC = 4;

    /// Block shift of the monotonic encoders (`65536` values per block).
    static constexpr int32_t DIRECT_MONOTONIC_BLOCK_SHIFT = 16;

    /// Block shift of the numeric values (`16384` values per block).
    static constexpr int32_t NUMERIC_BLOCK_SHIFT = 14;
    /// Values per numeric block.
    static constexpr int32_t NUMERIC_BLOCK_SIZE = 1 << NUMERIC_BLOCK_SHIFT;

    /// Terms of a dictionary block are LZ4 compressed in chunks of 64.
    static constexpr int32_t TERMS_DICT_BLOCK_LZ4_SHIFT = 6;
    static constexpr int32_t TERMS_DICT_BLOCK_LZ4_SIZE = 1 << TERMS_DICT_BLOCK_LZ4_SHIFT;
    static constexpr int32_t TERMS_DICT_BLOCK_LZ4_MASK = TERMS_DICT_BLOCK_LZ4_SIZE - 1;

    /// The reverse index holds one term per 1024.
    static constexpr int32_t TERMS_DICT_REVERSE_INDEX_SHIFT = 10;
    static constexpr int32_t TERMS_DICT_REVERSE_INDEX_SIZE = 1 << TERMS_DICT_REVERSE_INDEX_SHIFT;
    static constexpr int32_t TERMS_DICT_REVERSE_INDEX_MASK = TERMS_DICT_REVERSE_INDEX_SIZE - 1;

    /// Documents per skip-index interval.
    static constexpr int32_t DEFAULT_SKIP_INDEX_INTERVAL_SIZE = 4096;

    /// Bytes of one skip-index interval: 1 for the level count, 16 for the value
    /// bounds, 8 for the doc-id bounds and 4 for the document count.
    static constexpr int64_t SKIP_INDEX_INTERVAL_BYTES = 29;

    /// `max_accumulators` is `1 << (SKIP_INDEX_LEVEL_SHIFT * (SKIP_INDEX_MAX_LEVEL - 1))`,
    /// i.e. eight intervals per parent interval.
    static constexpr size_t SKIP_INDEX_LEVEL_SHIFT = 3;

    /// Number of skip-index levels.  Raising it raises the heap needed at index
    /// time: `8^3 == 512` accumulators.
    static constexpr size_t SKIP_INDEX_MAX_LEVEL = 4;

    [[nodiscard]] std::string_view name() const override { return NAME; }

    /// Defined in `lucene90_doc_values_format_impl.cpp`: the consumer and the
    /// producer both import this module for its constants, so their factories
    /// cannot live in the interface unit without creating an import cycle.
    [[nodiscard]] Result<std::unique_ptr<DocValuesConsumer>> fields_consumer(
            const SegmentWriteState &state, const SegmentInfo &segment_info) override;

    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> fields_producer(
            const SegmentReadState &state, const SegmentInfo &segment_info) override;
};

} // namespace lucene
