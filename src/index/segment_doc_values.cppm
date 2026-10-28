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

export module lucene_core:segment_doc_values;

import :types;
import std;

import :directory;
import :doc_values_format;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :index_types;
import :io_context;
import :segment_info;

namespace lucene {

/// The doc values producers of one segment, one per generation.
///
/// Port of `SegmentDocValues` (`index/segment_doc_values.rs`).  A segment starts
/// with one generation, the one its codec wrote (`doc_values_gen == -1`); every
/// in-place doc values update writes a new generation holding the fields it
/// touched, at a new file suffix.  A field records the generation that last
/// wrote it in its [`FieldInfo::doc_values_gen`], so resolving a field is a
/// single lookup here.
///
/// Every generation a field refers to is opened eagerly, like the reference
/// implementation does in `initDocValuesProducers`: a reader keeps the files it
/// needs open, and a segment file may be deleted once the reader lets go.
export class SegmentDocValues {
public:
    /// Opens the producer of every generation the field infos refer to.
    ///
    /// `directory` is where the generation the segment was written with lives:
    /// the compound directory when the segment is packed, the plain one otherwise.
    /// An update generation is always written *outside* the compound file, so
    /// `updates_directory` (usually the plain directory) is what opens those.
    [[nodiscard]] static Result<std::shared_ptr<SegmentDocValues>> open(
            Directory &directory, const DocValuesFormat &format, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context,
            Directory *updates_directory = nullptr);

    /// The producer of generation `gen`; `-1` is the generation the segment was
    /// written with.
    [[nodiscard]] Result<std::shared_ptr<DocValuesProducer>> get_doc_values_producer(int64_t gen);

    /// Checks every generation this segment has.
    [[nodiscard]] VoidResult check_integrity() const;

private:
    SegmentDocValues(Directory &directory, Directory &updates_directory, const DocValuesFormat &format,
                     const SegmentInfo &segment_info, std::shared_ptr<const FieldInfos> field_infos,
                     const IOContext &context);

    Directory &directory_;
    Directory &updates_directory_;
    const DocValuesFormat &format_;
    const SegmentInfo &segment_info_;
    std::shared_ptr<const FieldInfos> field_infos_;
    const IOContext &context_;
    std::map<int64_t, std::shared_ptr<DocValuesProducer>> producers_;
};

/// A [`DocValuesProducer`] that dispatches every field to the producer of the
/// generation that wrote it.
///
/// Port of `SegmentDocValuesProducer`
/// (`index/segment_doc_values_producer.rs`).  This is what makes an in-place
/// update visible to readers without rewriting the segment: an updated field
/// simply reports a later generation than the fields around it.
export class SegmentDocValuesProducer final : public DocValuesProducer {
public:
    SegmentDocValuesProducer(std::shared_ptr<SegmentDocValues> segment_doc_values,
                             std::shared_ptr<const FieldInfos> field_infos);

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override;

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] VoidResult close() const override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const override;

private:
    /// The producer of `field`, which must have doc values.
    [[nodiscard]] Result<std::shared_ptr<DocValuesProducer>> producer_for(const FieldInfo &field) const;

    std::shared_ptr<SegmentDocValues> segment_doc_values_;
    std::shared_ptr<const FieldInfos> field_infos_;
};

} // namespace lucene
