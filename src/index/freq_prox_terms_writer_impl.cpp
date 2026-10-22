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

module lucene_core:freq_prox_terms_writer.impl;

import :freq_prox_terms_writer;

import :byte_block_pool;
import :error;
import :field_infos;
import :freq_prox_terms_writer_per_field;
import :int_block_pool;
import :term_vectors_consumer;
import :terms_hash;
import :types;

import std;

namespace lucene {

FreqProxTermsWriter::FreqProxTermsWriter(std::unique_ptr<TermVectorsConsumer> next_terms_hash)
        : next_terms_hash_(std::move(next_terms_hash)) {}

Result<FreqProxTermsWriterPerField> FreqProxTermsWriter::add_field(std::shared_ptr<FieldInfo> field_info) {
    // The term-vectors consumer creates the writer of the second hash, which the
    // postings writer chains to; `has_term_vectors` is what later decides
    // whether the vectors are actually written.
    TermVectorsPerField *next_per_field = nullptr;
    if (next_terms_hash_ != nullptr) {
        auto created = next_terms_hash_->add_field(field_info);
        if (!created.has_value()) {
            return std::unexpected(std::move(created.error()));
        }
        next_per_field = created.value();
    }
    return FreqProxTermsWriterPerField(std::move(field_info), next_per_field);
}

VoidResult FreqProxTermsWriter::start_document() {
    if (next_terms_hash_ != nullptr) {
        return next_terms_hash_->start_document();
    }
    return {};
}

VoidResult FreqProxTermsWriter::finish_document(int32_t doc_id, IntBlockPool &int_pool,
                                                ByteBlockPool &next_byte_pool, ByteBlockPool &byte_pool) {
    if (next_terms_hash_ == nullptr) {
        return {};
    }
    // `int_pool` / `next_byte_pool` are the term-vectors pools, `byte_pool` is
    // the one shared with the postings.
    return next_terms_hash_->finish_document(doc_id, int_pool, next_byte_pool, byte_pool);
}

VoidResult FreqProxTermsWriter::abort(IntBlockPool &int_pool, ByteBlockPool &byte_pool,
                                      IntBlockPool &next_int_pool, ByteBlockPool &next_byte_pool) {
    int_pool.reset(false, false);
    byte_pool.reset(false, false);
    if (next_terms_hash_ != nullptr) {
        return next_terms_hash_->abort(next_int_pool, next_byte_pool);
    }
    next_int_pool.reset(false, false);
    next_byte_pool.reset(false, false);
    return {};
}

Result<std::vector<std::shared_ptr<FreqProxTermsWriterPerField>>> FreqProxTermsWriter::prepare_fields(
        std::vector<FreqProxTermsWriterPerField> fields_to_flush, const ByteBlockPool &byte_pool) {
    std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> all_fields;
    all_fields.reserve(fields_to_flush.size());
    for (auto &per_field : fields_to_flush) {
        if (per_field.base().get_num_terms() == 0) {
            continue;
        }
        if (auto sorted = per_field.base().sort_terms(byte_pool); !sorted.has_value()) {
            return std::unexpected(std::move(sorted.error()));
        }
        all_fields.push_back(std::make_shared<FreqProxTermsWriterPerField>(std::move(per_field)));
    }

    // Fields are written in name order so the postings consumer sees a stable
    // sequence.
    std::sort(all_fields.begin(), all_fields.end(),
              [](const auto &left, const auto &right) { return left->compare(*right) < 0; });
    return all_fields;
}

} // namespace lucene
