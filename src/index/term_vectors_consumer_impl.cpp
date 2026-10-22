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

module lucene_core:term_vectors_consumer.impl;

import :term_vectors_consumer;

import :byte_block_pool;
import :debug;
import :directory;
import :error;
import :field_infos;
import :index_types;
import :int_block_pool;
import :io_context;
import :segment_info;
import :term_vectors_consumer_per_field;
import :term_vectors_format;
import :term_vectors_writer;
import :types;

import std;

namespace lucene {

TermVectorsConsumer::TermVectorsConsumer(Directory &directory, const TermVectorsFormat &format,
                                         SegmentInfo &segment_info)
        : directory_(&directory), format_(&format), segment_info_(&segment_info) {}

VoidResult TermVectorsConsumer::start_document() {
    // The reference implementation only drops the list of per-field writers of the previous document
    // here; this port lets each writer clear its own flag when it is started.
    to_flush_.clear();
    return {};
}

Result<TermVectorsConsumerPerField *> TermVectorsConsumer::add_field(std::shared_ptr<FieldInfo> field_info) {
    if (field_info->index_options == IndexOptions::None) {
        return err(LuceneError::illegal_argument(
                "cannot store term vectors for a field that is not indexed (field=\"" + field_info->name + "\")"));
    }
    // The reference implementation leaves this to the indexing chain (`FieldInfo::has_term_vectors`);
    // the field info already carries the schema, so detecting it here keeps the
    // caller from forgetting the call.
    if (field_info->has_term_vectors()) {
        has_vectors_ = true;
    }
    per_fields_.push_back(std::make_unique<TermVectorsConsumerPerField>(std::move(field_info)));
    return per_fields_.back().get();
}

VoidResult TermVectorsConsumer::init_writer() {
    if (writer_ != nullptr) {
        return {};
    }
    // The RAM estimate the reference implementation passes here needs the `Accountable` tree, which
    // this port does not have.
    auto context = IOContext::with_flush(FlushInfo(last_doc_id_, 0));
    if (!context.has_value()) {
        return std::unexpected(std::move(context.error()));
    }
    auto created = format_->vectors_writer(*directory_, *segment_info_, context.value());
    if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
    }
    writer_ = std::move(created.value());
    last_doc_id_ = 0;
    return {};
}

VoidResult TermVectorsConsumer::fill(int32_t doc_id) {
    while (last_doc_id_ < doc_id) {
        if (writer_ == nullptr) {
            return err(LuceneError::illegal_state("TermVectorsConsumer writer is not initialized"));
        }
        if (auto started = writer_->start_document(0); !started.has_value()) {
            return started;
        }
        if (auto finished = writer_->finish_document(); !finished.has_value()) {
            return finished;
        }
        last_doc_id_ += 1;
    }
    return {};
}

VoidResult TermVectorsConsumer::finish_document(int32_t doc_id, IntBlockPool &int_pool, ByteBlockPool &byte_pool,
                                                ByteBlockPool &term_byte_pool) {
    if (!has_vectors_) {
        return {};
    }

    to_flush_.clear();
    for (const auto &per_field : per_fields_) {
        if (per_field->pending_flush()) {
            to_flush_.push_back(per_field.get());
        }
    }
    // Fields are written in name order so the codec sees a stable sequence.
    std::sort(to_flush_.begin(), to_flush_.end(),
              [](const TermVectorsConsumerPerField *left, const TermVectorsConsumerPerField *right) {
                  return left->field_info()->name < right->field_info()->name;
              });

    if (auto initialized = init_writer(); !initialized.has_value()) {
        return initialized;
    }
    if (auto filled = fill(doc_id); !filled.has_value()) {
        return filled;
    }

    if (auto started = writer_->start_document(static_cast<int32_t>(to_flush_.size())); !started.has_value()) {
        return started;
    }
    for (auto *per_field : to_flush_) {
        if (auto written = per_field->finish_document(*writer_, int_pool, byte_pool, term_byte_pool);
            !written.has_value()) {
            return written;
        }
        per_field->reset(term_byte_pool);
    }
    if (auto finished = writer_->finish_document(); !finished.has_value()) {
        return finished;
    }

    debug_check(last_doc_id_ == doc_id, "the term vectors writer is out of sync with the document ids");
    last_doc_id_ += 1;
    int_pool.reset(false, false);
    byte_pool.reset(false, false);
    to_flush_.clear();
    return {};
}

VoidResult TermVectorsConsumer::flush(int32_t max_doc) {
    if (writer_ == nullptr) {
        // No document of this segment had term vectors.
        return {};
    }
    debug_check(max_doc > 0, "a term vectors writer needs at least one document");

    // At least one document of this run had term vectors.
    auto filled = fill(max_doc);
    auto finished = filled.has_value() ? writer_->finish(max_doc) : VoidResult{};
    auto closed = writer_->close();
    writer_.reset();

    if (!filled.has_value()) {
        return filled;
    }
    if (!closed.has_value()) {
        return closed;
    }
    return finished;
}

VoidResult TermVectorsConsumer::abort(IntBlockPool &int_pool, ByteBlockPool &byte_pool) {
    int_pool.reset(false, false);
    byte_pool.reset(false, false);
    if (writer_ == nullptr) {
        return {};
    }
    auto closed = writer_->close();
    writer_.reset();
    return closed;
}

} // namespace lucene
