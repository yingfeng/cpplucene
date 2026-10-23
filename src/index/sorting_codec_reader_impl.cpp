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

module lucene_core:sorting_codec_reader.impl;

import :sorting_codec_reader;

import std;

import :bits;
import :codec_reader;
import :doc_values_producer;
import :error;
import :field_infos;
import :fields_producer;
import :index_sorter;
import :norms_producer;
import :point_values;
import :points_reader;
import :sorting_field_readers;
import :sorting_points;
import :sorting_producers;
import :sorting_terms;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

namespace {

/// A `shared_ptr` to an object the codec reader owns, which keeps the codec
/// reader alive instead of deleting the object: the wrappers hold their
/// delegate that way, so a wrapper may outlive the sorting reader that built
/// it.
template <class T, class Owner>
[[nodiscard]] std::shared_ptr<T> borrow(T *pointer, std::shared_ptr<Owner> owner) {
    if (pointer == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<T>(pointer, [keeper = std::move(owner)](T *) {});
}

} // namespace

SortingBits::SortingBits(std::shared_ptr<const Bits> delegate, std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), doc_map_(std::move(doc_map)) {}

std::unique_ptr<SortingBits> SortingBits::create(std::shared_ptr<const Bits> delegate,
                                                std::shared_ptr<const IndexSortDocMap> doc_map) {
    return std::unique_ptr<SortingBits>(new SortingBits(std::move(delegate), std::move(doc_map)));
}

Result<bool> SortingBits::get(size_t index) const {
    auto old_doc = doc_map_->new_to_old(static_cast<int32_t>(index));
    if (!old_doc) {
        return std::unexpected(std::move(old_doc.error()));
    }
    if (old_doc.value() < 0) {
        return false;
    }
    return delegate_->get(static_cast<size_t>(old_doc.value()));
}

size_t SortingBits::length() const {
    return delegate_->length();
}

std::string SortingBits::to_string() const {
    return "SortingBits(" + delegate_->to_string() + ")";
}

SortingCodecReader::SortingCodecReader(std::shared_ptr<CodecReader> delegate,
                                       std::shared_ptr<const IndexSortDocMap> doc_map)
        : delegate_(std::move(delegate)), doc_map_(std::move(doc_map)) {}

Result<std::unique_ptr<SortingCodecReader>> SortingCodecReader::create(std::shared_ptr<CodecReader> delegate,
                                                                      std::shared_ptr<const IndexSortDocMap> doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the codec reader must not be null"));
    }
    if (doc_map == nullptr) {
        return err(LuceneError::illegal_argument("the doc map must not be null"));
    }
    return std::unique_ptr<SortingCodecReader>(new SortingCodecReader(std::move(delegate), std::move(doc_map)));
}

StoredFieldsReader *SortingCodecReader::get_fields_reader() {
    if (stored_fields_ == nullptr) {
        auto wrapped = SortingStoredFieldsReader::create(borrow(delegate_->get_fields_reader(), delegate_), doc_map_);
        if (wrapped) {
            stored_fields_ = std::move(wrapped.value());
        }
    }
    return stored_fields_.get();
}

TermVectorsReader *SortingCodecReader::get_term_vectors_reader() {
    if (term_vectors_ == nullptr) {
        auto wrapped =
                SortingTermVectorsReader::create(borrow(delegate_->get_term_vectors_reader(), delegate_), doc_map_);
        if (wrapped) {
            term_vectors_ = std::move(wrapped.value());
        }
    }
    return term_vectors_.get();
}

NormsProducer *SortingCodecReader::get_norms_reader() {
    if (norms_ == nullptr) {
        auto wrapped = SortingNormsProducer::create(borrow(delegate_->get_norms_reader(), delegate_), doc_map_);
        if (wrapped) {
            norms_ = std::move(wrapped.value());
        }
    }
    return norms_.get();
}

DocValuesProducer *SortingCodecReader::get_doc_values_reader() {
    if (doc_values_ == nullptr) {
        auto wrapped =
                SortingDocValuesProducer::create(borrow(delegate_->get_doc_values_reader(), delegate_), doc_map_);
        if (wrapped) {
            doc_values_ = std::move(wrapped.value());
        }
    }
    return doc_values_.get();
}

FieldsProducer *SortingCodecReader::get_postings_reader() {
    if (postings_ == nullptr) {
        FieldsProducer *delegate = delegate_->get_postings_reader();
        if (delegate == nullptr) {
            return nullptr;
        }
        auto infos = delegate_->field_infos();
        if (!infos) {
            return nullptr;
        }
        auto wrapped =
                SortingFieldsProducer::create(borrow(delegate, delegate_), borrow(infos.value(), delegate_), doc_map_);
        if (wrapped) {
            postings_ = std::move(wrapped.value());
        }
    }
    return postings_.get();
}

PointsReader *SortingCodecReader::get_points_reader() {
    if (points_ == nullptr) {
        auto wrapped = SortingPointsReader::create(borrow(delegate_->get_points_reader(), delegate_), doc_map_);
        if (wrapped) {
            points_ = std::move(wrapped.value());
        }
    }
    return points_.get();
}

Result<const FieldInfos *> SortingCodecReader::field_infos() const {
    return delegate_->field_infos();
}

Result<const Bits *> SortingCodecReader::live_docs() const {
    if (!live_docs_built_) {
        live_docs_built_ = true;
        auto live = delegate_->live_docs();
        if (!live) {
            return std::unexpected(std::move(live.error()));
        }
        if (live.value() != nullptr) {
            live_docs_ = SortingBits::create(borrow(live.value(), delegate_), doc_map_);
        }
    }
    return static_cast<const Bits *>(live_docs_.get());
}

Result<int32_t> SortingCodecReader::max_doc() const {
    return delegate_->max_doc();
}

Result<int32_t> SortingCodecReader::num_docs() const {
    return delegate_->num_docs();
}

std::string SortingCodecReader::to_string() const {
    return "SortingCodecReader(" + delegate_->to_string() + ")";
}

} // namespace lucene
