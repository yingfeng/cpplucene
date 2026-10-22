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

module lucene_core:lucene101_postings_format.impl;

import :lucene101_postings_format;

import :block_tree_terms_reader;
import :block_tree_terms_writer;
import :error;
import :fields_consumer;
import :fields_producer;
import :lucene101_postings_reader;
import :lucene101_postings_writer;
import :postings_reader_base;
import :postings_writer_base;
import :push_postings_writer_base;
import :segment_info;
import :segment_read_state;
import :segment_write_state;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<FieldsConsumer>> Lucene101PostingsFormat::fields_consumer(
        const SegmentWriteState &state, const SegmentInfo &segment_info) const {
    auto postings_writer = Lucene101PostingsWriter::create(state, segment_info);
    if (!postings_writer.has_value()) {
        return std::unexpected(std::move(postings_writer.error()));
    }

    // The block tree wraps the codec specific writer, so that the terms dict
    // drives the postings writer.
    auto writer = BlockTreeTermsWriter::create(state,
                                               std::make_unique<PushPostingsWriterBase>(std::move(postings_writer.value())),
                                               min_term_block_size_, max_term_block_size_, segment_info);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    std::unique_ptr<FieldsConsumer> result = std::move(writer.value());
    return result;
}

Result<std::unique_ptr<FieldsProducer>> Lucene101PostingsFormat::fields_producer(
        const SegmentReadState &state, const SegmentInfo &segment_info) const {
    auto postings_reader = Lucene101PostingsReader::create(state, segment_info);
    if (!postings_reader.has_value()) {
        return std::unexpected(std::move(postings_reader.error()));
    }
    std::unique_ptr<PostingsReaderBase> base = std::move(postings_reader.value());

    // `create` mutates the read state (it records the opened files).
    auto mutable_state = state;
    auto reader = Lucene90BlockTreeTermsReader::create(std::move(base), mutable_state, segment_info);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    std::unique_ptr<FieldsProducer> result = std::move(reader.value());
    return result;
}

} // namespace lucene
