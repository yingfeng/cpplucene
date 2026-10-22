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

module lucene_core:documents_writer.impl;

import :documents_writer;

import :codec;
import :directory;
import :documents_writer_per_thread;
import :document;
import :error;
import :index_writer_config;
import :segment_commit_info;
import :segment_info;
import :string_helper;
import :term;
import :types;
import :version;

import std;

namespace lucene {

namespace {

/// The files of a segment, so an aborted writer can remove what it wrote.
std::set<std::string> files_of(const std::vector<SegmentCommitInfo> &segments) {
    std::set<std::string> files;
    for (const auto &segment : segments) {
        files.insert(segment.info.files.begin(), segment.info.files.end());
    }
    return files;
}

} // namespace

struct DocumentsWriter::Impl {
    Impl(Directory &directory, const IndexWriterConfig &config, int64_t next_segment_counter)
        : directory_(directory),
          codec_(config.codec()),
          analyzer_(config.analyzer()),
          similarity_(config.similarity()),
          index_created_version_major_(config.index_created_version_major()),
          max_buffered_docs_(config.max_buffered_docs()),
          next_segment_counter_(next_segment_counter) {}

    [[nodiscard]] SegmentInfo new_segment_info() {
        SegmentInfo info;
        info.name = "_" + std::to_string(next_segment_counter_++);
        const auto random = StringHelper::random_id();
        info.id.assign(random.begin(), random.end());
        info.version = LUCENE_CURRENT_VERSION;
        info.min_version = LUCENE_CURRENT_VERSION;
        info.codec_name = std::string(codec_.name());
        info.diagnostics.emplace("source", "flush");
        info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());
        return info;
    }

    [[nodiscard]] VoidResult ensure_buffer() {
        if (current_ != nullptr) {
            return {};
        }
        current_ = std::make_unique<DocumentsWriterPerThread>(
                directory_, codec_, new_segment_info(), index_created_version_major_, analyzer_, similarity_);
        return {};
    }

    /// Flushes the active buffer, if it holds any document.
    [[nodiscard]] VoidResult flush_buffer() {
        if (current_ == nullptr) {
            return {};
        }
        if (current_->num_docs_in_ram() == 0) {
            current_.reset();
            return {};
        }
        auto flushed = current_->flush();
        // The buffer is spent whether or not the flush worked: the reference implementation drops the
        // DWPT from its pool in both cases.
        current_.reset();
        if (!flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        if (flushed.value().has_value()) {
            flushed_segments_.push_back(std::move(flushed.value().value()));
        }
        return {};
    }

    [[nodiscard]] VoidResult add_document(const Document &document) {
        if (auto ensured = ensure_buffer(); !ensured.has_value()) {
            return ensured;
        }
        if (max_buffered_docs_ > 0 && current_->num_docs_in_ram() >= max_buffered_docs_) {
            if (auto flushed = flush_buffer(); !flushed.has_value()) {
                return flushed;
            }
            if (auto ensured = ensure_buffer(); !ensured.has_value()) {
                return ensured;
            }
        }
        return current_->add_document(document);
    }

    [[nodiscard]] Result<std::vector<SegmentCommitInfo>> flush_all() {
        if (auto flushed = flush_buffer(); !flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        return take_flushed_segments();
    }

    [[nodiscard]] std::vector<SegmentCommitInfo> take_flushed_segments() {
        std::vector<SegmentCommitInfo> segments = std::move(flushed_segments_);
        flushed_segments_.clear();
        return segments;
    }

    [[nodiscard]] VoidResult rollback() {
        if (current_ != nullptr) {
            (void)current_->abort();
            current_.reset();
        }
        // Remove the files of the segments that will never be committed.
        for (const std::string &file : files_of(flushed_segments_)) {
            (void)directory_.delete_file(file);
        }
        flushed_segments_.clear();
        delete_terms_.clear();
        delete_all_ = false;
        return {};
    }

    Directory &directory_;
    const Codec &codec_;
    Analyzer &analyzer_;
    std::shared_ptr<const Similarity> similarity_;
    int32_t index_created_version_major_;
    int32_t max_buffered_docs_;
    int64_t next_segment_counter_;

    std::unique_ptr<DocumentsWriterPerThread> current_;
    std::vector<SegmentCommitInfo> flushed_segments_;
    std::vector<std::pair<Term, int64_t>> delete_terms_;
    bool delete_all_{false};
};

DocumentsWriter::DocumentsWriter(Directory &directory, const IndexWriterConfig &config,
                                 int64_t next_segment_counter)
    : impl_(std::make_unique<Impl>(directory, config, next_segment_counter)) {}

DocumentsWriter::~DocumentsWriter() = default;

VoidResult DocumentsWriter::add_document(const Document &document) { return impl_->add_document(document); }

VoidResult DocumentsWriter::delete_documents(const Term &term, int64_t doc_upto) {
    impl_->delete_terms_.emplace_back(term, doc_upto);
    return {};
}


VoidResult DocumentsWriter::delete_all() {
    impl_->delete_all_ = true;
    return {};
}

Result<std::vector<SegmentCommitInfo>> DocumentsWriter::flush_all() { return impl_->flush_all(); }

std::vector<SegmentCommitInfo> DocumentsWriter::take_flushed_segments() {
    return impl_->take_flushed_segments();
}

int32_t DocumentsWriter::pending_doc_count() const noexcept {
    return impl_->current_ == nullptr ? 0 : impl_->current_->num_docs_in_ram();
}

const std::vector<std::pair<Term, int64_t>> &DocumentsWriter::buffered_delete_terms() const noexcept {
    return impl_->delete_terms_;
}

bool DocumentsWriter::delete_all_pending() const noexcept { return impl_->delete_all_; }

void DocumentsWriter::clear_delete_buffer() noexcept {
    impl_->delete_terms_.clear();
    impl_->delete_all_ = false;
}

VoidResult DocumentsWriter::rollback() { return impl_->rollback(); }

int64_t DocumentsWriter::next_segment_counter() const noexcept { return impl_->next_segment_counter_; }

void DocumentsWriter::set_next_segment_counter(int64_t counter) noexcept {
    impl_->next_segment_counter_ = counter;
}

} // namespace lucene
