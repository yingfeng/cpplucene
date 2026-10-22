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

module lucene_core:binary_doc_values_writer.impl;

import :binary_doc_values_writer;

import :array_util;
import :binary_doc_values;
import :bytes_ref;
import :doc_id_set_iterator;
import :doc_values_consumer;
import :doc_values_producer;
import :doc_values_writer;
import :docs_with_field_set;
import :error;
import :field_infos;
import :types;

import std;

namespace lucene {

namespace {

/// Reads the value that belongs to the document just returned by the
/// docs-with-field iterator.
[[nodiscard]] VoidResult take_next_value(const std::vector<BytesRef> &values, size_t &index, BytesRef &target) {
    if (index >= values.size()) {
        return err(LuceneError::illegal_state("the buffered binary values ran out of values"));
    }
    target = values[index];
    index += 1;
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// BufferedBinaryDocValues
// ---------------------------------------------------------------------------

BufferedBinaryDocValues::BufferedBinaryDocValues(std::shared_ptr<const std::vector<BytesRef>> values,
                                                 std::unique_ptr<DocIdSetIterator> docs_with_field)
        : values_(std::move(values)), docs_with_field_(std::move(docs_with_field)) {}

Result<int32_t> BufferedBinaryDocValues::next_doc() {
    auto doc = docs_with_field_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto taken = take_next_value(*values_, index_, value_); !taken.has_value()) {
            return std::unexpected(std::move(taken.error()));
        }
    }
    return doc.value();
}

Result<int32_t> BufferedBinaryDocValues::advance(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto taken = take_next_value(*values_, index_, value_); !taken.has_value()) {
            return std::unexpected(std::move(taken.error()));
        }
    }
    return doc.value();
}

Result<int64_t> BufferedBinaryDocValues::cost() const { return docs_with_field_->cost(); }

Result<bool> BufferedBinaryDocValues::advance_exact(int32_t target) {
    auto doc = docs_with_field_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != target) {
        return false;
    }
    if (auto taken = take_next_value(*values_, index_, value_); !taken.has_value()) {
        return std::unexpected(std::move(taken.error()));
    }
    return true;
}

Result<BytesRef> BufferedBinaryDocValues::binary_value() { return value_; }

// ---------------------------------------------------------------------------
// BinaryDocValuesProducer
// ---------------------------------------------------------------------------

BinaryDocValuesProducer::BinaryDocValuesProducer(std::shared_ptr<FieldInfo> field_info,
                                                 std::shared_ptr<const std::vector<BytesRef>> values,
                                                 const DocsWithFieldSet &docs_with_field)
        : field_info_(std::move(field_info)), values_(std::move(values)), docs_with_field_(&docs_with_field) {}

Result<std::unique_ptr<BinaryDocValues>> BinaryDocValuesProducer::get_binary(const FieldInfo &field) const {
    if (field.name != field_info_->name) {
        return err(LuceneError::illegal_argument("wrong fieldInfo: expected \"" + field_info_->name + "\", got \"" +
                                                field.name + "\""));
    }
    auto docs = docs_with_field_->iterator();
    if (!docs.has_value()) {
        return std::unexpected(std::move(docs.error()));
    }
    return std::unique_ptr<BinaryDocValues>(
            std::make_unique<BufferedBinaryDocValues>(values_, std::move(docs.value())));
}

// ---------------------------------------------------------------------------
// BinaryDocValuesWriter
// ---------------------------------------------------------------------------

BinaryDocValuesWriter::BinaryDocValuesWriter(std::shared_ptr<FieldInfo> field_info)
        : field_info_(std::move(field_info)) {}

VoidResult BinaryDocValuesWriter::add_value(int32_t doc_id, std::span<const uint8_t> value) {
    if (doc_id <= last_doc_id_) {
        return err(LuceneError::illegal_argument("DocValuesField \"" + field_info_->name +
                                                 "\" appears more than once in this document (only one value is "
                                                 "allowed per field)"));
    }
    if (value.size() > ArrayUtil::MAX_ARRAY_LENGTH) {
        return err(LuceneError::illegal_argument("DocValuesField \"" + field_info_->name +
                                                 "\" is too large, must be <= " +
                                                 std::to_string(ArrayUtil::MAX_ARRAY_LENGTH)));
    }

    max_length_ = std::max(max_length_, value.size());
    values_.push_back(BytesRef(std::vector<uint8_t>(value.begin(), value.end())));
    if (auto added = docs_with_field_.add(doc_id); !added.has_value()) {
        return added;
    }
    last_doc_id_ = doc_id;
    return {};
}

VoidResult BinaryDocValuesWriter::finish() {
    if (final_values_ == nullptr) {
        final_values_ = std::make_shared<const std::vector<BytesRef>>(values_);
    }
    return docs_with_field_.finish();
}

VoidResult BinaryDocValuesWriter::flush(DocValuesConsumer &consumer) {
    if (final_values_ == nullptr) {
        if (auto finished = finish(); !finished.has_value()) {
            return finished;
        }
    }
    BinaryDocValuesProducer producer(field_info_, final_values_, docs_with_field_);
    return consumer.add_binary_field(*field_info_, producer);
}

} // namespace lucene
