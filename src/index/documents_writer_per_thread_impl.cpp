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

module lucene_core:documents_writer_per_thread.impl;

import :documents_writer_per_thread;

import :analyzer;
import :bits;
import :byte_buffers_directory;
import :codec;
import :directory;
import :document;
import :error;
import :field_infos;
import :field_infos_builder;
import :fixed_bit_set;
import :info_stream;
import :index_writer_config;
import :indexing_chain;
import :io_context;
import :live_docs_format;
import :segment_commit_info;
import :segment_info;
import :segment_info_format;
import :segment_reader;
import :segment_write_state;
import :similarity;
import :sort_field;
import :sorting_rewriter;
import :types;
import :version;

import std;

namespace lucene {

/// A file belongs to `segment` when it is `<segment>.<ext>` or
/// `<segment>_<something>.<ext>`.
[[nodiscard]] bool is_segment_file(std::string_view file, std::string_view segment) {
    if (file.size() <= segment.size() || file.substr(0, segment.size()) != segment) {
        return false;
    }
    const char separator = file[segment.size()];
    return separator == '.' || separator == '_';
}

struct DocumentsWriterPerThread::Impl {
    Impl(Directory &directory, const Codec &codec, SegmentInfo segment_info,
         int32_t index_created_version_major, Analyzer &analyzer,
         std::shared_ptr<const Similarity> similarity, std::optional<Sort> sort)
        : directory_(directory),
          codec_(codec),
          segment_info_(std::move(segment_info)),
          analyzer_(analyzer),
          similarity_(std::move(similarity)),
          index_created_version_major_(index_created_version_major),
          sort_(std::move(sort)) {
        if (sort_.has_value()) {
            scratch_directory_ = std::make_unique<ByteBuffersDirectory>();
        }
        // A sorted segment is first written unsorted into the scratch directory,
        // so the chain has to put every file there.
        Directory &target = scratch_directory_ == nullptr ? directory_ : *scratch_directory_;
        chain_ = std::make_unique<IndexingChain>(target, codec_, segment_info_,
                                                 index_created_version_major_, analyzer_, similarity_);
    }

    [[nodiscard]] VoidResult add_document(const Document &document) {
        if (aborted_) {
            return err(LuceneError::illegal_state("this DocumentsWriterPerThread is aborted"));
        }
        if (flushed_) {
            return err(LuceneError::illegal_state("this DocumentsWriterPerThread was already flushed"));
        }
        const int32_t doc_id = num_docs_in_ram_;
        ++num_docs_in_ram_;
        auto processed = chain_->process_document(doc_id, document, field_infos_);
        if (!processed.has_value()) {
            // The id is spent: keep the hole and record it as deleted, so the
            // segment stays consistent without aborting the whole buffer.
            deleted_doc_ids_.push_back(doc_id);
            return processed;
        }
        return {};
    }

    [[nodiscard]] Result<std::optional<SegmentCommitInfo>> flush() {
        if (aborted_) {
            return err(LuceneError::illegal_state("cannot flush an aborted DocumentsWriterPerThread"));
        }
        if (flushed_) {
            return err(LuceneError::illegal_state("this DocumentsWriterPerThread was already flushed"));
        }
        flushed_ = true;
        if (num_docs_in_ram_ == 0) {
            return std::optional<SegmentCommitInfo>{};
        }
        if (sort_.has_value()) {
            return flush_sorted();
        }

        const IOContext &context = io_context_default();
        auto infos = write_segment_files(directory_);
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }

        SegmentCommitInfo commit;
        commit.info = segment_info_;

        // Documents whose indexing failed are holes in the segment: they get a
        // live docs file, exactly like the reference implementation does for non-aborting errors.
        if (!deleted_doc_ids_.empty()) {
            FixedBitSet live_docs(static_cast<size_t>(segment_info_.max_doc));
            if (auto set = live_docs.set_with_range(0, static_cast<size_t>(segment_info_.max_doc));
                !set.has_value()) {
                return std::unexpected(std::move(set.error()));
            }
            for (const int32_t doc_id : deleted_doc_ids_) {
                live_docs.clear(static_cast<size_t>(doc_id));
            }
            const int32_t deleted = static_cast<int32_t>(deleted_doc_ids_.size());
            if (auto written = codec_.live_docs_format().write_live_docs(live_docs, directory_, commit, deleted,
                                                                        context);
                !written.has_value()) {
                return std::unexpected(std::move(written.error()));
            }
            commit.del_gen = commit.next_del_gen();
            commit.del_count = deleted;
        }

        // The file list goes into both `.si` and `segments_N`, so it has to be
        // collected before the metadata is written.
        if (auto collected = collect_files(directory_, commit.info); !collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
        if (auto written = codec_.segment_info_format().write(directory_, commit.info, context);
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        return std::optional<SegmentCommitInfo>(std::move(commit));
    }

    /// Writes every file of the segment into `target` and returns the finished
    /// field infos of the segment.
    [[nodiscard]] Result<std::shared_ptr<const FieldInfos>> write_segment_files(Directory &target) {
        segment_info_.max_doc = num_docs_in_ram_;

        auto built = field_infos_.finish();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        auto infos = std::make_shared<const FieldInfos>(std::move(built.value()));
        SegmentWriteState state(get_default_info_stream(), target, infos, io_context_default(), "");
        if (auto written = chain_->flush(state); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        return infos;
    }

    /// Writes the segment unsorted into the scratch directory and then rewrites it
    /// in sorted order into the real one.
    ///
    /// The scratch segment must have no deletions: a document that failed to index
    /// has no place in the sorted order that keeps the document count consistent,
    /// so such a flush fails instead.
    [[nodiscard]] Result<std::optional<SegmentCommitInfo>> flush_sorted() {
        const IOContext &context = io_context_default();
        Directory &scratch = *scratch_directory_;

        auto infos = write_segment_files(scratch);
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        if (!deleted_doc_ids_.empty()) {
            return err(LuceneError::unsupported_operation(
                    "a sorted segment cannot be written when a document failed to index"));
        }

        SegmentInfo scratch_info = segment_info_;
        if (auto collected = collect_files(scratch, scratch_info); !collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
        if (auto written = codec_.segment_info_format().write(scratch, scratch_info, context); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }

        SegmentCommitInfo scratch_commit;
        scratch_commit.info = std::move(scratch_info);
        auto opened = SegmentReader::open(scratch, scratch_commit, LUCENE_CURRENT_VERSION.major, context);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }

        CodecReader *sources[] = {opened.value().get()};
        auto sorted =
                rewrite_sorted_segment(sources, sort_.value(), directory_, segment_info_.name, segment_info_.id);
        if (!sorted.has_value()) {
            return std::unexpected(std::move(sorted.error()));
        }

        SegmentCommitInfo commit;
        commit.info = sorted.value()->segment_info().info;
        return std::optional<SegmentCommitInfo>(std::move(commit));
    }

    [[nodiscard]] VoidResult collect_files(Directory &directory, SegmentInfo &info) {
        auto names = directory.list_all();
        if (!names.has_value()) {
            return std::unexpected(std::move(names.error()));
        }
        std::set<std::string> files;
        for (const std::string &name : names.value()) {
            if (is_segment_file(name, info.name)) {
                files.insert(name);
            }
        }
        return info.set_files(std::move(files));
    }

    [[nodiscard]] VoidResult abort() {
        if (aborted_) {
            return {};
        }
        aborted_ = true;
        return chain_->abort();
    }

    Directory &directory_;
    const Codec &codec_;
    SegmentInfo segment_info_;
    Analyzer &analyzer_;
    std::shared_ptr<const Similarity> similarity_;
    int32_t index_created_version_major_;
    std::unique_ptr<IndexingChain> chain_;
    /// Where a sorted segment is written before it is rewritten in sorted order.
    std::unique_ptr<ByteBuffersDirectory> scratch_directory_;
    std::optional<Sort> sort_;
    FieldInfosBuilder field_infos_;
    int32_t num_docs_in_ram_{0};
    std::vector<int32_t> deleted_doc_ids_;
    bool aborted_{false};
    bool flushed_{false};
};

DocumentsWriterPerThread::DocumentsWriterPerThread(Directory &directory, const Codec &codec,
                                                   SegmentInfo segment_info,
                                                   int32_t index_created_version_major, Analyzer &analyzer,
                                                   std::shared_ptr<const Similarity> similarity,
                                                   std::optional<Sort> sort)
    : impl_(std::make_unique<Impl>(directory, codec, std::move(segment_info), index_created_version_major,
                                   analyzer, std::move(similarity), std::move(sort))) {}

DocumentsWriterPerThread::~DocumentsWriterPerThread() = default;

VoidResult DocumentsWriterPerThread::add_document(const Document &document) {
    return impl_->add_document(document);
}

Result<std::optional<SegmentCommitInfo>> DocumentsWriterPerThread::flush() { return impl_->flush(); }

VoidResult DocumentsWriterPerThread::abort() { return impl_->abort(); }

int32_t DocumentsWriterPerThread::num_docs_in_ram() const noexcept { return impl_->num_docs_in_ram_; }

int32_t DocumentsWriterPerThread::num_deleted_doc_ids() const noexcept {
    return static_cast<int32_t>(impl_->deleted_doc_ids_.size());
}

const SegmentInfo &DocumentsWriterPerThread::segment_info() const noexcept { return impl_->segment_info_; }

bool DocumentsWriterPerThread::is_aborted() const noexcept { return impl_->aborted_; }

bool DocumentsWriterPerThread::has_flushed() const noexcept { return impl_->flushed_; }

} // namespace lucene
