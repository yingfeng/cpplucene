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

export module lucene_core:index_writer_config;

import :types;
import std;

import :analyzer;
import :codec;
import :merge_policy;
import :merge_scheduler;
import :similarity;

namespace lucene {

/// The knobs of an [`IndexWriter`](:index_writer).
///
/// Port of `LiveIndexWriterConfig` (`index/index_writer_config.rs`), reduced to
/// the settings the port actually honours.  The reference implementation's config is a large
/// mutable object shared through an `Arc`; here it is a plain value that the
/// writer copies when it is created.
///
/// **Not modelled yet** (they belong to subsystems the port does not have):
/// index sorting, deletes policy (soft deletes keep their own field), RAM
/// accounting for the `Accountable` tree (which is why flushing is driven by
/// [`max_buffered_docs`] only), commit data and event listeners.
export class IndexWriterConfig {
public:
    /// A config that indexes with `analyzer`.  The codec defaults to
    /// `Lucene101`, the similarity to BM25.
    explicit IndexWriterConfig(Analyzer &analyzer);

    IndexWriterConfig(const IndexWriterConfig &) = default;
    IndexWriterConfig &operator=(const IndexWriterConfig &) = default;
    IndexWriterConfig(IndexWriterConfig &&) noexcept = default;
    IndexWriterConfig &operator=(IndexWriterConfig &&) noexcept = default;
    ~IndexWriterConfig() = default;

    [[nodiscard]] Analyzer &analyzer() const noexcept { return *analyzer_; }

    /// The codec the writer creates segments with.
    [[nodiscard]] const Codec &codec() const noexcept { return *codec_; }
    void set_codec(std::shared_ptr<const Codec> codec);

    /// The similarity used to compute the norms at index time.
    [[nodiscard]] std::shared_ptr<const Similarity> similarity() const noexcept { return similarity_; }
    void set_similarity(std::shared_ptr<const Similarity> similarity);

    /// Flush the buffered documents once a segment holds this many.
    ///
    /// `-1` (the default) means "only flush on `commit`, `flush` or close".
    [[nodiscard]] int32_t max_buffered_docs() const noexcept { return max_buffered_docs_; }
    void set_max_buffered_docs(int32_t max_buffered_docs) noexcept { max_buffered_docs_ = max_buffered_docs; }

    /// The policy that decides which segments are merged.
    ///
    /// The default is `TieredMergePolicy`, like the reference implementation.
    [[nodiscard]] const std::shared_ptr<MergePolicy> &merge_policy() const noexcept { return merge_policy_; }
    void set_merge_policy(std::shared_ptr<MergePolicy> merge_policy);

    /// The scheduler that executes the merges the policy asks for.
    ///
    /// **Deviation:** the default is `SerialMergeScheduler`, because the writer
    /// is single threaded; the reference implementation defaults to
    /// `ConcurrentMergeScheduler`.
    [[nodiscard]] const std::shared_ptr<MergeScheduler> &merge_scheduler() const noexcept { return merge_scheduler_; }
    void set_merge_scheduler(std::shared_ptr<MergeScheduler> merge_scheduler);

    /// Number of index-created versions recorded in the segments.
    ///
    /// Kept so a caller can pin the "created version" of the index; the port
    /// defaults to the version this build writes.
    [[nodiscard]] int32_t index_created_version_major() const noexcept {
        return index_created_version_major_;
    }
    void set_index_created_version_major(int32_t major) noexcept { index_created_version_major_ = major; }

private:
    Analyzer *analyzer_{nullptr};
    std::shared_ptr<const Codec> codec_;
    std::shared_ptr<const Similarity> similarity_;
    std::shared_ptr<MergePolicy> merge_policy_;
    std::shared_ptr<MergeScheduler> merge_scheduler_;
    int32_t max_buffered_docs_{-1};
    int32_t index_created_version_major_{0};
};

} // namespace lucene
