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

export module lucene_core:segment_core_readers;

import :types;
import std;

import :close;
import :compound_directory;
import :directory;
import :doc_values_producer;
import :error;
import :field_infos;
import :fields_producer;
import :io_context;
import :norms_producer;
import :points_reader;
import :segment_commit_info;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

/// The codec readers of one segment, shared by every reader of that segment.
///
/// Port of `SegmentCoreReaders` (`index/segment_core_readers.rs`).  Opening a
/// segment means opening one reader per file family, all over the same (possibly
/// compound) directory; this type owns them so a clone or a reopen can share
/// them instead of re-reading the files.
///
/// **Deviations** (C++ mechanics only):
///  * the reference count is gone: the type is handed out as
///    `std::shared_ptr`, which is what the reference implementation's `Arc` does;
///  * the cache-helper / closed-listener protocol is not ported (no cache in
///    this port consumes it), so there is no `get_cache_helper`;
///  * the KNN vector reader is absent — vector formats are out of scope;
///  * doc values are opened here rather than in `SegmentReader`: doc-values
///    *updates* (`si.has_field_updates()`) are not ported, so the producer never
///    has to be rebuilt per commit.
export class SegmentCoreReaders final : public CloseableRef {
public:
    ~SegmentCoreReaders() override = default;

    /// Opens every codec reader of `si` from `dir`.
    ///
    /// The codec is resolved from the name recorded in `segments_N`, and when
    /// `si.info.use_compound_file` is set the whole segment is read through a
    /// [`CompoundDirectory`].
    [[nodiscard]] static Result<std::shared_ptr<SegmentCoreReaders>> open(Directory &dir, const SegmentCommitInfo &si,
                                                                         const IOContext &context);

    /// Field infos as written when the segment was created (never updated).
    [[nodiscard]] const std::shared_ptr<const FieldInfos> &core_field_infos() const noexcept {
        return core_field_infos_;
    }

    /// Directory the segment files are read from: the compound directory when the
    /// segment is packed, otherwise the segment's own directory.
    [[nodiscard]] Directory &directory() const noexcept {
        return cfs_reader_ != nullptr ? static_cast<Directory &>(*cfs_reader_) : *read_dir_;
    }

    /// Postings reader, or null when no field is indexed.
    [[nodiscard]] FieldsProducer *fields() const noexcept { return fields_.get(); }

    /// Norms producer, or null when no field stores norms.
    [[nodiscard]] NormsProducer *norms_producer() const noexcept { return norms_producer_.get(); }

    /// Stored fields reader; never null (every segment has one).
    [[nodiscard]] StoredFieldsReader *stored_fields_reader() const noexcept { return fields_reader_.get(); }

    /// Term vectors reader, or null when no field stores term vectors.
    [[nodiscard]] TermVectorsReader *term_vectors_reader() const noexcept { return term_vectors_reader_.get(); }

    /// Points reader, or null when no field has point values.
    [[nodiscard]] PointsReader *points_reader() const noexcept { return points_reader_.get(); }

    /// Doc values producer, or null when no field has doc values.
    [[nodiscard]] DocValuesProducer *doc_values_producer() const noexcept { return doc_values_producer_.get(); }

private:
    SegmentCoreReaders() = default;

    std::shared_ptr<const FieldInfos> core_field_infos_;
    std::unique_ptr<CompoundDirectory> cfs_reader_;
    /// `cfs_reader_` when the segment is packed, otherwise the segment's own
    /// directory.  Borrowed, so it never outlives the caller's directory.
    Directory *read_dir_{nullptr};
    std::unique_ptr<FieldsProducer> fields_;
    std::unique_ptr<NormsProducer> norms_producer_;
    std::unique_ptr<StoredFieldsReader> fields_reader_;
    std::unique_ptr<TermVectorsReader> term_vectors_reader_;
    std::unique_ptr<PointsReader> points_reader_;
    std::unique_ptr<DocValuesProducer> doc_values_producer_;
};

} // namespace lucene
