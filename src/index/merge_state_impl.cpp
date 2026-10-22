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

module lucene_core:merge_state.impl;

import :merge_state;

import std;

import :error;
import :field_infos;
import :info_stream;
import :packed_long_values;

namespace lucene {

namespace {

/// Takes a merge instance when the reader offers one, and a non-owning alias
/// otherwise.  Mirrors the reference implementation's
/// `if let Some(merge) = reader.get_merge_instance() { Some(merge) } else { Some(reader) }`.
template <class T>
[[nodiscard]] Result<std::shared_ptr<T>> merge_instance_or_borrow(T *reader) {
    if (reader == nullptr) {
        return std::shared_ptr<T>{};
    }
    auto instance = reader->get_merge_instance();
    if (!instance.has_value()) {
        return std::unexpected(std::move(instance.error()));
    }
    if (instance.value() != nullptr) {
        return std::shared_ptr<T>(std::move(instance.value()));
    }
    return merge_state_borrow(reader);
}

} // namespace

MergeStateDocMap::MergeStateDocMap(std::shared_ptr<const Bits> live_docs,
                                   std::shared_ptr<const PackedLongValues> del_doc_map, int32_t doc_base)
        : live_docs_(std::move(live_docs)), del_doc_map_(std::move(del_doc_map)), doc_base_(doc_base) {}

std::shared_ptr<MergeStateDocMap> MergeStateDocMap::new_deletions(std::shared_ptr<const Bits> live_docs,
                                                                  std::shared_ptr<const PackedLongValues> del_doc_map,
                                                                  int32_t doc_base) {
    return std::shared_ptr<MergeStateDocMap>(
            new MergeStateDocMap(std::move(live_docs), std::move(del_doc_map), doc_base));
}

Result<int32_t> MergeStateDocMap::get(int32_t doc_id) const {
    const auto doc_index = static_cast<size_t>(doc_id);
    if (live_docs_ == nullptr && del_doc_map_ == nullptr) {
        // No deletions: the mapping is a plain rebase.
        return doc_base_ + doc_id;
    }
    if (live_docs_ == nullptr || del_doc_map_ == nullptr) {
        return err(LuceneError::illegal_state("should not be here"));
    }
    auto live = live_docs_->get(doc_index);
    if (!live.has_value()) {
        return std::unexpected(std::move(live.error()));
    }
    if (!live.value()) {
        return -1;
    }
    auto mapped = del_doc_map_->get(doc_index);
    if (!mapped.has_value()) {
        return std::unexpected(std::move(mapped.error()));
    }
    return doc_base_ + static_cast<int32_t>(mapped.value());
}

Result<std::shared_ptr<const PackedLongValues>> remove_deletes(int32_t max_doc, const Bits &live_docs) {
    // The values are monotonic and non-negative, so the min-based (delta) page
    // layout compresses them best -- the reference implementation's `PackedInts::COMPACT`.
    PackedLongValues::Builder builder(PackedLongValues::DEFAULT_PAGE_SIZE, 64, 0, true);

    int32_t deleted = 0;
    for (int32_t i = 0; i < max_doc; ++i) {
        if (auto added = builder.add(i - deleted); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        auto live = live_docs.get(static_cast<size_t>(i));
        if (!live.has_value()) {
            return std::unexpected(std::move(live.error()));
        }
        if (!live.value()) {
            ++deleted;
        }
    }

    auto built = builder.build();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::make_shared<const PackedLongValues>(std::move(built.value()));
}

MergeState::~MergeState() = default;

MergeState::MergeState(SegmentInfo &segment_info_, std::shared_ptr<InfoStreamEnum> info_stream_)
        : segment_info(segment_info_),
          doc_maps(std::make_shared<DocMaps>()),
          merge_field_infos(std::make_shared<const FieldInfos>(FieldInfos::none())),
          field_infos(std::make_shared<const FieldInfosList>()),
          info_stream(std::move(info_stream_)) {}

Result<std::unique_ptr<MergeState>> MergeState::create(std::span<CodecReader *const> readers, SegmentInfo &segment_info,
                                                       std::shared_ptr<InfoStreamEnum> info_stream) {
    std::unique_ptr<MergeState> state{new MergeState(segment_info, std::move(info_stream))};

    const auto num_readers = readers.size();
    state->stored_fields_readers.reserve(num_readers);
    state->term_vectors_readers.reserve(num_readers);
    state->norms_producers.reserve(num_readers);
    state->doc_values_producers.reserve(num_readers);
    state->fields_producers.reserve(num_readers);
    state->points_readers.reserve(num_readers);
    state->max_docs.reserve(num_readers);
    state->live_docs.reserve(num_readers);

    auto infos = std::make_shared<FieldInfosList>();
    infos->reserve(num_readers);

    int32_t num_docs = 0;
    for (auto *reader : readers) {
        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        state->max_docs.push_back(max_doc.value());

        auto reader_live_docs = reader->live_docs();
        if (!reader_live_docs.has_value()) {
            return std::unexpected(std::move(reader_live_docs.error()));
        }
        state->live_docs.push_back(merge_state_borrow(reader_live_docs.value()));

        auto reader_field_infos = reader->field_infos();
        if (!reader_field_infos.has_value()) {
            return std::unexpected(std::move(reader_field_infos.error()));
        }
        infos->push_back(merge_state_borrow(reader_field_infos.value()));

        auto stored = merge_instance_or_borrow(reader->get_fields_reader());
        if (!stored.has_value()) {
            return std::unexpected(std::move(stored.error()));
        }
        state->stored_fields_readers.push_back(std::move(stored.value()));

        auto vectors = merge_instance_or_borrow(reader->get_term_vectors_reader());
        if (!vectors.has_value()) {
            return std::unexpected(std::move(vectors.error()));
        }
        state->term_vectors_readers.push_back(std::move(vectors.value()));

        auto norms = merge_instance_or_borrow(reader->get_norms_reader());
        if (!norms.has_value()) {
            return std::unexpected(std::move(norms.error()));
        }
        state->norms_producers.push_back(std::move(norms.value()));

        auto doc_values = merge_instance_or_borrow(reader->get_doc_values_reader());
        if (!doc_values.has_value()) {
            return std::unexpected(std::move(doc_values.error()));
        }
        state->doc_values_producers.push_back(std::move(doc_values.value()));

        auto postings = merge_instance_or_borrow(reader->get_postings_reader());
        if (!postings.has_value()) {
            return std::unexpected(std::move(postings.error()));
        }
        state->fields_producers.push_back(std::move(postings.value()));

        // `PointsReader` has no merge instance in this port (the reference implementation's does not
        // either: it is borrowed as is).
        state->points_readers.push_back(merge_state_borrow(reader->get_points_reader()));

        auto reader_num_docs = reader->num_docs();
        if (!reader_num_docs.has_value()) {
            return std::unexpected(std::move(reader_num_docs.error()));
        }
        num_docs += reader_num_docs.value();
    }

    state->field_infos = std::move(infos);
    state->segment_info.max_doc = num_docs;

    // No index sort in this port, so only the deletion mapping is needed.
    auto maps = std::make_shared<DocMaps>();
    maps->reserve(num_readers);

    int32_t total_docs = 0;
    for (auto *reader : readers) {
        const Bits *bits = nullptr;
        auto reader_live_docs = reader->live_docs();
        if (!reader_live_docs.has_value()) {
            return std::unexpected(std::move(reader_live_docs.error()));
        }
        bits = reader_live_docs.value();

        std::shared_ptr<const PackedLongValues> del_doc_map;
        std::shared_ptr<const Bits> live;
        if (bits != nullptr) {
            auto max_doc = reader->max_doc();
            if (!max_doc.has_value()) {
                return std::unexpected(std::move(max_doc.error()));
            }
            auto removed = remove_deletes(max_doc.value(), *bits);
            if (!removed.has_value()) {
                return std::unexpected(std::move(removed.error()));
            }
            del_doc_map = std::move(removed.value());
            live = merge_state_borrow(bits);
        }

        maps->push_back(MergeStateDocMap::new_deletions(std::move(live), std::move(del_doc_map), total_docs));

        auto reader_num_docs = reader->num_docs();
        if (!reader_num_docs.has_value()) {
            return std::unexpected(std::move(reader_num_docs.error()));
        }
        total_docs += reader_num_docs.value();
    }

    state->doc_maps = std::move(maps);
    return state;
}

MergeStateMeta MergeState::get_meta() const {
    MergeStateMeta meta;
    meta.fields_producers_len = fields_producers.size();
    meta.doc_maps = doc_maps;
    meta.needs_index_sort = needs_index_sort;
    meta.merge_field_infos = merge_field_infos;
    meta.field_infos = field_infos;
    return meta;
}

} // namespace lucene
