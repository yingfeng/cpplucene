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

module lucene_core:segment_core_readers.impl;

import :segment_core_readers;

import :codec;
import :codec_formats;
import :compound_format;
import :doc_values_format;
import :error;
import :field_infos_format;
import :index_file_names;
import :lucene101_codec;
import :norms_format;
import :points_format;
import :postings_format;
import :segment_info;
import :segment_read_state;
import :stored_fields_format;
import :term_vectors_format;
import :types;

import std;

namespace lucene {

namespace {

/// Resolves the codec of `si`, reporting an `illegal_argument` when
/// `segments_N` names one this port does not know.
Result<const Codec *> codec_of(const SegmentCommitInfo &si) {
    const Codec *codec = CodecFormats::for_name(si.info.codec_name);
    if (codec == nullptr) {
        return err(LuceneError::illegal_argument("unknown codec: " + si.info.codec_name));
    }
    return codec;
}

/// Builds the state shared by the postings, norms, points and doc values
/// readers.
SegmentReadState read_state(Directory &dir, std::shared_ptr<const FieldInfos> infos, const IOContext &context) {
    return SegmentReadState(dir, std::move(infos), context);
}

} // namespace

Result<std::shared_ptr<SegmentCoreReaders>> SegmentCoreReaders::open(Directory &dir, const SegmentCommitInfo &si,
                                                                    const IOContext &context) {
    auto codec = codec_of(si);
    if (!codec.has_value()) {
        return std::unexpected(std::move(codec.error()));
    }

    std::shared_ptr<SegmentCoreReaders> core(new SegmentCoreReaders());
    core->read_dir_ = &dir;

    // 1. The compound directory, when the segment is packed.
    if (si.info.use_compound_file) {
        const CompoundFormat *compound = codec.value()->compound_format();
        if (compound == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name +
                                                  " does not support compound files"));
        }
        auto opened = compound->get_compound_reader(dir, si.info);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        core->cfs_reader_ = std::move(opened.value());
    }

    // 2. Field infos.  A doc values update writes a new generation of them, so a
    //    segment may have several; the current one carries the doc values
    //    generation of every updated field.
    //
    //    An update is always written *outside* the compound file (the segment is
    //    packed once, at flush or merge, and never repacked), so only the base
    //    generation lives inside `_N.cfs`.
    const FieldInfosFormat &field_infos_format = codec.value()->field_infos_format();
    const std::string infos_suffix =
            si.field_infos_gen == -1 ? std::string{} : IndexFileNames::base36_encode(si.field_infos_gen);
    Directory &infos_dir = si.field_infos_gen == -1 && core->cfs_reader_ != nullptr ? *core->cfs_reader_ : dir;
    auto infos = field_infos_format.read(infos_dir, si.info, infos_suffix, context);
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    core->core_field_infos_ = std::make_shared<const FieldInfos>(std::move(infos.value()));
    const std::shared_ptr<const FieldInfos> &core_infos = core->core_field_infos_;

    // 3. Stored fields: every segment has a reader, with or without any field
    //    actually storing something.
    const StoredFieldsFormat *stored_fields = codec.value()->stored_fields_format();
    if (stored_fields == nullptr) {
        return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no stored fields format"));
    }
    auto stored = stored_fields->fields_reader(core->directory(), si.info, core_infos, context);
    if (!stored.has_value()) {
        return std::unexpected(std::move(stored.error()));
    }
    core->fields_reader_ = std::move(stored.value());

    // 4. Term vectors, only when some field stores them.
    if (core_infos->has_term_vectors()) {
        const TermVectorsFormat *vectors = codec.value()->term_vectors_format();
        if (vectors == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no term vectors format"));
        }
        auto opened = vectors->vectors_reader(core->directory(), si.info, core_infos, context);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        core->term_vectors_reader_ = std::move(opened.value());
    }

    auto state = read_state(core->directory(), core_infos, context);

    // 5. Postings.
    if (core_infos->has_postings()) {
        const PostingsFormat *postings = codec.value()->postings_format();
        if (postings == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no postings format"));
        }
        auto opened = postings->fields_producer(state, si.info);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        core->fields_ = std::move(opened.value());
    }

    // 6. Norms.
    if (core_infos->has_norms()) {
        const NormsFormat *norms = codec.value()->norms_format();
        if (norms == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no norms format"));
        }
        auto opened = norms->norms_producer(state, si.info);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        core->norms_producer_ = std::move(opened.value());
    }

    // 7. Points.
    if (core_infos->has_point_values()) {
        const PointsFormat *points = codec.value()->points_format();
        if (points == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no points format"));
        }
        auto opened = points->fields_reader(state, si.info);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        core->points_reader_ = std::move(opened.value());
    }

    // 8. Doc values.  A field may have been updated in place, which writes a new
    //    generation of just that field, so the readers are resolved per field.
    if (core_infos->has_doc_values()) {
        const DocValuesFormat *doc_values = codec.value()->doc_values_format();
        if (doc_values == nullptr) {
            return err(LuceneError::illegal_state("codec " + si.info.codec_name + " has no doc values format"));
        }
        // `dir` is the plain directory: an in-place update is written there, never
        // into the compound file the segment was packed into.
        auto generations =
                SegmentDocValues::open(core->directory(), *doc_values, si.info, core_infos, context, &dir);
        if (!generations.has_value()) {
            return std::unexpected(std::move(generations.error()));
        }
        core->segment_doc_values_ = std::move(generations.value());
        core->doc_values_producer_ =
                std::make_unique<SegmentDocValuesProducer>(core->segment_doc_values_, core_infos);
    }

    return core;
}

} // namespace lucene
