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

module lucene_core:stored_fields_writer.impl;

import :stored_fields_writer;

import std;

import :doc_id_merger;
import :doc_id_set_iterator;
import :doc_map;
import :error;
import :merge_state;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_reader;

namespace lucene {

namespace {

/// A [`StoredFieldVisitor`] that writes every field it sees to a
/// [`StoredFieldsWriter`], remapping field numbers when the merged segment
/// renumbered them.
///
/// Port of `MergeVisitor`.  **Deviation:** the reference implementation threads the writer through
/// every callback; this port stores it in the visitor, because the ported
/// [`StoredFieldVisitor`] has no writer parameter.
class MergeVisitor final : public StoredFieldVisitor {
public:
    MergeVisitor(const FieldInfos &merge_field_infos, const FieldInfos &reader_field_infos, StoredFieldsWriter &writer)
            : writer_(&writer) {
        // Fast path: when every field of the input segment has the same number
        // and name in the merged segment there is nothing to remap.
        for (const auto &info : reader_field_infos.fields()) {
            const FieldInfo *other = merge_field_infos.by_number(info.number);
            if (other == nullptr || other->name != info.name) {
                remapper_ = &merge_field_infos;
                return;
            }
        }
    }

    [[nodiscard]] Status needs_field(const FieldInfo & /*field_info*/) override { return Status::Yes; }

    [[nodiscard]] VoidResult binary_field(const FieldInfo &field_info, std::span<const uint8_t> value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_bytes(*remapped.value(), value);
    }

    [[nodiscard]] VoidResult string_field(const FieldInfo &field_info, std::string_view value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_str(*remapped.value(), value);
    }

    [[nodiscard]] VoidResult int_field(const FieldInfo &field_info, int32_t value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_i32(*remapped.value(), value);
    }

    [[nodiscard]] VoidResult long_field(const FieldInfo &field_info, int64_t value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_i64(*remapped.value(), value);
    }

    [[nodiscard]] VoidResult float_field(const FieldInfo &field_info, float value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_f32(*remapped.value(), value);
    }

    [[nodiscard]] VoidResult double_field(const FieldInfo &field_info, double value) override {
        auto remapped = remap(field_info);
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        return writer_->write_field_f64(*remapped.value(), value);
    }

private:
    [[nodiscard]] Result<const FieldInfo *> remap(const FieldInfo &field) const {
        if (remapper_ == nullptr) {
            return &field;
        }
        const FieldInfo *remapped = remapper_->by_name(field.name);
        if (remapped == nullptr) {
            return err(LuceneError::illegal_state("FieldInfo not found in remapper with field_name: " + field.name));
        }
        return remapped;
    }

    StoredFieldsWriter *writer_;
    const FieldInfos *remapper_{nullptr};
};

/// One input segment taking part in a stored fields merge.
class StoredFieldsMergeSub {
public:
    StoredFieldsMergeSub(size_t reader_index, int32_t max_doc, const DocMap &doc_map)
            : reader_index_(reader_index), max_doc_(max_doc), doc_map_(&doc_map) {}

    /// Walks the input segment's doc ids one by one; deletions are handled by
    /// [`DocIdMergerSub`].
    [[nodiscard]] Result<int32_t> next_doc() {
        ++doc_id_;
        if (doc_id_ == max_doc_) {
            return NO_MORE_DOCS;
        }
        return doc_id_;
    }

    [[nodiscard]] const DocMap &get_doc_map() const { return *doc_map_; }

    [[nodiscard]] size_t reader_index() const noexcept { return reader_index_; }
    [[nodiscard]] int32_t doc_id() const noexcept { return doc_id_; }

private:
    size_t reader_index_;
    int32_t max_doc_;
    const DocMap *doc_map_;
    int32_t doc_id_{-1};
};

} // namespace

Result<int32_t> StoredFieldsWriter::merge(MergeState &merge_state) {
    auto &readers = merge_state.stored_fields_readers;
    std::vector<DocIdMergerSub<StoredFieldsMergeSub>> subs;
    subs.reserve(readers.size());

    for (size_t i = 0; i < readers.size(); ++i) {
        auto &reader = readers[i];
        if (reader == nullptr) {
            return err(LuceneError::illegal_state("Expected Lucene90CompressingStoredFieldsReader"));
        }
        if (auto checked = reader->check_integrity(); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        subs.emplace_back(StoredFieldsMergeSub(i, merge_state.max_docs[i], *merge_state.doc_maps->at(i)));
    }

    auto merger = DocIdMerger<StoredFieldsMergeSub>::of(std::move(subs), merge_state.needs_index_sort);
    if (!merger.has_value()) {
        return std::unexpected(std::move(merger.error()));
    }

    int32_t doc_count = 0;
    while (true) {
        auto next = merger.value().next();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (!next.value().has_value()) {
            break;
        }
        auto &sub = merger.value().subs()[next.value().value()];

        if (auto started = start_document(); !started.has_value()) {
            return std::unexpected(std::move(started.error()));
        }
        auto &reader = readers[sub.sub.reader_index()];
        MergeVisitor visitor(*merge_state.merge_field_infos, *merge_state.field_infos->at(sub.sub.reader_index()),
                             *this);
        if (auto visited = reader->document_with_visitor(sub.sub.doc_id(), visitor); !visited.has_value()) {
            return std::unexpected(std::move(visited.error()));
        }
        if (auto finished = finish_document(); !finished.has_value()) {
            return std::unexpected(std::move(finished.error()));
        }
        ++doc_count;
    }

    if (auto finished = finish(doc_count); !finished.has_value()) {
        return std::unexpected(std::move(finished.error()));
    }
    return doc_count;
}

} // namespace lucene
