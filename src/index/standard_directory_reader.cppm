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

export module lucene_core:standard_directory_reader;

import :types;
import std;

import :base_composite_reader;
import :directory;
import :error;
import :identity;
import :index_reader;
import :io_context;
import :leaf_reader_context;
import :segment_infos;
import :segment_reader;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// A composite reader over the segments of one commit.
///
/// Port of `StandardDirectoryReader` (`index/standard_directory_reader.rs`),
/// minus the writer-ownership half: this port has no `IndexWriter` yet, so a
/// reader is always opened from a committed `segments_N` rather than from a
/// writer's in-memory state.
///
/// **Deviations**:
///  * NRT and `open_if_changed` against a writer are not ported; [`open_if_changed`]
///    here re-reads `segments_N` from the directory and returns null when the
///    generation did not move;
///  * sub-readers are not sorted (`MultiReader`'s `sub_reader_sorter`), and
///    `MultiReader` itself is not ported yet;
///  * the cache helper / closed-listener protocol is not ported.
export class DirectoryReader final : public IndexReader {
public:
    ~DirectoryReader() override = default;

    /// Opens the newest commit in `dir`.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open(Directory &dir, const IOContext &context = io_context_default());

    /// Opens the segments listed by `infos`; `infos` is kept for
    /// [`segment_infos`] and [`is_current`].
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_infos(Directory &dir, SegmentInfos infos, const IOContext &context = io_context_default());

    /// Reopens the index when `dir` has a newer commit; null otherwise.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> open_if_changed() const;

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return base_.max_doc(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return base_.num_docs(); }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override { return base_.doc_freq(term); }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override { return base_.total_term_freq(term); }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override { return base_.sum_doc_freq(field); }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override { return base_.doc_count(field); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override {
        return base_.sum_total_term_freq(field);
    }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override;
    [[nodiscard]] const Identity &identity() const override { return identity_; }

    // ---- commit accessors ----

    /// The commit this reader was opened from.
    [[nodiscard]] const SegmentInfos &segment_infos() const noexcept { return infos_; }
    /// Index version of the commit (bumped by every commit).
    [[nodiscard]] int64_t version() const noexcept { return infos_.version; }
    /// The segment readers, in document order.
    [[nodiscard]] const std::vector<std::shared_ptr<SegmentReader>> &segment_readers() const noexcept {
        return segment_readers_;
    }
    /// `true` when `dir` still holds the commit this reader was opened from.
    [[nodiscard]] Result<bool> is_current() const;

private:
    DirectoryReader() = default;

    Directory *dir_{nullptr};
    SegmentInfos infos_;
    std::vector<std::shared_ptr<SegmentReader>> segment_readers_;
    CompositeReaderBase base_;
    mutable std::vector<LeafReaderContext> leaves_;
    Identity identity_{};
};

} // namespace lucene
