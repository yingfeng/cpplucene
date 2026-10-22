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

module lucene_core:standard_directory_reader.impl;

import :standard_directory_reader;

import :base_composite_reader;
import :error;
import :index_reader;
import :leaf_reader_context;
import :segment_infos;
import :segment_reader;
import :stored_fields;
import :term_vectors;
import :types;

import std;

namespace lucene {

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open(Directory &dir, const IOContext &context) {
    auto infos = SegmentInfos::read_latest(dir, context);
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    return open_infos(dir, std::move(infos.value()), context);
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_infos(Directory &dir, SegmentInfos infos,
                                                                     const IOContext &context) {
    std::shared_ptr<DirectoryReader> reader(new DirectoryReader());
    reader->dir_ = &dir;
    reader->infos_ = std::move(infos);

    // A missing commit file is a bug in the caller, not an empty index: an empty
    // index still has a `segments_N`, it just lists no segments.
    reader->segment_readers_.reserve(reader->infos_.size());
    std::vector<std::shared_ptr<IndexReader>> sub_readers;
    sub_readers.reserve(reader->infos_.size());
    for (const SegmentCommitInfo &info : reader->infos_.segments) {
        auto segment = SegmentReader::open(dir, info, reader->infos_.index_created_version_major, context);
        if (!segment.has_value()) {
            return std::unexpected(std::move(segment.error()));
        }
        sub_readers.push_back(segment.value());
        reader->segment_readers_.push_back(std::move(segment.value()));
    }

    auto base = CompositeReaderBase::create(std::move(sub_readers));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    reader->base_ = std::move(base.value());
    return reader;
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_if_changed() const {
    if (dir_ == nullptr) {
        return err(LuceneError::illegal_state("DirectoryReader was not opened from a directory"));
    }
    auto latest = SegmentInfos::read_latest(*dir_);
    if (!latest.has_value()) {
        return std::unexpected(std::move(latest.error()));
    }
    if (latest.value().version == infos_.version) {
        return std::shared_ptr<DirectoryReader>{};
    }
    return open_infos(*dir_, std::move(latest.value()));
}

Result<std::span<const LeafReaderContext>> DirectoryReader::leaves() const {
    const size_t leaves_num = segment_readers_.size();
    if (leaves_.size() != leaves_num) {
        leaves_.clear();
        leaves_.reserve(leaves_num);
        const TopParentMeta top_parent{static_cast<int32_t>(leaves_num), base_.max_doc(), identity_};
        for (size_t i = 0; i < leaves_num; ++i) {
            leaves_.emplace_back(segment_readers_[i].get(), static_cast<int32_t>(i),
                                 base_.starts()[i], top_parent);
        }
    }
    return std::span<const LeafReaderContext>(leaves_);
}

Result<std::unique_ptr<StoredFields>> DirectoryReader::stored_fields() { return base_.stored_fields(); }

Result<std::unique_ptr<TermVectors>> DirectoryReader::term_vectors() { return base_.term_vectors(); }

Result<bool> DirectoryReader::is_current() const {
    if (dir_ == nullptr) {
        return err(LuceneError::illegal_state("DirectoryReader was not opened from a directory"));
    }
    auto latest = SegmentInfos::read_latest(*dir_);
    if (!latest.has_value()) {
        return std::unexpected(std::move(latest.error()));
    }
    return latest.value().version == infos_.version;
}

} // namespace lucene
