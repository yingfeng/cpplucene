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

module lucene_core:stored_fields_consumer.impl;

import :stored_fields_consumer;

import :codec;
import :debug;
import :directory;
import :document;
import :error;
import :field_infos;
import :io_context;
import :number;
import :segment_info;
import :stored_fields_format;
import :stored_fields_writer;
import :types;

import std;

namespace lucene {

StoredFieldsConsumer::StoredFieldsConsumer(Directory &directory, const StoredFieldsFormat &format,
                                           SegmentInfo &segment_info)
        : directory_(&directory), format_(&format), segment_info_(&segment_info) {}

VoidResult StoredFieldsConsumer::init_writer() {
    if (writer_ == nullptr) {
        auto created = format_->fields_writer(*directory_, *segment_info_, IOContext{});
        if (!created.has_value()) {
            return std::unexpected(std::move(created.error()));
        }
        writer_ = std::move(created.value());
    }
    return {};
}

VoidResult StoredFieldsConsumer::start_document(int32_t doc_id) {
    debug_check(last_doc_ < doc_id, "the document ids must be increasing");
    if (auto initialized = init_writer(); !initialized.has_value()) {
        return initialized;
    }
    while (last_doc_ + 1 < doc_id) {
        last_doc_ += 1;
        if (auto started = writer_->start_document(); !started.has_value()) {
            return started;
        }
        if (auto finished = writer_->finish_document(); !finished.has_value()) {
            return finished;
        }
    }
    last_doc_ += 1;
    return writer_->start_document();
}

VoidResult StoredFieldsConsumer::write_field(const FieldInfo &field_info, const FieldValue &value) {
    if (writer_ == nullptr) {
        return err(LuceneError::illegal_state("writer must be initialized"));
    }
    if (const auto *text = std::get_if<std::string>(&value); text != nullptr) {
        return writer_->write_field_str(field_info, *text);
    }
    if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&value); bytes != nullptr) {
        return writer_->write_field_bytes(field_info, std::span<const uint8_t>(*bytes));
    }

    // The reference implementation only accepts the four widest number types here; the narrower ones
    // must be widened by the caller (`FieldType` does that when the value is
    // added to the document).
    return std::visit(
            [&](const auto &number) -> VoidResult {
                using T = std::decay_t<decltype(number)>;
                if constexpr (std::is_same_v<T, int32_t>) {
                    return writer_->write_field_i32(field_info, number);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    return writer_->write_field_i64(field_info, number);
                } else if constexpr (std::is_same_v<T, float>) {
                    return writer_->write_field_f32(field_info, number);
                } else if constexpr (std::is_same_v<T, double>) {
                    return writer_->write_field_f64(field_info, number);
                } else {
                    return err(LuceneError::illegal_argument("unsupported number type"));
                }
            },
            std::get<Number>(value));
}

VoidResult StoredFieldsConsumer::finish_document() {
    if (writer_ == nullptr) {
        return err(LuceneError::illegal_state("writer must be initialized"));
    }
    return writer_->finish_document();
}

VoidResult StoredFieldsConsumer::finish(int32_t max_doc) {
    while (last_doc_ < max_doc - 1) {
        if (auto started = start_document(last_doc_ + 1); !started.has_value()) {
            return started;
        }
        if (auto finished = finish_document(); !finished.has_value()) {
            return finished;
        }
    }
    return {};
}

VoidResult StoredFieldsConsumer::flush(int32_t max_doc) {
    if (auto finished = finish(max_doc); !finished.has_value()) {
        return finished;
    }
    if (writer_ == nullptr) {
        // No document was ever stored: nothing to flush.
        return {};
    }
    if (auto finished = writer_->finish(max_doc); !finished.has_value()) {
        return finished;
    }
    return writer_->close();
}

VoidResult StoredFieldsConsumer::abort() {
    if (writer_ == nullptr) {
        return {};
    }
    auto closed = writer_->close();
    writer_.reset();
    return closed;
}

} // namespace lucene
