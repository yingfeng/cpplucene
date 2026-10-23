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

module lucene_core:sorting_rewriter.impl;

import :sorting_rewriter;

import std;

import :codec_formats;
import :codec_reader;
import :directory;
import :error;
import :index_sorter;
import :info_stream;
import :io_context;
import :segment_commit_info;
import :segment_info;
import :segment_info_format;
import :segment_merger;
import :segment_reader;
import :sort_field;
import :sorting_codec_reader;
import :types;
import :version;

namespace lucene {

namespace {

/// A file belongs to `segment` when it is `<segment>.<ext>` or
/// `<segment>_<something>.<ext>`.
[[nodiscard]] bool is_segment_file(std::string_view file, std::string_view segment) {
    if (file.size() <= segment.size() || file.substr(0, segment.size()) != segment) {
        return false;
    }
    const char separator = file[segment.size()];
    return separator == '.' || separator == '_';
}

} // namespace

Result<std::shared_ptr<SegmentReader>> rewrite_sorted_segment(std::span<CodecReader *const> readers,
                                                             const Sort &sort, Directory &directory,
                                                             std::string segment_name,
                                                             std::vector<uint8_t> segment_id) {
    if (readers.empty()) {
        return err(LuceneError::illegal_argument("at least one reader is required"));
    }
    if (segment_name.empty()) {
        return err(LuceneError::illegal_argument("the segment name must not be empty"));
    }
    if (segment_id.empty()) {
        return err(LuceneError::illegal_argument("the segment id must not be empty"));
    }

    // The wrappers have to outlive the merge, because the merge state keeps
    // borrowing the readers it was built from.
    std::vector<std::unique_ptr<SortingCodecReader>> wrappers;
    std::vector<CodecReader *> sources;
    wrappers.reserve(readers.size());
    sources.reserve(readers.size());

    for (CodecReader *reader : readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("the readers must not be null"));
        }
        auto deleted = reader->num_deleted_docs();
        if (!deleted) {
            return std::unexpected(std::move(deleted.error()));
        }
        if (deleted.value() != 0) {
            return err(LuceneError::illegal_argument("the segments to sort must not have deletions"));
        }
        auto max_doc = reader->max_doc();
        if (!max_doc) {
            return std::unexpected(std::move(max_doc.error()));
        }
        auto doc_map = sort.sort_documents(*reader, max_doc.value());
        if (!doc_map) {
            return std::unexpected(std::move(doc_map.error()));
        }
        if (!doc_map.value().has_value()) {
            // The documents are already in order; merge the segment as it is.
            sources.push_back(reader);
            continue;
        }

        // The caller owns the reader; the sorting reader only borrows it.
        std::shared_ptr<CodecReader> borrowed(reader, [](CodecReader *) {});
        auto wrapped = SortingCodecReader::create(
                std::move(borrowed),
                std::make_shared<const IndexSortDocMapImpl>(std::move(doc_map.value().value())));
        if (!wrapped) {
            return std::unexpected(std::move(wrapped.error()));
        }
        sources.push_back(wrapped.value().get());
        wrappers.push_back(std::move(wrapped.value()));
    }

    int32_t total_max_doc = 0;
    for (const CodecReader *reader : sources) {
        auto max_doc = reader->max_doc();
        if (!max_doc) {
            return std::unexpected(std::move(max_doc.error()));
        }
        total_max_doc += max_doc.value();
    }
    auto context = IOContext::with_merge(
            MergeInfo(total_max_doc, 0, false, static_cast<int32_t>(sources.size())));
    if (!context) {
        return std::unexpected(std::move(context.error()));
    }

    SegmentInfo info;
    info.name = std::move(segment_name);
    info.id = std::move(segment_id);
    info.codec_name = std::string(CodecFormats::default_codec().name());
    info.version = LUCENE_CURRENT_VERSION;
    info.min_version = LUCENE_CURRENT_VERSION;
    info.diagnostics.emplace("source", "sort");
    info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());
    // `info.sort` is deliberately left unset here: the documents are already in
    // the sorted order, so a merger that saw the sort would merge-sort the inputs
    // a second time.  The sort is recorded once the merge is done.

    {
        auto merger =
                SegmentMerger::create(sources, info, std::make_shared<InfoStreamEnum>(), directory, context.value());
        if (!merger) {
            return std::unexpected(std::move(merger.error()));
        }
        if (!merger.value()->should_merge()) {
            return err(LuceneError::illegal_state("sorting the segments would produce an empty segment"));
        }
        auto merged = merger.value()->merge();
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
    }

    // The inputs were reordered before the merge, so the result is sorted: record
    // the sort for the readers that want to exploit it.
    info.sort = sort;

    // The merger's file list does not contain every file it wrote (it is the
    // caller's job to collect them), and the commit that registers the segment
    // needs the full list, otherwise the files look like orphans.
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
    if (auto set = info.set_files(std::move(files)); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }

    if (auto written = CodecFormats::default_codec().segment_info_format().write(directory, info,
                                                                                io_context_default());
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    SegmentCommitInfo commit;
    commit.info = std::move(info);
    return SegmentReader::open(directory, commit, LUCENE_CURRENT_VERSION.major, io_context_default());
}

} // namespace lucene
