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

module lucene_core:segment_doc_values.impl;

import :segment_doc_values;

import :directory;
import :doc_values_format;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :index_file_names;
import :index_types;
import :io_context;
import :segment_info;
import :segment_read_state;
import :types;

import std;

namespace lucene {

namespace {

/// The field infos a generation was written with: the fields whose doc values
/// live in it.
[[nodiscard]] std::shared_ptr<const FieldInfos> generation_field_infos(const FieldInfos &infos, int64_t gen) {
    std::set<std::string> names;
    for (const FieldInfo &field : infos.fields()) {
        if (field.doc_values_type != DocValuesType::None && field.doc_values_gen == gen) {
            names.insert(field.name);
        }
    }
    return std::make_shared<const FieldInfos>(infos.with_filter(names));
}

} // namespace

SegmentDocValues::SegmentDocValues(Directory &directory, Directory &updates_directory, const DocValuesFormat &format,
                                   const SegmentInfo &segment_info, std::shared_ptr<const FieldInfos> field_infos,
                                   const IOContext &context)
        : directory_(directory),
          updates_directory_(updates_directory),
          format_(format),
          segment_info_(segment_info),
          field_infos_(std::move(field_infos)),
          context_(context) {}

Result<std::shared_ptr<SegmentDocValues>> SegmentDocValues::open(Directory &directory,
                                                                const DocValuesFormat &format,
                                                                const SegmentInfo &segment_info,
                                                                std::shared_ptr<const FieldInfos> field_infos,
                                                                const IOContext &context,
                                                                Directory *updates_directory) {
    if (field_infos == nullptr) {
        return err(LuceneError::illegal_state("the field infos of the segment are not available"));
    }
    std::shared_ptr<SegmentDocValues> doc_values(new SegmentDocValues(
            directory, updates_directory == nullptr ? directory : *updates_directory, format, segment_info,
            field_infos, context));

    std::set<int64_t> generations;
    for (const FieldInfo &field : field_infos->fields()) {
        if (field.doc_values_type != DocValuesType::None) {
            generations.insert(field.doc_values_gen);
        }
    }
    for (int64_t gen : generations) {
        // The base generation has no suffix; an update generation is named after
        // its number, which is what the writer used.
        const std::string suffix = gen == -1 ? std::string{} : IndexFileNames::base36_encode(gen);
        // Only the generation the segment was written with can live inside the
        // compound file; an update is always written next to it.
        Directory &gen_directory = gen == -1 ? directory : doc_values->updates_directory_;
        SegmentReadState state(gen_directory, generation_field_infos(*field_infos, gen), context, suffix);
        auto opened = format.fields_producer(state, segment_info);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        doc_values->producers_.emplace(gen, std::shared_ptr<DocValuesProducer>(std::move(opened.value())));
    }
    return doc_values;
}

Result<std::shared_ptr<DocValuesProducer>> SegmentDocValues::get_doc_values_producer(int64_t gen) {
    const auto found = producers_.find(gen);
    if (found == producers_.end()) {
        return err(LuceneError::illegal_state("segment " + segment_info_.name + " has no doc values generation " +
                                             std::to_string(gen)));
    }
    return found->second;
}

VoidResult SegmentDocValues::check_integrity() const {
    for (const auto &[gen, producer] : producers_) {
        if (auto checked = producer->check_integrity(); !checked.has_value()) {
            return checked;
        }
    }
    return {};
}

// ---- SegmentDocValuesProducer ----

SegmentDocValuesProducer::SegmentDocValuesProducer(std::shared_ptr<SegmentDocValues> segment_doc_values,
                                                   std::shared_ptr<const FieldInfos> field_infos)
        : segment_doc_values_(std::move(segment_doc_values)), field_infos_(std::move(field_infos)) {}

Result<std::shared_ptr<DocValuesProducer>> SegmentDocValuesProducer::producer_for(const FieldInfo &field) const {
    return segment_doc_values_->get_doc_values_producer(field.doc_values_gen);
}

Result<std::unique_ptr<NumericDocValues>> SegmentDocValuesProducer::get_numeric(const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_numeric(field);
}

Result<std::unique_ptr<BinaryDocValues>> SegmentDocValuesProducer::get_binary(const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_binary(field);
}

Result<std::unique_ptr<SortedDocValues>> SegmentDocValuesProducer::get_sorted(const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_sorted(field);
}

Result<std::unique_ptr<SortedNumericDocValues>> SegmentDocValuesProducer::get_sorted_numeric(
        const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_sorted_numeric(field);
}

Result<std::unique_ptr<SortedSetDocValues>> SegmentDocValuesProducer::get_sorted_set(const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_sorted_set(field);
}

Result<std::unique_ptr<DocValuesSkipper>> SegmentDocValuesProducer::get_skipper(const FieldInfo &field) const {
    auto producer = producer_for(field);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return producer.value()->get_skipper(field);
}

VoidResult SegmentDocValuesProducer::check_integrity() const {
    return segment_doc_values_->check_integrity();
}

VoidResult SegmentDocValuesProducer::close() const {
    // The generations belong to the segment's core readers, which close them.
    return {};
}

Result<std::unique_ptr<DocValuesProducer>> SegmentDocValuesProducer::get_merge_instance() const {
    return std::unique_ptr<DocValuesProducer>(new SegmentDocValuesProducer(segment_doc_values_, field_infos_));
}

} // namespace lucene
